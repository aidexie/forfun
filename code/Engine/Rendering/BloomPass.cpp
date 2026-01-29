#include "BloomPass.h"
#include "Engine/SceneLightSettings.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ICommandList.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <algorithm>

using namespace RHI;

// ============================================
// Vertex structure for fullscreen quad
// ============================================
struct BloomVertex {
    float x, y;   // Position (NDC space)
    float u, v;   // UV
};

// ============================================
// Constant buffer structures
// ============================================
struct CB_BloomThreshold {
    float texelSizeX, texelSizeY;
    float threshold;
    float softKnee;
};

struct CB_BloomDownsample {
    float texelSizeX, texelSizeY;
    float _pad[2];
};

struct CB_BloomUpsample {
    float texelSizeX, texelSizeY;
    float scatter;
    float _pad;
};

// ============================================
// Embedded Shaders
// ============================================
namespace {

// Fullscreen vertex shader (shared by all passes)
static const char* kFullscreenVS = R"(
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

// Threshold pixel shader - extracts bright pixels
static const char* kThresholdPS = R"(
    cbuffer CB_BloomThreshold : register(b0) {
        float2 gTexelSize;
        float gThreshold;
        float gSoftKnee;
    };

    Texture2D gHDRInput : register(t0);
    SamplerState gSampler : register(s0);

    struct PSIn {
        float4 pos : SV_Position;
        float2 uv : TEXCOORD0;
    };

    float Luminance(float3 color) {
        return dot(color, float3(0.2126, 0.7152, 0.0722));
    }

    float3 SoftThreshold(float3 color, float threshold, float knee) {
        float luma = Luminance(color);
        float soft = luma - threshold + knee;
        soft = clamp(soft, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-5);
        float contribution = max(soft, luma - threshold);
        contribution /= max(luma, 1e-5);
        return color * saturate(contribution);
    }

    float4 main(PSIn input) : SV_Target {
        float3 color = gHDRInput.Sample(gSampler, input.uv).rgb;
        float3 bloom = SoftThreshold(color, gThreshold, gSoftKnee * gThreshold);
        bloom = min(bloom, 10.0);  // Clamp fireflies
        return float4(bloom, 1.0);
    }
)";

// Downsample pixel shader - Kawase 5-tap
static const char* kDownsamplePS = R"(
    cbuffer CB_BloomDownsample : register(b0) {
        float2 gTexelSize;
        float2 _pad;
    };

    Texture2D gInput : register(t0);
    SamplerState gSampler : register(s0);

    struct PSIn {
        float4 pos : SV_Position;
        float2 uv : TEXCOORD0;
    };

    float4 main(PSIn input) : SV_Target {
        float2 uv = input.uv;

        float3 center = gInput.Sample(gSampler, uv).rgb;
        float3 tl = gInput.Sample(gSampler, uv + float2(-1.0, -1.0) * gTexelSize).rgb;
        float3 tr = gInput.Sample(gSampler, uv + float2( 1.0, -1.0) * gTexelSize).rgb;
        float3 bl = gInput.Sample(gSampler, uv + float2(-1.0,  1.0) * gTexelSize).rgb;
        float3 br = gInput.Sample(gSampler, uv + float2( 1.0,  1.0) * gTexelSize).rgb;

        float3 result = center * 4.0 + tl + tr + bl + br;
        result *= (1.0 / 8.0);

        return float4(result, 1.0);
    }
)";

// Upsample pixel shader - 9-tap tent filter
static const char* kUpsamplePS = R"(
    cbuffer CB_BloomUpsample : register(b0) {
        float2 gTexelSize;
        float gScatter;
        float _pad;
    };

    Texture2D gInput : register(t0);
    SamplerState gSampler : register(s0);

    struct PSIn {
        float4 pos : SV_Position;
        float2 uv : TEXCOORD0;
    };

    float4 main(PSIn input) : SV_Target {
        float2 uv = input.uv;

        // 9-tap tent filter
        float3 s0 = gInput.Sample(gSampler, uv + float2(-1.0, -1.0) * gTexelSize).rgb;
        float3 s1 = gInput.Sample(gSampler, uv + float2( 0.0, -1.0) * gTexelSize).rgb;
        float3 s2 = gInput.Sample(gSampler, uv + float2( 1.0, -1.0) * gTexelSize).rgb;
        float3 s3 = gInput.Sample(gSampler, uv + float2(-1.0,  0.0) * gTexelSize).rgb;
        float3 s4 = gInput.Sample(gSampler, uv).rgb;
        float3 s5 = gInput.Sample(gSampler, uv + float2( 1.0,  0.0) * gTexelSize).rgb;
        float3 s6 = gInput.Sample(gSampler, uv + float2(-1.0,  1.0) * gTexelSize).rgb;
        float3 s7 = gInput.Sample(gSampler, uv + float2( 0.0,  1.0) * gTexelSize).rgb;
        float3 s8 = gInput.Sample(gSampler, uv + float2( 1.0,  1.0) * gTexelSize).rgb;

        float3 result = s0 + s2 + s6 + s8;
        result += (s1 + s3 + s5 + s7) * 2.0;
        result += s4 * 4.0;
        result *= (1.0 / 16.0);

        // Apply scatter factor to control contribution from lower mip
        // With additive blend: scatter=1 full glow, scatter=0 no glow
        result *= gScatter;

        return float4(result, 1.0);
    }
)";


} // anonymous namespace

// ============================================
// Implementation
// ============================================

bool CBloomPass::Initialize() {
    if (m_initialized) return true;

    createFullscreenQuad();
#ifndef FF_LEGACY_BINDING_DISABLED
    createShaders();
    createPSOs();
#endif // FF_LEGACY_BINDING_DISABLED
    createBlackTexture();

    // Create linear sampler
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::MinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Clamp;
    samplerDesc.addressV = ETextureAddressMode::Clamp;
    samplerDesc.addressW = ETextureAddressMode::Clamp;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    m_linearSampler.reset(ctx->CreateSampler(samplerDesc));

    initDescriptorSets();

    m_initialized = true;
    CFFLog::Info("[BloomPass] Initialized");
    return true;
}

void CBloomPass::Shutdown() {
    for (int i = 0; i < kMipCount; ++i) {
        m_mipChain[i].reset();
        m_mipWidth[i] = 0;
        m_mipHeight[i] = 0;
    }

#ifndef FF_LEGACY_BINDING_DISABLED
    m_thresholdPSO.reset();
    m_downsamplePSO.reset();
    m_upsamplePSO.reset();
    m_upsampleBlendPSO.reset();

    m_fullscreenVS.reset();
    m_thresholdPS.reset();
    m_downsamplePS.reset();
    m_upsamplePS.reset();
#endif // FF_LEGACY_BINDING_DISABLED

    m_vertexBuffer.reset();
    m_linearSampler.reset();
    m_blackTexture.reset();

    // Cleanup DS resources
    m_fullscreenVS_ds.reset();
    m_thresholdPS_ds.reset();
    m_downsamplePS_ds.reset();
    m_upsamplePS_ds.reset();

    m_thresholdPSO_ds.reset();
    m_downsamplePSO_ds.reset();
    m_upsamplePSO_ds.reset();
    m_upsampleBlendPSO_ds.reset();

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

    m_cachedWidth = 0;
    m_cachedHeight = 0;
    m_initialized = false;
}

RHI::ITexture* CBloomPass::Render(ITexture* hdrInput,
                                   uint32_t width, uint32_t height,
                                   const SBloomSettings& settings) {
    if (!m_initialized || !hdrInput || width == 0 || height == 0) {
        return m_blackTexture.get();
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // Ensure mip chain is properly sized
    ensureMipChain(width, height);

    // Unbind any existing render targets to avoid hazards
    cmdList->UnbindRenderTargets();

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        // ============================================
        // Pass 1: Threshold (HDR -> Mip[0] at half res)
        // ============================================
        {
            CScopedDebugEvent evt(cmdList, L"Bloom Threshold (DS)");

            ITexture* rt = m_mipChain[0].get();
            cmdList->SetRenderTargets(1, &rt, nullptr);
            cmdList->SetViewport(0, 0, (float)m_mipWidth[0], (float)m_mipHeight[0], 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, m_mipWidth[0], m_mipHeight[0]);

            cmdList->SetPipelineState(m_thresholdPSO_ds.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
            cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(BloomVertex), 0);

            CB_BloomThreshold cb;
            cb.texelSizeX = 1.0f / (float)width;
            cb.texelSizeY = 1.0f / (float)height;
            cb.threshold = settings.threshold;
            cb.softKnee = 0.5f;  // Fixed soft knee

            // Bind PerPass descriptor set
            m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(CB_BloomThreshold)));
            m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, hdrInput));
            m_perPassSet->Bind(BindingSetItem::Sampler(0, m_linearSampler.get()));
            cmdList->BindDescriptorSet(1, m_perPassSet);

            cmdList->Draw(4, 0);
        }

        // ============================================
        // Pass 2: Downsample chain (Mip[0] -> Mip[4])
        // ============================================
        for (int i = 1; i < kMipCount; ++i) {
            CScopedDebugEvent evt(cmdList, L"Bloom Downsample (DS)");

            // Unbind previous RT before using as SRV
            cmdList->UnbindRenderTargets();

            ITexture* rt = m_mipChain[i].get();
            cmdList->SetRenderTargets(1, &rt, nullptr);
            cmdList->SetViewport(0, 0, (float)m_mipWidth[i], (float)m_mipHeight[i], 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, m_mipWidth[i], m_mipHeight[i]);

            cmdList->SetPipelineState(m_downsamplePSO_ds.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
            cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(BloomVertex), 0);

            CB_BloomDownsample cb;
            cb.texelSizeX = 1.0f / (float)m_mipWidth[i - 1];
            cb.texelSizeY = 1.0f / (float)m_mipHeight[i - 1];
            cb._pad[0] = cb._pad[1] = 0.0f;

            // Bind PerPass descriptor set
            m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(CB_BloomDownsample)));
            m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, m_mipChain[i - 1].get()));
            m_perPassSet->Bind(BindingSetItem::Sampler(0, m_linearSampler.get()));
            cmdList->BindDescriptorSet(1, m_perPassSet);

            cmdList->Draw(4, 0);
        }

        // ============================================
        // Pass 3: Upsample chain (Mip[4] -> Mip[0])
        // ============================================
        for (int i = kMipCount - 2; i >= 0; --i) {
            CScopedDebugEvent evt(cmdList, L"Bloom Upsample (DS)");

            // Unbind previous RT before using as SRV
            cmdList->UnbindRenderTargets();

            ITexture* rt = m_mipChain[i].get();
            cmdList->SetRenderTargets(1, &rt, nullptr);
            cmdList->SetViewport(0, 0, (float)m_mipWidth[i], (float)m_mipHeight[i], 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, m_mipWidth[i], m_mipHeight[i]);

            // Use additive blend PSO to accumulate with existing content
            cmdList->SetPipelineState(m_upsampleBlendPSO_ds.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
            cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(BloomVertex), 0);

            CB_BloomUpsample cb;
            cb.texelSizeX = 1.0f / (float)m_mipWidth[i + 1];
            cb.texelSizeY = 1.0f / (float)m_mipHeight[i + 1];
            cb.scatter = settings.scatter;
            cb._pad = 0.0f;

            // Bind PerPass descriptor set
            m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(CB_BloomUpsample)));
            m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, m_mipChain[i + 1].get()));
            m_perPassSet->Bind(BindingSetItem::Sampler(0, m_linearSampler.get()));
            cmdList->BindDescriptorSet(1, m_perPassSet);

            cmdList->Draw(4, 0);
        }
    }
#ifndef FF_LEGACY_BINDING_DISABLED
    else {
        // Legacy path for DX11
        // ============================================
        // Pass 1: Threshold (HDR -> Mip[0] at half res)
        // ============================================
        {
            CScopedDebugEvent evt(cmdList, L"Bloom Threshold");

            ITexture* rt = m_mipChain[0].get();
            cmdList->SetRenderTargets(1, &rt, nullptr);
            cmdList->SetViewport(0, 0, (float)m_mipWidth[0], (float)m_mipHeight[0], 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, m_mipWidth[0], m_mipHeight[0]);

            cmdList->SetPipelineState(m_thresholdPSO.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
            cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(BloomVertex), 0);

            CB_BloomThreshold cb;
            cb.texelSizeX = 1.0f / (float)width;
            cb.texelSizeY = 1.0f / (float)height;
            cb.threshold = settings.threshold;
            cb.softKnee = 0.5f;  // Fixed soft knee

            cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
            cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
            cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

            cmdList->Draw(4, 0);
        }

        // ============================================
        // Pass 2: Downsample chain (Mip[0] -> Mip[4])
        // ============================================
        for (int i = 1; i < kMipCount; ++i) {
            CScopedDebugEvent evt(cmdList, L"Bloom Downsample");

            // Unbind previous RT before using as SRV
            cmdList->UnbindRenderTargets();

            ITexture* rt = m_mipChain[i].get();
            cmdList->SetRenderTargets(1, &rt, nullptr);
            cmdList->SetViewport(0, 0, (float)m_mipWidth[i], (float)m_mipHeight[i], 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, m_mipWidth[i], m_mipHeight[i]);

            cmdList->SetPipelineState(m_downsamplePSO.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
            cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(BloomVertex), 0);

            CB_BloomDownsample cb;
            cb.texelSizeX = 1.0f / (float)m_mipWidth[i - 1];
            cb.texelSizeY = 1.0f / (float)m_mipHeight[i - 1];
            cb._pad[0] = cb._pad[1] = 0.0f;

            cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
            cmdList->SetShaderResource(EShaderStage::Pixel, 0, m_mipChain[i - 1].get());
            cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

            cmdList->Draw(4, 0);
        }

        // ============================================
        // Pass 3: Upsample chain (Mip[4] -> Mip[0])
        // ============================================
        for (int i = kMipCount - 2; i >= 0; --i) {
            CScopedDebugEvent evt(cmdList, L"Bloom Upsample");

            // Unbind previous RT before using as SRV
            cmdList->UnbindRenderTargets();

            ITexture* rt = m_mipChain[i].get();
            cmdList->SetRenderTargets(1, &rt, nullptr);
            cmdList->SetViewport(0, 0, (float)m_mipWidth[i], (float)m_mipHeight[i], 0.0f, 1.0f);
            cmdList->SetScissorRect(0, 0, m_mipWidth[i], m_mipHeight[i]);

            // Use additive blend PSO to accumulate with existing content
            cmdList->SetPipelineState(m_upsampleBlendPSO.get());
            cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
            cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(BloomVertex), 0);

            CB_BloomUpsample cb;
            cb.texelSizeX = 1.0f / (float)m_mipWidth[i + 1];
            cb.texelSizeY = 1.0f / (float)m_mipHeight[i + 1];
            cb.scatter = settings.scatter;
            cb._pad = 0.0f;

            cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
            cmdList->SetShaderResource(EShaderStage::Pixel, 0, m_mipChain[i + 1].get());
            cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

            cmdList->Draw(4, 0);
        }
    }
#endif // FF_LEGACY_BINDING_DISABLED

    // Unbind render targets to prepare for PostProcess pass
    cmdList->UnbindRenderTargets();

    // Return the final bloom texture (Mip[0] at half resolution)
    return m_mipChain[0].get();
}

void CBloomPass::ensureMipChain(uint32_t width, uint32_t height) {
    // Check if resize is needed
    if (width == m_cachedWidth && height == m_cachedHeight) {
        return;
    }

    m_cachedWidth = width;
    m_cachedHeight = height;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Calculate mip dimensions (each mip is half the previous)
    uint32_t w = width / 2;
    uint32_t h = height / 2;

    for (int i = 0; i < kMipCount; ++i) {
        // Ensure minimum size of 1
        w = std::max(w, 1u);
        h = std::max(h, 1u);

        m_mipWidth[i] = w;
        m_mipHeight[i] = h;

        // Create render target texture
        // Use R16G16B16A16_FLOAT for HDR precision
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = ETextureFormat::R16G16B16A16_FLOAT;
        desc.usage = ETextureUsage::RenderTarget | ETextureUsage::ShaderResource;
        desc.clearColor[0] = 0.0f;
        desc.clearColor[1] = 0.0f;
        desc.clearColor[2] = 0.0f;
        desc.clearColor[3] = 1.0f;

        // Create debug name
        char debugNameBuf[32];
        snprintf(debugNameBuf, sizeof(debugNameBuf), "Bloom_Mip%d", i);
        desc.debugName = debugNameBuf;

        m_mipChain[i].reset(ctx->CreateTexture(desc, nullptr));

        // Halve for next mip
        w /= 2;
        h /= 2;
    }

    CFFLog::Info("[BloomPass] Mip chain resized: %dx%d -> %dx%d (5 levels)",
                 m_mipWidth[0], m_mipHeight[0],
                 m_mipWidth[kMipCount - 1], m_mipHeight[kMipCount - 1]);
}

void CBloomPass::createFullscreenQuad() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Fullscreen quad in NDC space (triangle strip)
    BloomVertex vertices[] = {
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

#ifndef FF_LEGACY_BINDING_DISABLED
void CBloomPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile vertex shader
    SCompiledShader vsCompiled = CompileShaderFromSource(kFullscreenVS, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("[BloomPass] VS compilation failed: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "Bloom_VS";
    m_fullscreenVS.reset(ctx->CreateShader(vsDesc));

    // Compile threshold pixel shader
    SCompiledShader thresholdCompiled = CompileShaderFromSource(kThresholdPS, "main", "ps_5_0", nullptr, debugShaders);
    if (!thresholdCompiled.success) {
        CFFLog::Error("[BloomPass] Threshold PS compilation failed: %s", thresholdCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc thresholdDesc;
    thresholdDesc.type = EShaderType::Pixel;
    thresholdDesc.bytecode = thresholdCompiled.bytecode.data();
    thresholdDesc.bytecodeSize = thresholdCompiled.bytecode.size();
    thresholdDesc.debugName = "Bloom_Threshold_PS";
    m_thresholdPS.reset(ctx->CreateShader(thresholdDesc));

    // Compile downsample pixel shader
    SCompiledShader downsampleCompiled = CompileShaderFromSource(kDownsamplePS, "main", "ps_5_0", nullptr, debugShaders);
    if (!downsampleCompiled.success) {
        CFFLog::Error("[BloomPass] Downsample PS compilation failed: %s", downsampleCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc downsampleDesc;
    downsampleDesc.type = EShaderType::Pixel;
    downsampleDesc.bytecode = downsampleCompiled.bytecode.data();
    downsampleDesc.bytecodeSize = downsampleCompiled.bytecode.size();
    downsampleDesc.debugName = "Bloom_Downsample_PS";
    m_downsamplePS.reset(ctx->CreateShader(downsampleDesc));

    // Compile upsample pixel shader
    SCompiledShader upsampleCompiled = CompileShaderFromSource(kUpsamplePS, "main", "ps_5_0", nullptr, debugShaders);
    if (!upsampleCompiled.success) {
        CFFLog::Error("[BloomPass] Upsample PS compilation failed: %s", upsampleCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc upsampleDesc;
    upsampleDesc.type = EShaderType::Pixel;
    upsampleDesc.bytecode = upsampleCompiled.bytecode.data();
    upsampleDesc.bytecodeSize = upsampleCompiled.bytecode.size();
    upsampleDesc.debugName = "Bloom_Upsample_PS";
    m_upsamplePS.reset(ctx->CreateShader(upsampleDesc));
}

void CBloomPass::createPSOs() {
    if (!m_fullscreenVS || !m_thresholdPS || !m_downsamplePS || !m_upsamplePS) {
        return;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Common PSO settings
    PipelineStateDesc basePsoDesc;
    basePsoDesc.vertexShader = m_fullscreenVS.get();
    basePsoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 8, 0 }
    };
    basePsoDesc.rasterizer.cullMode = ECullMode::None;
    basePsoDesc.rasterizer.fillMode = EFillMode::Solid;
    basePsoDesc.rasterizer.depthClipEnable = false;
    basePsoDesc.depthStencil.depthEnable = false;
    basePsoDesc.depthStencil.depthWriteEnable = false;
    basePsoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;
    basePsoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    basePsoDesc.depthStencilFormat = ETextureFormat::Unknown;

    // Threshold PSO (no blending)
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_thresholdPS.get();
        psoDesc.blend.blendEnable = false;
        psoDesc.debugName = "Bloom_Threshold_PSO";
        m_thresholdPSO.reset(ctx->CreatePipelineState(psoDesc));
    }

    // Downsample PSO (no blending)
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_downsamplePS.get();
        psoDesc.blend.blendEnable = false;
        psoDesc.debugName = "Bloom_Downsample_PSO";
        m_downsamplePSO.reset(ctx->CreatePipelineState(psoDesc));
    }

    // Upsample PSO (no blending - for first upsample if needed)
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_upsamplePS.get();
        psoDesc.blend.blendEnable = false;
        psoDesc.debugName = "Bloom_Upsample_PSO";
        m_upsamplePSO.reset(ctx->CreatePipelineState(psoDesc));
    }

    // Upsample PSO with additive blending (for accumulation)
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_upsamplePS.get();
        psoDesc.blend.blendEnable = true;
        psoDesc.blend.srcBlend = EBlendFactor::One;
        psoDesc.blend.dstBlend = EBlendFactor::One;
        psoDesc.blend.blendOp = EBlendOp::Add;
        psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
        psoDesc.blend.dstBlendAlpha = EBlendFactor::One;
        psoDesc.blend.blendOpAlpha = EBlendOp::Add;
        psoDesc.debugName = "Bloom_UpsampleBlend_PSO";
        m_upsampleBlendPSO.reset(ctx->CreatePipelineState(psoDesc));
    }
}
#endif // FF_LEGACY_BINDING_DISABLED

void CBloomPass::createBlackTexture() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Create 1x1 black texture as fallback when bloom is disabled
    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = ETextureFormat::R16G16B16A16_FLOAT;
    desc.usage = ETextureUsage::ShaderResource;
    desc.debugName = "Bloom_BlackFallback";

    // Initialize with black pixel (4 floats = 16 bytes)
    float blackPixel[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_blackTexture.reset(ctx->CreateTexture(desc, blackPixel));
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CBloomPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[BloomPass] DX11 mode - descriptor sets not supported");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/Bloom_DS.ps.hlsl";

    // Create PerPass layout for Bloom
    // Bloom uses: CB (b0), Input texture (t0), Sampler (s0)
    BindingLayoutDesc layoutDesc("Bloom_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, 32));  // CB_Bloom (32 bytes max)
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));      // Input texture
    layoutDesc.AddItem(BindingLayoutItem::Sampler(0));          // Linear sampler

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[BloomPass] Failed to create PerPass layout");
        return;
    }

    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[BloomPass] Failed to allocate PerPass set");
        return;
    }

    // Bind static sampler
    m_perPassSet->Bind(BindingSetItem::Sampler(0, m_linearSampler.get()));

    // Compile SM 5.1 shaders
    // Vertex shader (same for all passes)
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "VSMain", "vs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[BloomPass] VSMain (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Vertex;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "Bloom_DS_VS";
        m_fullscreenVS_ds.reset(ctx->CreateShader(desc));
    }

    // Threshold pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSThreshold", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[BloomPass] PSThreshold (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "Bloom_DS_Threshold_PS";
        m_thresholdPS_ds.reset(ctx->CreateShader(desc));
    }

    // Downsample pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSDownsample", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[BloomPass] PSDownsample (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "Bloom_DS_Downsample_PS";
        m_downsamplePS_ds.reset(ctx->CreateShader(desc));
    }

    // Upsample pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSUpsample", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[BloomPass] PSUpsample (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "Bloom_DS_Upsample_PS";
        m_upsamplePS_ds.reset(ctx->CreateShader(desc));
    }

    // Create PSOs with descriptor set layouts
    PipelineStateDesc basePsoDesc;
    basePsoDesc.vertexShader = m_fullscreenVS_ds.get();
    basePsoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 8, 0 }
    };
    basePsoDesc.rasterizer.cullMode = ECullMode::None;
    basePsoDesc.rasterizer.fillMode = EFillMode::Solid;
    basePsoDesc.rasterizer.depthClipEnable = false;
    basePsoDesc.depthStencil.depthEnable = false;
    basePsoDesc.depthStencil.depthWriteEnable = false;
    basePsoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;
    basePsoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    basePsoDesc.depthStencilFormat = ETextureFormat::Unknown;
    basePsoDesc.setLayouts[1] = m_perPassLayout;  // Set 1: PerPass (space1)

    // Threshold PSO
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_thresholdPS_ds.get();
        psoDesc.blend.blendEnable = false;
        psoDesc.debugName = "Bloom_DS_Threshold_PSO";
        m_thresholdPSO_ds.reset(ctx->CreatePipelineState(psoDesc));
    }

    // Downsample PSO
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_downsamplePS_ds.get();
        psoDesc.blend.blendEnable = false;
        psoDesc.debugName = "Bloom_DS_Downsample_PSO";
        m_downsamplePSO_ds.reset(ctx->CreatePipelineState(psoDesc));
    }

    // Upsample PSO (no blend)
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_upsamplePS_ds.get();
        psoDesc.blend.blendEnable = false;
        psoDesc.debugName = "Bloom_DS_Upsample_PSO";
        m_upsamplePSO_ds.reset(ctx->CreatePipelineState(psoDesc));
    }

    // Upsample PSO with additive blending
    {
        PipelineStateDesc psoDesc = basePsoDesc;
        psoDesc.pixelShader = m_upsamplePS_ds.get();
        psoDesc.blend.blendEnable = true;
        psoDesc.blend.srcBlend = EBlendFactor::One;
        psoDesc.blend.dstBlend = EBlendFactor::One;
        psoDesc.blend.blendOp = EBlendOp::Add;
        psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
        psoDesc.blend.dstBlendAlpha = EBlendFactor::One;
        psoDesc.blend.blendOpAlpha = EBlendOp::Add;
        psoDesc.debugName = "Bloom_DS_UpsampleBlend_PSO";
        m_upsampleBlendPSO_ds.reset(ctx->CreatePipelineState(psoDesc));
    }

    CFFLog::Info("[BloomPass] Descriptor set resources initialized");
}
