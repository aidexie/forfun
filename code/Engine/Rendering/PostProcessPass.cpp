#include "PostProcessPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include <cstring>

using namespace RHI;

struct FullscreenVertex {
    float x, y;       // Position (NDC space)
    float u, v;       // UV
};

struct CB_PostProcess {
    float exposure;
    float bloomIntensity;
    float _pad[2];
};

bool CPostProcessPass::Initialize() {
    if (m_initialized) return true;

    createFullscreenQuad();
    createShaders();
    createPipelineState();

    // Create sampler
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Clamp;
    samplerDesc.addressV = ETextureAddressMode::Clamp;
    samplerDesc.addressW = ETextureAddressMode::Clamp;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    m_sampler.reset(ctx->CreateSampler(samplerDesc));

    // Create constant buffer (CPU writable for updating exposure)
    BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_PostProcess);
    cbDesc.usage = EBufferUsage::Constant;
    cbDesc.cpuAccess = ECPUAccess::Write;
    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));

    m_initialized = true;
    return true;
}

void CPostProcessPass::Shutdown() {
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_vertexBuffer.reset();
    m_constantBuffer.reset();
    m_sampler.reset();
    m_initialized = false;
}

void CPostProcessPass::Render(ITexture* hdrInput,
                              ITexture* bloomTexture,
                              ITexture* ldrOutput,
                              uint32_t width, uint32_t height,
                              float exposure,
                              float bloomIntensity) {
    if (!m_initialized || !hdrInput || !ldrOutput || !m_pso) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // CRITICAL: Unbind render targets before using hdrInput as SRV
    // Otherwise D3D11 will null out the SRV to avoid RTV/SRV hazard
    cmdList->UnbindRenderTargets();

    // Update constant buffer with exposure and bloom intensity
    CB_PostProcess cb;
    cb.exposure = exposure;
    cb.bloomIntensity = bloomTexture ? bloomIntensity : 0.0f;
    cb._pad[0] = cb._pad[1] = 0.0f;

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

    // Set constant buffer and resources (use SetConstantBufferData for DX12 compatibility)
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_PostProcess));
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
    if (bloomTexture) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, bloomTexture);
    }
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

    // Draw fullscreen quad
    cmdList->Draw(4, 0);

    // Unbind render target to prevent hazards
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

    // Pixel shader: Tone mapping + Gamma correction + Bloom compositing
    const char* psCode = R"(
        Texture2D hdrTexture : register(t0);
        Texture2D bloomTexture : register(t1);
        SamplerState samp : register(s0);

        cbuffer CB_PostProcess : register(b0) {
            float gExposure;
            float gBloomIntensity;
            float2 _pad;
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

        float4 main(PSIn input) : SV_Target {
            // Sample HDR input (linear space)
            float3 hdrColor = hdrTexture.Sample(samp, input.uv).rgb;

            // Add bloom contribution (bloom texture is half res, bilinear upsample)
            if (gBloomIntensity > 0.0) {
                float3 bloom = bloomTexture.Sample(samp, input.uv).rgb;
                hdrColor += bloom * gBloomIntensity;
            }

            // Apply exposure (adjust brightness before tone mapping)
            hdrColor *= gExposure;

            // Tone mapping: HDR → LDR [0, 1] (still linear space)
            float3 ldrColor = ACESFilm(hdrColor);

            // Gamma correction: Linear → sRGB
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
    psoDesc.debugName = "PostProcess_ToneMap_PSO";

    m_pso.reset(ctx->CreatePipelineState(psoDesc));
}
