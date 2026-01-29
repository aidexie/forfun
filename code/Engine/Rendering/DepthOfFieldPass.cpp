#include "DepthOfFieldPass.h"
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

    initDescriptorSets();

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

    m_vertexBuffer.reset();
    m_linearSampler.reset();
    m_pointSampler.reset();

    // Cleanup DS resources
    m_fullscreenVS_ds.reset();
    m_cocPS_ds.reset();
    m_downsampleSplitPS_ds.reset();
    m_blurHPS_ds.reset();
    m_blurVPS_ds.reset();
    m_compositePS_ds.reset();

    m_cocPSO_ds.reset();
    m_downsampleSplitPSO_ds.reset();
    m_blurHPSO_ds.reset();
    m_blurVPSO_ds.reset();
    m_compositePSO_ds.reset();

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

// ============================================
// Rendering
// ============================================

ITexture* CDepthOfFieldPass::Render(ITexture* hdrInput,
                                     ITexture* depthBuffer,
                                     float cameraNearZ, float cameraFarZ,
                                     uint32_t width, uint32_t height,
                                     const SDepthOfFieldSettings& settings) {
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

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        // Pass 1: CoC Calculation (full-res)
        renderCoCPass_DS(cmdList, depthBuffer, cameraNearZ, cameraFarZ, width, height, settings);

        // Pass 2: Downsample + Near/Far Split (half-res)
        renderDownsampleSplitPass_DS(cmdList, hdrInput, width, height);

        // Pass 3: Horizontal Blur
        renderBlurPass_DS(cmdList, true, halfWidth, halfHeight, settings);

        // Pass 4: Vertical Blur
        renderBlurPass_DS(cmdList, false, halfWidth, halfHeight, settings);

        // Pass 5: Composite (full-res)
        renderCompositePass_DS(cmdList, hdrInput, width, height);

        return m_outputHDR.get();
    }
    CFFLog::Warning("[DepthOfFieldPass] Legacy binding disabled and descriptor sets not available");
    return hdrInput;
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

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CDepthOfFieldPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[DepthOfFieldPass] DX11 mode - descriptor sets not supported");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/DoF_DS.ps.hlsl";

    // Create PerPass layout for DoF
    // DoF uses: CB (b0), up to 6 textures (t0-t5), 2 samplers (s0-s1)
    BindingLayoutDesc layoutDesc("DoF_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, 64));  // CB_DoF (max 64 bytes)
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));      // Input texture 0
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(1));      // Input texture 1
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(2));      // Input texture 2
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(3));      // Input texture 3
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(4));      // Input texture 4
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(5));      // Input texture 5
    layoutDesc.AddItem(BindingLayoutItem::Sampler(0));          // Linear sampler
    layoutDesc.AddItem(BindingLayoutItem::Sampler(1));          // Point sampler

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[DepthOfFieldPass] Failed to create PerPass layout");
        return;
    }

    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[DepthOfFieldPass] Failed to allocate PerPass set");
        return;
    }

    // Bind static samplers
    m_perPassSet->Bind(BindingSetItem::Sampler(0, m_linearSampler.get()));
    m_perPassSet->Bind(BindingSetItem::Sampler(1, m_pointSampler.get()));

    // Compile SM 5.1 shaders
    // Vertex shader (shared for all passes)
    {
        std::string vsPath = FFPath::GetSourceDir() + "/Shader/Fullscreen_DS.vs.hlsl";
        SCompiledShader compiled = CompileShaderFromFile(vsPath, "main", "vs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] Fullscreen_DS.vs.hlsl (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Vertex;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "DoF_DS_VS";
        m_fullscreenVS_ds.reset(ctx->CreateShader(desc));
    }

    // CoC pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSCoC", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] PSCoC (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "DoF_DS_CoC_PS";
        m_cocPS_ds.reset(ctx->CreateShader(desc));
    }

    // Downsample + Split pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSDownsampleSplit", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] PSDownsampleSplit (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "DoF_DS_DownsampleSplit_PS";
        m_downsampleSplitPS_ds.reset(ctx->CreateShader(desc));
    }

    // Blur Horizontal pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSBlurH", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] PSBlurH (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "DoF_DS_BlurH_PS";
        m_blurHPS_ds.reset(ctx->CreateShader(desc));
    }

    // Blur Vertical pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSBlurV", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] PSBlurV (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "DoF_DS_BlurV_PS";
        m_blurVPS_ds.reset(ctx->CreateShader(desc));
    }

    // Composite pixel shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "PSComposite", "ps_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[DepthOfFieldPass] PSComposite (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc desc;
        desc.type = EShaderType::Pixel;
        desc.bytecode = compiled.bytecode.data();
        desc.bytecodeSize = compiled.bytecode.size();
        desc.debugName = "DoF_DS_Composite_PS";
        m_compositePS_ds.reset(ctx->CreateShader(desc));
    }

    // Create PSOs with descriptor set layouts
    auto createBasePSODesc = [this]() {
        PipelineStateDesc desc;
        desc.vertexShader = m_fullscreenVS_ds.get();
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
        desc.setLayouts[1] = m_perPassLayout;  // Set 1: PerPass (space1)
        return desc;
    };

    // CoC PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_cocPS_ds.get();
        desc.renderTargetFormats = { ETextureFormat::R32_FLOAT };
        desc.debugName = "DoF_DS_CoC_PSO";
        m_cocPSO_ds.reset(ctx->CreatePipelineState(desc));
    }

    // Downsample + Split PSO (4 render targets)
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_downsampleSplitPS_ds.get();
        desc.renderTargetFormats = {
            ETextureFormat::R16G16B16A16_FLOAT,  // nearColor
            ETextureFormat::R16G16B16A16_FLOAT,  // farColor
            ETextureFormat::R32_FLOAT,            // nearCoC
            ETextureFormat::R32_FLOAT             // farCoC
        };
        desc.debugName = "DoF_DS_DownsampleSplit_PSO";
        m_downsampleSplitPSO_ds.reset(ctx->CreatePipelineState(desc));
    }

    // Blur Horizontal PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_blurHPS_ds.get();
        desc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        desc.debugName = "DoF_DS_BlurH_PSO";
        m_blurHPSO_ds.reset(ctx->CreatePipelineState(desc));
    }

    // Blur Vertical PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_blurVPS_ds.get();
        desc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        desc.debugName = "DoF_DS_BlurV_PSO";
        m_blurVPSO_ds.reset(ctx->CreatePipelineState(desc));
    }

    // Composite PSO
    {
        PipelineStateDesc desc = createBasePSODesc();
        desc.pixelShader = m_compositePS_ds.get();
        desc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
        desc.debugName = "DoF_DS_Composite_PSO";
        m_compositePSO_ds.reset(ctx->CreatePipelineState(desc));
    }

    CFFLog::Info("[DepthOfFieldPass] Descriptor set resources initialized");
}

// ============================================
// Pass Implementations (Descriptor Set Binding)
// ============================================

void CDepthOfFieldPass::renderCoCPass_DS(ICommandList* cmdList, ITexture* depthBuffer,
                                          float nearZ, float farZ, uint32_t width, uint32_t height,
                                          const SDepthOfFieldSettings& settings) {
    cmdList->UnbindRenderTargets();

    ITexture* rt = m_cocBuffer.get();
    cmdList->SetRenderTargets(1, &rt, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    cmdList->SetPipelineState(m_cocPSO_ds.get());
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

    // Bind PerPass descriptor set
    m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(SCBCoC)));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, depthBuffer));
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Draw(4, 0);
    cmdList->UnbindRenderTargets();
}

void CDepthOfFieldPass::renderDownsampleSplitPass_DS(ICommandList* cmdList, ITexture* hdrInput,
                                                      uint32_t width, uint32_t height) {
    cmdList->UnbindRenderTargets();

    uint32_t halfWidth = std::max(1u, width / 2);
    uint32_t halfHeight = std::max(1u, height / 2);

    ITexture* rts[4] = { m_nearColor.get(), m_farColor.get(), m_nearCoc.get(), m_farCoc.get() };
    cmdList->SetRenderTargets(4, rts, nullptr);
    cmdList->SetViewport(0, 0, (float)halfWidth, (float)halfHeight, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, halfWidth, halfHeight);

    cmdList->SetPipelineState(m_downsampleSplitPSO_ds.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

    // Bind PerPass descriptor set (no CB needed for this pass)
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, hdrInput));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(1, m_cocBuffer.get()));
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Draw(4, 0);
    cmdList->UnbindRenderTargets();
}

void CDepthOfFieldPass::renderBlurPass_DS(ICommandList* cmdList, bool horizontal,
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

    SCBBlur cb;
    cb.texelSizeX = 1.0f / static_cast<float>(halfWidth);
    cb.texelSizeY = 1.0f / static_cast<float>(halfHeight);
    cb.maxCoCRadius = settings.maxBlurRadius;
    cb.sampleCount = 11;

    // Blur near layer
    {
        ITexture* rt = nearOutput;
        cmdList->SetRenderTargets(1, &rt, nullptr);
        cmdList->SetViewport(0, 0, (float)halfWidth, (float)halfHeight, 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, halfWidth, halfHeight);

        cmdList->SetPipelineState(horizontal ? m_blurHPSO_ds.get() : m_blurVPSO_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

        // Bind PerPass descriptor set
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(SCBBlur)));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, nearInput));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(1, nearCocInput));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        cmdList->Draw(4, 0);
    }

    cmdList->UnbindRenderTargets();

    // Blur far layer
    {
        ITexture* rt = farOutput;
        cmdList->SetRenderTargets(1, &rt, nullptr);
        cmdList->SetViewport(0, 0, (float)halfWidth, (float)halfHeight, 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, halfWidth, halfHeight);

        cmdList->SetPipelineState(horizontal ? m_blurHPSO_ds.get() : m_blurVPSO_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

        // Bind PerPass descriptor set
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(SCBBlur)));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, farInput));
        m_perPassSet->Bind(BindingSetItem::Texture_SRV(1, farCocInput));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        cmdList->Draw(4, 0);
    }

    cmdList->UnbindRenderTargets();
}

void CDepthOfFieldPass::renderCompositePass_DS(ICommandList* cmdList, ITexture* hdrInput,
                                                uint32_t width, uint32_t height) {
    cmdList->UnbindRenderTargets();

    ITexture* rt = m_outputHDR.get();
    cmdList->SetRenderTargets(1, &rt, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    cmdList->SetPipelineState(m_compositePSO_ds.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SDoFVertex), 0);

    SCBComposite cb;
    cb.texelSizeX = 1.0f / static_cast<float>(width);
    cb.texelSizeY = 1.0f / static_cast<float>(height);
    cb._pad[0] = 0.0f;
    cb._pad[1] = 0.0f;

    // Bind PerPass descriptor set
    m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(SCBComposite)));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(0, hdrInput));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(1, m_cocBuffer.get()));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(2, m_nearColor.get()));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(3, m_farColor.get()));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(4, m_nearCoc.get()));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(5, m_farCoc.get()));
    cmdList->BindDescriptorSet(1, m_perPassSet);

    cmdList->Draw(4, 0);
    cmdList->UnbindRenderTargets();
}
