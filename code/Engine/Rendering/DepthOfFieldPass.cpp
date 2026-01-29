#include "DepthOfFieldPass.h"
#include "Engine/SceneLightSettings.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
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
struct SDoFVertex {
    float x, y;   // Position (NDC space)
    float u, v;   // UV
};

// ============================================
// Constant buffer structures
// ============================================
struct SCBCoC {
    float focusDistance;    // Focus plane distance (world units)
    float focalRange;       // Depth range in focus
    float aperture;         // f-stop value
    float maxCoCRadius;     // Max CoC in pixels
    float nearZ;            // Camera near plane
    float farZ;             // Camera far plane
    float texelSizeX;       // 1.0 / width
    float texelSizeY;       // 1.0 / height
};

struct SCBBlur {
    float texelSizeX;       // 1.0 / halfWidth
    float texelSizeY;       // 1.0 / halfHeight
    float maxCoCRadius;     // Max blur radius
    int sampleCount;        // Number of blur samples (11)
};

struct SCBComposite {
    float texelSizeX;       // 1.0 / width
    float texelSizeY;       // 1.0 / height
    float _pad[2];
};

// ============================================
// Lifecycle
// ============================================

bool CDepthOfFieldPass::Initialize() {
    if (m_initialized) return true;

    createFullscreenQuad();
    if (!createShaders()) {
        CFFLog::Error("[DepthOfFieldPass] Shader compilation failed, initialization aborted");
        Shutdown();
        return false;
    }
    if (!createPSOs()) {
        CFFLog::Error("[DepthOfFieldPass] PSO creation failed, initialization aborted");
        Shutdown();
        return false;
    }

    // Create linear sampler
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;

        IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
        m_linearSampler.reset(ctx->CreateSampler(desc));
    }

    // Create point sampler
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
    CFFLog::Info("[DepthOfFieldPass] Initialized");
    return true;
}

void CDepthOfFieldPass::Shutdown() {
    m_outputHDR.reset();
    m_cocBuffer.reset();
    m_nearColor.reset();
    m_farColor.reset();
    m_nearCoc.reset();
    m_farCoc.reset();
    m_blurTempNear.reset();
    m_blurTempFar.reset();

    m_cocPSO.reset();
    m_downsampleSplitPSO.reset();
    m_blurHPSO.reset();
    m_blurVPSO.reset();
    m_compositePSO.reset();

    m_fullscreenVS.reset();
    m_cocPS.reset();
    m_downsampleSplitPS.reset();
    m_blurHPS.reset();
    m_blurVPS.reset();
    m_compositePS.reset();

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

ITexture* CDepthOfFieldPass::Render(ITexture* hdrInput,
                                     ITexture* depthBuffer,
                                     float cameraNearZ, float cameraFarZ,
                                     uint32_t width, uint32_t height,
                                     const SDepthOfFieldSettings& settings) {
#ifndef FF_LEGACY_BINDING_DISABLED
    if (!m_initialized || !hdrInput || !depthBuffer || width == 0 || height == 0) {
        return hdrInput;
    }

    // Skip if aperture is very high (minimal blur)
    if (settings.aperture >= 16.0f) {
        return hdrInput;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    // Ensure textures are properly sized
    ensureTextures(width, height);

    uint32_t halfWidth = std::max(1u, width / 2);
    uint32_t halfHeight = std::max(1u, height / 2);

    // Pass 1: CoC Calculation (full-res)
    renderCoCPass(cmdList, depthBuffer, cameraNearZ, cameraFarZ, width, height, settings);

    // Pass 2: Downsample + Near/Far Split (half-res)
    renderDownsampleSplitPass(cmdList, hdrInput, width, height);

    // Pass 3: Horizontal Blur
    renderBlurPass(cmdList, true, halfWidth, halfHeight, settings);

    // Pass 4: Vertical Blur
    renderBlurPass(cmdList, false, halfWidth, halfHeight, settings);

    // Pass 5: Composite (full-res)
    renderCompositePass(cmdList, hdrInput, width, height);

    return m_outputHDR.get();
#else
    CFFLog::Warning("[DepthOfFieldPass] Legacy binding disabled - descriptor set path not implemented");
    return hdrInput;
#endif
}

// ============================================
// Pass Implementations
// ============================================

void CDepthOfFieldPass::renderCoCPass(ICommandList* cmdList, ITexture* depthBuffer,
                                       float nearZ, float farZ, uint32_t width, uint32_t height,
                                       const SDepthOfFieldSettings& settings) {
    cmdList->UnbindRenderTargets();

    ITexture* rt = m_cocBuffer.get();
    cmdList->SetRenderTargets(1, &rt, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    cmdList->SetPipelineState(m_cocPSO.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

    SCBCoC cb;
    cb.focusDistance = settings.focusDistance;
    cb.focalRange = std::max(settings.focalRange, 0.1f);
    cb.aperture = settings.aperture;
    cb.maxCoCRadius = settings.maxBlurRadius;
    cb.nearZ = nearZ;
    cb.farZ = farZ;
    cb.texelSizeX = 1.0f / static_cast<float>(width);
    cb.texelSizeY = 1.0f / static_cast<float>(height);

    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, depthBuffer);
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_pointSampler.get());

    cmdList->Draw(4, 0);
    cmdList->UnbindRenderTargets();
}

void CDepthOfFieldPass::renderDownsampleSplitPass(ICommandList* cmdList, ITexture* hdrInput,
                                                   uint32_t width, uint32_t height) {
    cmdList->UnbindRenderTargets();

    uint32_t halfWidth = std::max(1u, width / 2);
    uint32_t halfHeight = std::max(1u, height / 2);

    ITexture* rts[4] = { m_nearColor.get(), m_farColor.get(), m_nearCoc.get(), m_farCoc.get() };
    cmdList->SetRenderTargets(4, rts, nullptr);
    cmdList->SetViewport(0, 0, (float)halfWidth, (float)halfHeight, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, halfWidth, halfHeight);

    cmdList->SetPipelineState(m_downsampleSplitPSO.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

    cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
    cmdList->SetShaderResource(EShaderStage::Pixel, 1, m_cocBuffer.get());
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
    cmdList->SetSampler(EShaderStage::Pixel, 1, m_pointSampler.get());

    cmdList->Draw(4, 0);
    cmdList->UnbindRenderTargets();
}

void CDepthOfFieldPass::renderBlurPass(ICommandList* cmdList, bool horizontal,
                                        uint32_t halfWidth, uint32_t halfHeight,
                                        const SDepthOfFieldSettings& settings) {
    cmdList->UnbindRenderTargets();

    // Select input/output based on direction
    ITexture* nearInput = horizontal ? m_nearColor.get() : m_blurTempNear.get();
    ITexture* farInput = horizontal ? m_farColor.get() : m_blurTempFar.get();
    ITexture* nearCocInput = m_nearCoc.get();
    ITexture* farCocInput = m_farCoc.get();

    ITexture* nearOutput = horizontal ? m_blurTempNear.get() : m_nearColor.get();
    ITexture* farOutput = horizontal ? m_blurTempFar.get() : m_farColor.get();

    // Blur near layer
    {
        ITexture* rt = nearOutput;
        cmdList->SetRenderTargets(1, &rt, nullptr);
        cmdList->SetViewport(0, 0, (float)halfWidth, (float)halfHeight, 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, halfWidth, halfHeight);

        cmdList->SetPipelineState(horizontal ? m_blurHPSO.get() : m_blurVPSO.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

        SCBBlur cb;
        cb.texelSizeX = 1.0f / static_cast<float>(halfWidth);
        cb.texelSizeY = 1.0f / static_cast<float>(halfHeight);
        cb.maxCoCRadius = settings.maxBlurRadius;
        cb.sampleCount = 11;

        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, nearInput);
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, nearCocInput);
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

        cmdList->Draw(4, 0);
    }

    cmdList->UnbindRenderTargets();

    // Blur far layer
    {
        ITexture* rt = farOutput;
        cmdList->SetRenderTargets(1, &rt, nullptr);
        cmdList->SetViewport(0, 0, (float)halfWidth, (float)halfHeight, 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, halfWidth, halfHeight);

        cmdList->SetPipelineState(horizontal ? m_blurHPSO.get() : m_blurVPSO.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

        SCBBlur cb;
        cb.texelSizeX = 1.0f / static_cast<float>(halfWidth);
        cb.texelSizeY = 1.0f / static_cast<float>(halfHeight);
        cb.maxCoCRadius = settings.maxBlurRadius;
        cb.sampleCount = 11;

        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, farInput);
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, farCocInput);
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

        cmdList->Draw(4, 0);
    }

    cmdList->UnbindRenderTargets();
}

void CDepthOfFieldPass::renderCompositePass(ICommandList* cmdList, ITexture* hdrInput,
                                             uint32_t width, uint32_t height) {
    cmdList->UnbindRenderTargets();

    ITexture* rt = m_outputHDR.get();
    cmdList->SetRenderTargets(1, &rt, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    cmdList->SetPipelineState(m_compositePSO.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

    SCBComposite cb;
    cb.texelSizeX = 1.0f / static_cast<float>(width);
    cb.texelSizeY = 1.0f / static_cast<float>(height);
    cb._pad[0] = 0.0f;
    cb._pad[1] = 0.0f;

    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, hdrInput);
    cmdList->SetShaderResource(EShaderStage::Pixel, 1, m_cocBuffer.get());
    cmdList->SetShaderResource(EShaderStage::Pixel, 2, m_nearColor.get());
    cmdList->SetShaderResource(EShaderStage::Pixel, 3, m_farColor.get());
    cmdList->SetShaderResource(EShaderStage::Pixel, 4, m_nearCoc.get());
    cmdList->SetShaderResource(EShaderStage::Pixel, 5, m_farCoc.get());
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
    cmdList->SetSampler(EShaderStage::Pixel, 1, m_pointSampler.get());

    cmdList->Draw(4, 0);
    cmdList->UnbindRenderTargets();
}

// ============================================
// Internal Methods
// ============================================

void CDepthOfFieldPass::ensureTextures(uint32_t width, uint32_t height) {
    if (width == m_cachedWidth && height == m_cachedHeight && m_outputHDR) {
        return;
    }

    m_cachedWidth = width;
    m_cachedHeight = height;

    uint32_t halfWidth = std::max(1u, width / 2);
    uint32_t halfHeight = std::max(1u, height / 2);

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

    // Full-res textures
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R32_FLOAT);
        desc.debugName = "DoF_CoC";
        m_cocBuffer.reset(ctx->CreateTexture(desc, nullptr));
    }
    {
        TextureDesc desc = TextureDesc::RenderTarget(width, height, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "DoF_Output";
        m_outputHDR.reset(ctx->CreateTexture(desc, nullptr));
    }

    // Half-res textures (near/far layers)
    {
        TextureDesc desc = TextureDesc::RenderTarget(halfWidth, halfHeight, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "DoF_NearColor";
        m_nearColor.reset(ctx->CreateTexture(desc, nullptr));
    }
    {
        TextureDesc desc = TextureDesc::RenderTarget(halfWidth, halfHeight, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "DoF_FarColor";
        m_farColor.reset(ctx->CreateTexture(desc, nullptr));
    }
    {
        TextureDesc desc = TextureDesc::RenderTarget(halfWidth, halfHeight, ETextureFormat::R32_FLOAT);
        desc.debugName = "DoF_NearCoC";
        m_nearCoc.reset(ctx->CreateTexture(desc, nullptr));
    }
    {
        TextureDesc desc = TextureDesc::RenderTarget(halfWidth, halfHeight, ETextureFormat::R32_FLOAT);
        desc.debugName = "DoF_FarCoC";
        m_farCoc.reset(ctx->CreateTexture(desc, nullptr));
    }
    {
        TextureDesc desc = TextureDesc::RenderTarget(halfWidth, halfHeight, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "DoF_BlurTempNear";
        m_blurTempNear.reset(ctx->CreateTexture(desc, nullptr));
    }
    {
        TextureDesc desc = TextureDesc::RenderTarget(halfWidth, halfHeight, ETextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "DoF_BlurTempFar";
        m_blurTempFar.reset(ctx->CreateTexture(desc, nullptr));
    }

    CFFLog::Info("[DepthOfFieldPass] Textures resized to %ux%u (half: %ux%u)", width, height, halfWidth, halfHeight);
}

void CDepthOfFieldPass::createFullscreenQuad() {
    SDoFVertex vertices[] = {
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
    desc.debugName = "DoF_VB";

    m_vertexBuffer.reset(ctx->CreateBuffer(desc, vertices));
}

bool CDepthOfFieldPass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

#if defined(_DEBUG)
    static constexpr bool kDebugShaders = true;
#else
    static constexpr bool kDebugShaders = false;
#endif

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

    // Fullscreen VS (shared with other post-process passes)
    {
        std::string path = shaderDir + "Fullscreen.vs.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(path, "main", "vs_5_0", nullptr, kDebugShaders);

        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] Fullscreen.vs.hlsl compilation failed: %s", compiled.errorMessage.c_str());
            return false;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Vertex;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_fullscreenVS.reset(ctx->CreateShader(desc));
    }

    // CoC PS
    {
        std::string path = shaderDir + "DoFCoC.ps.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(path, "main", "ps_5_0", nullptr, kDebugShaders);

        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] DoFCoC.ps.hlsl compilation failed: %s", compiled.errorMessage.c_str());
            return false;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_cocPS.reset(ctx->CreateShader(desc));
    }

    // Downsample + Split PS
    {
        std::string path = shaderDir + "DoFDownsampleSplit.ps.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(path, "main", "ps_5_0", nullptr, kDebugShaders);

        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] DoFDownsampleSplit.ps.hlsl compilation failed: %s", compiled.errorMessage.c_str());
            return false;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_downsampleSplitPS.reset(ctx->CreateShader(desc));
    }

    // Blur Horizontal PS
    {
        std::string path = shaderDir + "DoFBlurH.ps.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(path, "main", "ps_5_0", nullptr, kDebugShaders);

        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] DoFBlurH.ps.hlsl compilation failed: %s", compiled.errorMessage.c_str());
            return false;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_blurHPS.reset(ctx->CreateShader(desc));
    }

    // Blur Vertical PS
    {
        std::string path = shaderDir + "DoFBlurV.ps.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(path, "main", "ps_5_0", nullptr, kDebugShaders);

        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] DoFBlurV.ps.hlsl compilation failed: %s", compiled.errorMessage.c_str());
            return false;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_blurVPS.reset(ctx->CreateShader(desc));
    }

    // Composite PS
    {
        std::string path = shaderDir + "DoFComposite.ps.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(path, "main", "ps_5_0", nullptr, kDebugShaders);

        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] DoFComposite.ps.hlsl compilation failed: %s", compiled.errorMessage.c_str());
            return false;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        m_compositePS.reset(ctx->CreateShader(desc));
    }

    CFFLog::Info("[DepthOfFieldPass] All shaders compiled successfully");
    return true;
}

bool CDepthOfFieldPass::createPSOs() {
    if (!m_fullscreenVS) {
        CFFLog::Error("[DepthOfFieldPass] Cannot create PSOs: vertex shader not compiled");
        return false;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();

    // Common PSO settings
    auto createBasePSODesc = [this]() {
        PipelineStateDesc desc;
        desc.vertexShader = m_fullscreenVS.get();
        desc.inputLayout = {
            { EVertexSemantic::Position, 0, EVertexFormat::Float2, 0, 0 },
            { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 8, 0 }
        };
        desc.rasterizer.fillMode = EFillMode::Solid;
        desc.rasterizer.cullMode = ECullMode::None;
        desc.rasterizer.depthClipEnable = false;
        desc.depthStencil.depthEnable = false;
        desc.depthStencil.depthWriteEnable = false;
        desc.blend.blendEnable = false;
        desc.primitiveTopology = EPrimitiveTopology::TriangleStrip;
        desc.depthStencilFormat = ETextureFormat::Unknown;
        return desc;
    };

    // CoC PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_cocPS.get();
        desc.renderTargetFormats = { ETextureFormat::R32_FLOAT };
        desc.debugName = "DoF_CoC_PSO";
        m_cocPSO.reset(ctx->CreatePipelineState(desc));
    }

    // Downsample + Split PSO (4 render targets)
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_downsampleSplitPS.get();
        desc.renderTargetFormats = {
            ETextureFormat::R16G16B16A16_FLOAT,  // nearColor
            ETextureFormat::R16G16B16A16_FLOAT,  // farColor
            ETextureFormat::R32_FLOAT,            // nearCoC
            ETextureFormat::R32_FLOAT             // farCoC
        };
        desc.debugName = "DoF_DownsampleSplit_PSO";
        m_downsampleSplitPSO.reset(ctx->CreatePipelineState(desc));
    }

    // Blur Horizontal PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_blurHPS.get();
        desc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        desc.debugName = "DoF_BlurH_PSO";
        m_blurHPSO.reset(ctx->CreatePipelineState(desc));
    }

    // Blur Vertical PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_blurVPS.get();
        desc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        desc.debugName = "DoF_BlurV_PSO";
        m_blurVPSO.reset(ctx->CreatePipelineState(desc));
    }

    // Composite PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_compositePS.get();
        desc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        desc.debugName = "DoF_Composite_PSO";
        m_compositePSO.reset(ctx->CreatePipelineState(desc));
    }

    CFFLog::Info("[DepthOfFieldPass] All PSOs created successfully");
    return true;
}
