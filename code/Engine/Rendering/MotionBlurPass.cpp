#include "MotionBlurPass.h"
#include "Engine/SceneLightSettings.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"

using namespace RHI;

// ============================================
// Vertex structure for fullscreen quad
// ============================================
struct SMotionBlurVertex {
    float x, y;   // Position (NDC space)
    float u, v;   // UV
};

// ============================================
// Constant buffer structure
// ============================================
struct SCBMotionBlur {
    float intensity;      // Blur strength multiplier
    int sampleCount;      // Number of samples along velocity
    float maxBlurPixels;  // Maximum blur radius in pixels
    float _pad;
    float texelSizeX;     // 1.0 / width
    float texelSizeY;     // 1.0 / height
    float _pad2[2];
};

// ============================================
// Embedded Shaders
// ============================================
namespace {

// Fullscreen vertex shader
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

// Motion blur pixel shader - linear blur along velocity direction
static const char* kMotionBlurPS = R"(
    cbuffer CB_MotionBlur : register(b0) {
        float gIntensity;
        int gSampleCount;
        float gMaxBlurPixels;
        float _pad;
        float2 gTexelSize;
        float2 _pad2;
    };

    Texture2D gHDRInput : register(t0);
    Texture2D gVelocityBuffer : register(t1);
    SamplerState gLinearSampler : register(s0);
    SamplerState gPointSampler : register(s1);

    struct PSIn {
        float4 pos : SV_Position;
        float2 uv : TEXCOORD0;
    };

    float4 main(PSIn input) : SV_Target {
        // Sample velocity (UV-space motion vector)
        float2 velocity = gVelocityBuffer.SampleLevel(gPointSampler, input.uv, 0).rg;
        velocity *= gIntensity;

        // Early out if velocity is negligible
        float velocityMag = length(velocity);
        if (velocityMag < 0.0001) {
            return gHDRInput.SampleLevel(gLinearSampler, input.uv, 0);
        }

        // Clamp velocity to max blur radius (in UV space)
        float2 maxBlurUV = gMaxBlurPixels * gTexelSize;
        float maxBlurMag = length(maxBlurUV);
        if (velocityMag > maxBlurMag) {
            velocity = velocity * (maxBlurMag / velocityMag);
        }

        // Accumulate samples along velocity direction with tent filter
        float3 color = float3(0.0, 0.0, 0.0);
        float totalWeight = 0.0;
        float invSampleCountMinusOne = 1.0 / (float)(gSampleCount - 1);

        for (int i = 0; i < gSampleCount; ++i) {
            // Sample from -0.5 to +0.5 along velocity
            float t = (float)i * invSampleCountMinusOne - 0.5;
            float2 sampleUV = saturate(input.uv + velocity * t);

            float3 sampleColor = gHDRInput.SampleLevel(gLinearSampler, sampleUV, 0).rgb;

            // Tent filter weight (1.0 at center, 0.0 at edges)
            float weight = 1.0 - abs(t * 2.0);
            color += sampleColor * weight;
            totalWeight += weight;
        }

        return float4(color / totalWeight, 1.0);
    }
)";

} // anonymous namespace

// ============================================
// Lifecycle
// ============================================

bool CMotionBlurPass::Initialize() {
    if (m_initialized) return true;

    createFullscreenQuad();
    createShaders();
    createPSO();

    // Create linear sampler (for HDR input)
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;

        IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
        m_linearSampler.reset(ctx->CreateSampler(desc));
    }

    // Create point sampler (for velocity buffer - no interpolation)
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipPoint;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;

        IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
        m_pointSampler.reset(ctx->CreateSampler(desc));
    }

    m_initialized = true;
    CFFLog::Info("[MotionBlurPass] Initialized");
    return true;
}

void CMotionBlurPass::Shutdown() {
    m_outputHDR.reset();
    m_pso.reset();
    m_fullscreenVS.reset();
    m_motionBlurPS.reset();
    m_vertexBuffer.reset();
    m_linearSampler.reset();
    m_pointSampler.reset();

    m_cachedWidth = 0;
    m_cachedHeight = 0;
    m_initialized = false;
}

// ============================================
// Rendering
// ============================================

ITexture* CMotionBlurPass::Render(ITexture* hdrInput,
                                   ITexture* velocityBuffer,
                                   uint32_t width, uint32_t height,
                                   const SMotionBlurSettings& settings) {
#ifndef FF_LEGACY_BINDING_DISABLED
    CFFLog::Warning("[MotionBlurPass] Using legacy binding path - descriptor set migration pending");

    if (!m_initialized || !hdrInput || !velocityBuffer || width == 0 || height == 0) {
        return hdrInput;  // Return input unchanged on error
    }

    // Skip if intensity is zero
    if (settings.intensity <= 0.0f) {
        return hdrInput;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // Ensure output texture is properly sized
    ensureOutputTexture(width, height);

    // Unbind any existing render targets to avoid hazards
    cmdList->UnbindRenderTargets();

    // Set render target
    ITexture* rt = m_outputHDR.get();
    cmdList->SetRenderTargets(1, &rt, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    // Set pipeline state
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SMotionBlurVertex), 0);

    // Set constant buffer
    SCBMotionBlur cb;
    cb.intensity = settings.intensity;
    cb.sampleCount = std::max(settings.sampleCount, 2);  // Minimum 2 to avoid div-by-zero
    cb.maxBlurPixels = settings.maxBlurPixels;
    cb._pad = 0.0f;
    cb.texelSizeX = 1.0f / static_cast<float>(width);
    cb.texelSizeY = 1.0f / static_cast<float>(height);
    cb._pad2[0] = 0.0f;
    cb._pad2[1] = 0.0f;

    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));

    // Bind textures
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
    cmdList->SetShaderResource(EShaderStage::Pixel, 1, velocityBuffer);
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
    cmdList->SetSampler(EShaderStage::Pixel, 1, m_pointSampler.get());

    // Draw fullscreen quad
    cmdList->Draw(4, 0);

    // Unbind render targets
    cmdList->UnbindRenderTargets();

    return m_outputHDR.get();
#else
    // Descriptor set path not yet implemented
    (void)velocityBuffer;
    (void)width;
    (void)height;
    (void)settings;
    return hdrInput;
#endif
}

// ============================================
// Internal Methods
// ============================================

void CMotionBlurPass::ensureOutputTexture(uint32_t width, uint32_t height) {
    if (width == m_cachedWidth && height == m_cachedHeight && m_outputHDR) {
        return;
    }

    m_cachedWidth = width;
    m_cachedHeight = height;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

    // Create HDR output texture (same format as input)
    TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R16G16B16A16_FLOAT);
    desc.debugName = "MotionBlur_Output";
    m_outputHDR.reset(ctx->CreateTexture(desc, nullptr));

    CFFLog::Info("[MotionBlurPass] Output texture resized to %ux%u", width, height);
}

void CMotionBlurPass::createFullscreenQuad() {
    // Fullscreen quad vertices (triangle strip)
    // NDC: (-1,-1) bottom-left, (1,1) top-right
    // UV: (0,0) top-left, (1,1) bottom-right (DirectX convention)
    SMotionBlurVertex vertices[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f },  // Top-left
        {  1.0f,  1.0f, 1.0f, 0.0f },  // Top-right
        { -1.0f, -1.0f, 0.0f, 1.0f },  // Bottom-left
        {  1.0f, -1.0f, 1.0f, 1.0f },  // Bottom-right
    };

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

    BufferDesc desc;
    desc.size = sizeof(vertices);
    desc.usage = EBufferUsage::Vertex;
    desc.cpuAccess = ECPUAccess::None;
    desc.debugName = "MotionBlur_VB";

    m_vertexBuffer.reset(ctx->CreateBuffer(desc, vertices));
}

void CMotionBlurPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile vertex shader
    {
        SCompiledShader compiled = CompileShaderFromSource(
            kFullscreenVS, "main", "vs_5_0", nullptr, debugShaders);

        if (!compiled.success) {
            CFFLog::Error("[MotionBlurPass] VS compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Vertex;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_fullscreenVS.reset(ctx->CreateShader(desc));
    }

    // Compile pixel shader
    {
        SCompiledShader compiled = CompileShaderFromSource(
            kMotionBlurPS, "main", "ps_5_0", nullptr, debugShaders);

        if (!compiled.success) {
            CFFLog::Error("[MotionBlurPass] PS compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_motionBlurPS.reset(ctx->CreateShader(desc));
    }
}

void CMotionBlurPass::createPSO() {
    if (!m_fullscreenVS || !m_motionBlurPS) {
        CFFLog::Error("[MotionBlurPass] Cannot create PSO: shaders not compiled");
        return;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_fullscreenVS.get();
    psoDesc.pixelShader = m_motionBlurPS.get();

    // Input layout (same as BloomPass)
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 8, 0 }
    };

    // Rasterizer state
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.depthClipEnable = false;

    // Depth stencil state (disabled)
    psoDesc.depthStencil.depthEnable = false;
    psoDesc.depthStencil.depthWriteEnable = false;

    // Blend state (no blending)
    psoDesc.blend.blendEnable = false;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;

    // Render target format (HDR)
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoDesc.depthStencilFormat = ETextureFormat::Unknown;

    psoDesc.debugName = "MotionBlur_PSO";

    m_pso.reset(ctx->CreatePipelineState(psoDesc));
}
