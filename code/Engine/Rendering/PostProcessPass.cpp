#include "PostProcessPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ICommandList.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/Loader/LUTLoader.h"
#include "Engine/SceneLightSettings.h"
#include <cstring>

using namespace RHI;

struct FullscreenVertex {
    float x, y;       // Position (NDC space)
    float u, v;       // UV
};

// Constant buffer for post-processing (must be 16-byte aligned)
struct CB_PostProcess {
    float exposure;
    int useExposureBuffer;
    float bloomIntensity;
    float _pad0;

    // Color Grading parameters
    DirectX::XMFLOAT3 lift;
    float saturation;

    DirectX::XMFLOAT3 gamma;
    float contrast;

    DirectX::XMFLOAT3 gain;
    float temperature;

    float lutContribution;
    int colorGradingEnabled;
    float _pad1[2];
};

static constexpr int kLUTSize = 32;  // 32x32x32 LUT

bool CPostProcessPass::Initialize() {
    if (m_initialized) return true;

    createFullscreenQuad();
    createShaders();
    createPipelineState();
    createNeutralLUT();

    // Create sampler
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Clamp;
    samplerDesc.addressV = ETextureAddressMode::Clamp;
    samplerDesc.addressW = ETextureAddressMode::Clamp;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    m_sampler.reset(ctx->CreateSampler(samplerDesc));

    // Create constant buffer
    BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_PostProcess);
    cbDesc.usage = EBufferUsage::Constant;
    cbDesc.cpuAccess = ECPUAccess::Write;
    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));

    // Create dummy exposure buffer for t3 slot when no real exposure buffer is provided
    // DX12 GPU validation requires all declared SRVs to have valid resources bound
    BufferDesc dummyDesc;
    dummyDesc.size = sizeof(float);
    dummyDesc.usage = EBufferUsage::Structured;
    dummyDesc.cpuAccess = ECPUAccess::None;
    dummyDesc.structureByteStride = sizeof(float);
    float dummyExposure = 1.0f;
    m_dummyExposureBuffer.reset(ctx->CreateBuffer(dummyDesc, &dummyExposure));

    initDescriptorSets();

    m_initialized = true;
    return true;
}

void CPostProcessPass::Shutdown() {
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_vertexBuffer.reset();
    m_constantBuffer.reset();
    m_dummyExposureBuffer.reset();
    m_sampler.reset();
    m_neutralLUT.reset();
    m_customLUT.reset();
    m_cachedLUTPath.clear();

    // Cleanup DS resources
    m_vs_ds.reset();
    m_ps_ds.reset();
    m_pso_ds.reset();

    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perPassSet) {
            ctx->FreeDescriptorSet(m_perPassSet);
            m_perPassSet = nullptr;
        }
        if (m_perPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_perPassLayout);
            m_perPassLayout = nullptr;
        }
    }

    m_initialized = false;
}

void CPostProcessPass::Render(ITexture* hdrInput,
                              ITexture* bloomTexture,
                              ITexture* ldrOutput,
                              uint32_t width, uint32_t height,
                              float exposure,
                              IBuffer* exposureBuffer,
                              float bloomIntensity,
                              const SColorGradingSettings* colorGrading,
                              bool colorGradingEnabled) {
    if (!m_initialized || !hdrInput || !ldrOutput || !m_pso) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // CRITICAL: Unbind render targets before using hdrInput as SRV
    // Otherwise D3D11 will null out the SRV to avoid RTV/SRV hazard
    cmdList->UnbindRenderTargets();

    // Handle LUT loading if color grading is enabled with custom LUT
    if (colorGradingEnabled && colorGrading &&
        colorGrading->preset == EColorGradingPreset::Custom &&
        !colorGrading->lutPath.empty() &&
        colorGrading->lutPath != m_cachedLUTPath) {
        loadLUT(colorGrading->lutPath);
    }

    // Update constant buffer
    CB_PostProcess cb;
    cb.exposure = exposure;  // Fallback value, may be overridden by GPU buffer
    cb.useExposureBuffer = exposureBuffer ? 1 : 0;
    cb.bloomIntensity = bloomTexture ? bloomIntensity : 0.0f;
    cb._pad0 = 0.0f;

    // Color grading parameters
    if (colorGradingEnabled && colorGrading) {
        cb.lift = colorGrading->lift;
        cb.saturation = colorGrading->saturation;
        cb.gamma = colorGrading->gamma;
        cb.contrast = colorGrading->contrast;
        cb.gain = colorGrading->gain;
        cb.temperature = colorGrading->temperature;
        cb.lutContribution = (colorGrading->preset == EColorGradingPreset::Custom && m_customLUT) ? 1.0f : 0.0f;
        cb.colorGradingEnabled = 1;
    } else {
        cb.lift = cb.gamma = cb.gain = {0.0f, 0.0f, 0.0f};
        cb.saturation = cb.contrast = cb.temperature = 0.0f;
        cb.lutContribution = 0.0f;
        cb.colorGradingEnabled = 0;
    }
    cb._pad1[0] = cb._pad1[1] = 0.0f;

    // Set render target (no depth)
    ITexture* renderTargets[] = { ldrOutput };
    cmdList->SetRenderTargets(1, renderTargets, nullptr);

    // Set viewport and scissor rect (DX12 requires both)
    cmdList->SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    // Set pipeline state (includes rasterizer, depth stencil, blend states)
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);

    // Set vertex buffer
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(FullscreenVertex), 0);

    // Set constant buffer and resources
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_PostProcess));
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
    if (bloomTexture) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, bloomTexture);
    }

    // Bind LUT texture (use custom if available, otherwise neutral)
    ITexture* lutTexture = (cb.lutContribution > 0.0f && m_customLUT) ? m_customLUT.get() : m_neutralLUT.get();
    if (lutTexture) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 2, lutTexture);
    }

    // Bind exposure buffer for GPU-only auto exposure path
    // Always bind a valid buffer to t3 to satisfy DX12 GPU validation
    if (exposureBuffer) {
        cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 3, exposureBuffer);
    } else {
        cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 3, m_dummyExposureBuffer.get());
    }

    cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

    // Draw fullscreen quad
    cmdList->Draw(4, 0);

    // Unbind resources to prevent hazards
    if (exposureBuffer) {
        cmdList->UnbindShaderResources(EShaderStage::Pixel, 3, 1);
    }
    cmdList->SetRenderTargets(0, nullptr, nullptr);
}

void CPostProcessPass::createFullscreenQuad() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Fullscreen quad in NDC space (triangle strip)
    FullscreenVertex vertices[] = {
        { -1.0f,  1.0f,  0.0f, 0.0f },  // Top-left
        {  1.0f,  1.0f,  1.0f, 0.0f },  // Top-right
        { -1.0f, -1.0f,  0.0f, 1.0f },  // Bottom-left
        {  1.0f, -1.0f,  1.0f, 1.0f }   // Bottom-right
    };

    BufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.usage = EBufferUsage::Vertex;
    vbDesc.cpuAccess = ECPUAccess::None;

    m_vertexBuffer.reset(ctx->CreateBuffer(vbDesc, vertices));
}

void CPostProcessPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Vertex shader: Pass-through with UV
    const char* vsCode = R"(
        struct VSIn {
            float2 pos : POSITION;
            float2 uv : TEXCOORD0;
        };
        struct VSOut {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };
        VSOut main(VSIn input) {
            VSOut output;
            output.pos = float4(input.pos, 0.0, 1.0);
            output.uv = input.uv;
            return output;
        }
    )";

    // Pixel shader: Tone mapping + Color Grading + Gamma correction
    const char* psCode = R"(
        Texture2D hdrTexture : register(t0);
        Texture2D bloomTexture : register(t1);
        Texture3D lutTexture : register(t2);
        StructuredBuffer<float> exposureBuffer : register(t3);
        SamplerState samp : register(s0);

        cbuffer CB_PostProcess : register(b0) {
            float gExposure;
            int gUseExposureBuffer;
            float gBloomIntensity;
            float _pad0;

            float3 gLift;
            float gSaturation;

            float3 gGamma;
            float gContrast;

            float3 gGain;
            float gTemperature;

            float gLutContribution;
            int gColorGradingEnabled;
            float2 _pad1;
        };

        struct PSIn {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };

        // ACES Filmic Tone Mapping
        float3 ACESFilm(float3 x) {
            float a = 2.51;
            float b = 0.03;
            float c = 2.43;
            float d = 0.59;
            float e = 0.14;
            return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
        }

        // Lift/Gamma/Gain (ASC CDL style)
        float3 ApplyLiftGammaGain(float3 color, float3 lift, float3 gamma, float3 gain) {
            // Lift: offset in shadows (add)
            color = color + lift * 0.1;

            // Gamma: power curve in midtones
            // Convert -1 to +1 range to 0.5 to 2.0 gamma adjustment
            float3 gammaAdj = 1.0 / (1.0 + gamma);
            color = pow(max(color, 0.0001), gammaAdj);

            // Gain: multiply in highlights
            color = color * (1.0 + gain * 0.5);

            return color;
        }

        // Saturation adjustment
        float3 ApplySaturation(float3 color, float saturation) {
            float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
            return lerp(float3(luma, luma, luma), color, 1.0 + saturation);
        }

        // Contrast adjustment (around 0.5 pivot)
        float3 ApplyContrast(float3 color, float contrast) {
            return (color - 0.5) * (1.0 + contrast) + 0.5;
        }

        // Temperature adjustment (blue-orange axis)
        float3 ApplyTemperature(float3 color, float temperature) {
            // Warm: shift toward orange, Cool: shift toward blue
            float3 warm = float3(1.05, 1.0, 0.95);
            float3 cool = float3(0.95, 1.0, 1.05);
            float3 tint = lerp(cool, warm, temperature * 0.5 + 0.5);
            return color * tint;
        }

        // 3D LUT sampling
        float3 ApplyLUT(float3 color, float contribution) {
            if (contribution <= 0.0) return color;

            // LUT is 32x32x32, map [0,1] color to proper UV coordinates
            // Offset by half texel to sample center of texels
            float lutSize = 32.0;
            float3 scale = (lutSize - 1.0) / lutSize;
            float3 offset = 0.5 / lutSize;
            float3 uvw = saturate(color) * scale + offset;

            float3 lutColor = lutTexture.Sample(samp, uvw).rgb;
            return lerp(color, lutColor, contribution);
        }

        float4 main(PSIn input) : SV_Target {
            // Sample HDR input (linear space)
            float3 hdrColor = hdrTexture.Sample(samp, input.uv).rgb;

            // Add bloom contribution (bloom texture is half res, bilinear upsample)
            if (gBloomIntensity > 0.0) {
                float3 bloom = bloomTexture.Sample(samp, input.uv).rgb;
                hdrColor += bloom * gBloomIntensity;
            }

            // Apply exposure (from GPU buffer if available, otherwise from constant)
            float exposure = gExposure;
            if (gUseExposureBuffer) {
                exposure = exposureBuffer[0];  // [0] = current exposure
            }
            hdrColor *= exposure;

            // Tone mapping: HDR -> LDR [0, 1] (still linear space)
            float3 ldrColor = ACESFilm(hdrColor);

            // === COLOR GRADING (LDR space, after tone mapping) ===
            if (gColorGradingEnabled) {
                // 1. Lift/Gamma/Gain
                ldrColor = ApplyLiftGammaGain(ldrColor, gLift, gGamma, gGain);

                // 2. Saturation
                ldrColor = ApplySaturation(ldrColor, gSaturation);

                // 3. Contrast
                ldrColor = ApplyContrast(ldrColor, gContrast);

                // 4. Temperature
                ldrColor = ApplyTemperature(ldrColor, gTemperature);

                // 5. 3D LUT (final creative look)
                ldrColor = ApplyLUT(ldrColor, gLutContribution);

                // Clamp to valid range
                ldrColor = saturate(ldrColor);
            }

            // Gamma correction: Linear -> sRGB
            // Since output RT is UNORM_SRGB, GPU will do this automatically

            return float4(ldrColor, 1.0);
        }
    )";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile Vertex Shader
    SCompiledShader vsCompiled = CompileShaderFromSource(vsCode, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("PostProcess VS compilation error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    // Compile Pixel Shader
    SCompiledShader psCompiled = CompileShaderFromSource(psCode, "main", "ps_5_0", nullptr, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("PostProcess PS compilation error: %s", psCompiled.errorMessage.c_str());
        return;
    }

    // Create shader objects using RHI
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    m_vs.reset(ctx->CreateShader(vsDesc));

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    m_ps.reset(ctx->CreateShader(psDesc));
}

void CPostProcessPass::createPipelineState() {
    if (!m_vs || !m_ps) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();

    // Input layout: POSITION + TEXCOORD
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 8, 0 }
    };

    // Rasterizer state: no culling, no depth clip
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.depthClipEnable = false;

    // Depth stencil state: no depth test
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.depthStencil.depthWriteEnable = false;

    // Blend state: no blending
    psoDesc.blend.blendEnable = false;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;

    // Render target format: LDR uses R8G8B8A8_UNORM_SRGB for gamma-correct output
    psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };

    // No depth stencil for post-processing
    psoDesc.depthStencilFormat = ETextureFormat::Unknown;
    psoDesc.debugName = "PostProcess_ToneMap_ColorGrading_PSO";

    m_pso.reset(ctx->CreatePipelineState(psoDesc));
}

void CPostProcessPass::createNeutralLUT() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Generate identity LUT data
    SLUTData lutData;
    GenerateIdentityLUT(kLUTSize, lutData);

    // Convert to RGBA format (add alpha channel)
    std::vector<float> rgbaData(kLUTSize * kLUTSize * kLUTSize * 4);
    for (uint32_t i = 0; i < kLUTSize * kLUTSize * kLUTSize; ++i) {
        rgbaData[i * 4 + 0] = lutData.data[i * 3 + 0];
        rgbaData[i * 4 + 1] = lutData.data[i * 3 + 1];
        rgbaData[i * 4 + 2] = lutData.data[i * 3 + 2];
        rgbaData[i * 4 + 3] = 1.0f;
    }

    // Create 3D texture
    TextureDesc desc = TextureDesc::Texture3D(
        kLUTSize, kLUTSize, kLUTSize,
        ETextureFormat::R32G32B32A32_FLOAT,
        ETextureUsage::ShaderResource
    );
    desc.debugName = "NeutralLUT";

    m_neutralLUT.reset(ctx->CreateTexture(desc, rgbaData.data()));

    if (m_neutralLUT) {
        CFFLog::Info("[PostProcess] Created neutral LUT (%dx%dx%d)", kLUTSize, kLUTSize, kLUTSize);
    } else {
        CFFLog::Error("[PostProcess] Failed to create neutral LUT");
    }
}

bool CPostProcessPass::loadLUT(const std::string& cubePath) {
    if (cubePath.empty()) {
        m_customLUT.reset();
        m_cachedLUTPath.clear();
        return false;
    }

    // Load .cube file
    SLUTData lutData;
    if (!LoadCubeFile(cubePath, lutData)) {
        CFFLog::Error("[PostProcess] Failed to load LUT: %s", cubePath.c_str());
        return false;
    }

    // Validate LUT size (we only support 32x32x32 for now)
    if (lutData.size != kLUTSize) {
        CFFLog::Warning("[PostProcess] LUT size %u not supported, expected %d. Resampling not implemented.",
                        lutData.size, kLUTSize);
        return false;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Convert to RGBA format
    uint32_t totalTexels = lutData.size * lutData.size * lutData.size;
    std::vector<float> rgbaData(totalTexels * 4);
    for (uint32_t i = 0; i < totalTexels; ++i) {
        rgbaData[i * 4 + 0] = lutData.data[i * 3 + 0];
        rgbaData[i * 4 + 1] = lutData.data[i * 3 + 1];
        rgbaData[i * 4 + 2] = lutData.data[i * 3 + 2];
        rgbaData[i * 4 + 3] = 1.0f;
    }

    // Create 3D texture
    TextureDesc desc = TextureDesc::Texture3D(
        lutData.size, lutData.size, lutData.size,
        ETextureFormat::R32G32B32A32_FLOAT,
        ETextureUsage::ShaderResource
    );
    desc.debugName = "CustomLUT";

    m_customLUT.reset(ctx->CreateTexture(desc, rgbaData.data()));
    m_cachedLUTPath = cubePath;

    if (m_customLUT) {
        CFFLog::Info("[PostProcess] Loaded custom LUT: %s", cubePath.c_str());
        return true;
    } else {
        CFFLog::Error("[PostProcess] Failed to create custom LUT texture");
        return false;
    }
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CPostProcessPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[PostProcessPass] DX11 mode - descriptor sets not supported");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/PostProcess_DS.hlsl";

    // Create PerPass layout for PostProcess
    // PostProcess uses: CB (b0), HDR input (t0), Bloom (t1), LUT (t2), Exposure buffer (t3), Sampler (s0)
    BindingLayoutDesc layoutDesc("PostProcess_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_PostProcess)));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));      // HDR input
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(1));      // Bloom
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(2));      // LUT (3D)
    layoutDesc.AddItem(BindingLayoutItem::Buffer_SRV(3));       // Exposure buffer
    layoutDesc.AddItem(BindingLayoutItem::Sampler(0));          // Linear sampler

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[PostProcessPass] Failed to create PerPass layout");
        return;
    }

    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[PostProcessPass] Failed to allocate PerPass set");
        return;
    }

    // Bind static sampler
    m_perPassSet->Bind(BindingSetItem::Sampler(0, m_sampler.get()));

    // Compile SM 5.1 shaders
    // Vertex shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "VSMain", "vs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[PostProcessPass] VSMain (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Vertex;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "PostProcess_DS_VS";
        m_vs_ds.reset(ctx->CreateShader(desc));
    }

    // Pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSMain", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[PostProcessPass] PSMain (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "PostProcess_DS_PS";
        m_ps_ds.reset(ctx->CreateShader(desc));
    }

    // Create PSO with descriptor set layout
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs_ds.get();
    psoDesc.pixelShader = m_ps_ds.get();
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 8, 0 }
    };
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.depthClipEnable = false;
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.blend.blendEnable = false;
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;
    psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    psoDesc.depthStencilFormat = ETextureFormat::Unknown;
    psoDesc.setLayouts[1] = m_perPassLayout;  // Set 1: PerPass (space1)
    psoDesc.debugName = "PostProcess_DS_PSO";

    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));

    CFFLog::Info("[PostProcessPass] Descriptor set resources initialized");
}
