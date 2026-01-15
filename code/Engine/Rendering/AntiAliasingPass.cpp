#include "AntiAliasingPass.h"
#include "SMAALookupTextures.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/SceneLightSettings.h"

using namespace RHI;

// ============================================
// Fullscreen Vertex Structure
// ============================================
struct FullscreenVertex {
    float x, y;   // Position (NDC space)
    float u, v;   // UV
};

// ============================================
// Constant Buffers
// ============================================
struct alignas(16) CB_FXAA {
    DirectX::XMFLOAT2 rcpFrame;         // 1.0 / resolution
    float subpixelQuality;              // Subpixel AA quality (0-1)
    float edgeThreshold;                // Edge detection threshold
    float edgeThresholdMin;             // Minimum edge threshold
    float _pad[3];
};

struct alignas(16) CB_SMAA {
    DirectX::XMFLOAT4 rtMetrics;        // (1/width, 1/height, width, height)
};

// ============================================
// Lifecycle
// ============================================
bool CAntiAliasingPass::Initialize() {
    if (m_initialized) return true;

    createSharedResources();
    createFXAAResources();
    createSMAAResources();

    m_initialized = true;
    CFFLog::Info("[AntiAliasing] Initialized (FXAA + SMAA)");
    return true;
}

void CAntiAliasingPass::Shutdown() {
    // Shared resources
    m_fullscreenQuadVB.reset();
    m_linearSampler.reset();
    m_pointSampler.reset();
    m_fullscreenVS.reset();

    // FXAA resources
    m_fxaaPS.reset();
    m_fxaaPSO.reset();

    // SMAA resources
    m_smaaEdgePS.reset();
    m_smaaEdgePSO.reset();
    m_smaaEdgesTex.reset();

    m_smaaBlendPS.reset();
    m_smaaBlendPSO.reset();
    m_smaaBlendTex.reset();

    m_smaaNeighborPS.reset();
    m_smaaNeighborPSO.reset();

    m_smaaAreaTex.reset();
    m_smaaSearchTex.reset();

    m_cachedWidth = 0;
    m_cachedHeight = 0;
    m_initialized = false;

    CFFLog::Info("[AntiAliasing] Shutdown");
}

// ============================================
// Rendering
// ============================================
void CAntiAliasingPass::Render(ITexture* inputTexture,
                               ITexture* outputTexture,
                               uint32_t width, uint32_t height,
                               const SAntiAliasingSettings& settings) {
    if (!m_initialized || !inputTexture || !outputTexture) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    switch (settings.mode) {
        case EAntiAliasingMode::FXAA:
            renderFXAA(cmdList, inputTexture, outputTexture, width, height, settings);
            break;
        case EAntiAliasingMode::SMAA:
            renderSMAA(cmdList, inputTexture, outputTexture, width, height, settings);
            break;
        case EAntiAliasingMode::Off:
        default:
            // No AA - should not reach here if IsEnabled() is checked
            break;
    }
}

bool CAntiAliasingPass::IsEnabled(const SAntiAliasingSettings& settings) const {
    return settings.mode != EAntiAliasingMode::Off;
}

// ============================================
// FXAA Implementation
// ============================================
void CAntiAliasingPass::renderFXAA(ICommandList* cmdList,
                                   ITexture* input, ITexture* output,
                                   uint32_t width, uint32_t height,
                                   const SAntiAliasingSettings& settings) {
    if (!m_fxaaPSO) return;

    CScopedDebugEvent evt(cmdList, L"FXAA");

    // Unbind render targets before using input as SRV
    cmdList->UnbindRenderTargets();

    // Update constant buffer
    CB_FXAA cb;
    cb.rcpFrame = { 1.0f / width, 1.0f / height };
    cb.subpixelQuality = settings.fxaaSubpixelQuality;
    cb.edgeThreshold = settings.fxaaEdgeThreshold;
    cb.edgeThresholdMin = settings.fxaaEdgeThresholdMin;
    cb._pad[0] = cb._pad[1] = cb._pad[2] = 0.0f;

    // Set render target
    ITexture* renderTargets[] = { output };
    cmdList->SetRenderTargets(1, renderTargets, nullptr);
    cmdList->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    // Set pipeline state
    cmdList->SetPipelineState(m_fxaaPSO.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);

    // Set vertex buffer
    cmdList->SetVertexBuffer(0, m_fullscreenQuadVB.get(), sizeof(FullscreenVertex), 0);

    // Set resources
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_FXAA));
    cmdList->SetShaderResource(EShaderStage::Pixel, 0, input);
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

    // Draw fullscreen quad
    cmdList->Draw(4, 0);

    // Cleanup
    cmdList->SetRenderTargets(0, nullptr, nullptr);
}

// ============================================
// SMAA Implementation
// ============================================
void CAntiAliasingPass::renderSMAA(ICommandList* cmdList,
                                   ITexture* input, ITexture* output,
                                   uint32_t width, uint32_t height,
                                   const SAntiAliasingSettings& settings) {
    if (!m_smaaEdgePSO || !m_smaaBlendPSO || !m_smaaNeighborPSO) return;

    // Ensure intermediate textures are allocated
    ensureSMAATextures(width, height);
    if (!m_smaaEdgesTex || !m_smaaBlendTex) return;

    CScopedDebugEvent evt(cmdList, L"SMAA");

    // Constant buffer (shared across all passes)
    CB_SMAA cb;
    cb.rtMetrics = { 1.0f / width, 1.0f / height, static_cast<float>(width), static_cast<float>(height) };

    // ============================================
    // Pass 1: Edge Detection
    // ============================================
    {
        CScopedDebugEvent evt1(cmdList, L"SMAA Edge Detection");

        cmdList->UnbindRenderTargets();

        // Clear edge texture
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ITexture* edgeRT = m_smaaEdgesTex.get();
        cmdList->SetRenderTargets(1, &edgeRT, nullptr);
        cmdList->ClearRenderTarget(m_smaaEdgesTex.get(), clearColor);

        cmdList->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, width, height);

        cmdList->SetPipelineState(m_smaaEdgePSO.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_fullscreenQuadVB.get(), sizeof(FullscreenVertex), 0);

        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_SMAA));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, input);
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
        cmdList->SetSampler(EShaderStage::Pixel, 1, m_pointSampler.get());

        cmdList->Draw(4, 0);
        cmdList->SetRenderTargets(0, nullptr, nullptr);
    }

    // ============================================
    // Pass 2: Blending Weight Calculation
    // ============================================
    {
        CScopedDebugEvent evt2(cmdList, L"SMAA Blend Weight");

        cmdList->UnbindRenderTargets();

        // Clear blend texture
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ITexture* blendRT = m_smaaBlendTex.get();
        cmdList->SetRenderTargets(1, &blendRT, nullptr);
        cmdList->ClearRenderTarget(m_smaaBlendTex.get(), clearColor);

        cmdList->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, width, height);

        cmdList->SetPipelineState(m_smaaBlendPSO.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_fullscreenQuadVB.get(), sizeof(FullscreenVertex), 0);

        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_SMAA));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, m_smaaEdgesTex.get());
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, m_smaaAreaTex.get());
        cmdList->SetShaderResource(EShaderStage::Pixel, 2, m_smaaSearchTex.get());
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
        cmdList->SetSampler(EShaderStage::Pixel, 1, m_pointSampler.get());

        cmdList->Draw(4, 0);
        cmdList->SetRenderTargets(0, nullptr, nullptr);
    }

    // ============================================
    // Pass 3: Neighborhood Blending
    // ============================================
    {
        CScopedDebugEvent evt3(cmdList, L"SMAA Neighborhood Blend");

        cmdList->UnbindRenderTargets();

        ITexture* outputRT = output;
        cmdList->SetRenderTargets(1, &outputRT, nullptr);
        cmdList->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
        cmdList->SetScissorRect(0, 0, width, height);

        cmdList->SetPipelineState(m_smaaNeighborPSO.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);
        cmdList->SetVertexBuffer(0, m_fullscreenQuadVB.get(), sizeof(FullscreenVertex), 0);

        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_SMAA));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, input);
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, m_smaaBlendTex.get());
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
        cmdList->SetSampler(EShaderStage::Pixel, 1, m_pointSampler.get());

        cmdList->Draw(4, 0);
        cmdList->SetRenderTargets(0, nullptr, nullptr);
    }
}

// ============================================
// Resource Creation
// ============================================
void CAntiAliasingPass::createSharedResources() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Create fullscreen quad vertex buffer
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
    m_fullscreenQuadVB.reset(ctx->CreateBuffer(vbDesc, vertices));

    // Create samplers
    SamplerDesc linearSamplerDesc;
    linearSamplerDesc.filter = EFilter::MinMagMipLinear;
    linearSamplerDesc.addressU = ETextureAddressMode::Clamp;
    linearSamplerDesc.addressV = ETextureAddressMode::Clamp;
    linearSamplerDesc.addressW = ETextureAddressMode::Clamp;
    m_linearSampler.reset(ctx->CreateSampler(linearSamplerDesc));

    SamplerDesc pointSamplerDesc;
    pointSamplerDesc.filter = EFilter::MinMagMipPoint;
    pointSamplerDesc.addressU = ETextureAddressMode::Clamp;
    pointSamplerDesc.addressV = ETextureAddressMode::Clamp;
    pointSamplerDesc.addressW = ETextureAddressMode::Clamp;
    m_pointSampler.reset(ctx->CreateSampler(pointSamplerDesc));

    // Compile shared vertex shader
    std::string vsPath = FFPath::GetSourceDir() + "/Shader/Fullscreen.vs.hlsl";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader vsCompiled = CompileShaderFromFile(vsPath, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile Fullscreen.vs.hlsl: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    m_fullscreenVS.reset(ctx->CreateShader(vsDesc));
}

void CAntiAliasingPass::createFXAAResources() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx || !m_fullscreenVS) return;

    // Compile FXAA pixel shader
    std::string psPath = FFPath::GetSourceDir() + "/Shader/FXAA.ps.hlsl";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader psCompiled = CompileShaderFromFile(psPath, "main", "ps_5_0", nullptr, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile FXAA.ps.hlsl: %s", psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    m_fxaaPS.reset(ctx->CreateShader(psDesc));

    // Create FXAA PSO
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_fullscreenVS.get();
    psoDesc.pixelShader = m_fxaaPS.get();

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
    psoDesc.debugName = "FXAA_PSO";

    m_fxaaPSO.reset(ctx->CreatePipelineState(psoDesc));

    if (m_fxaaPSO) {
        CFFLog::Info("[AntiAliasing] FXAA resources created");
    }
}

void CAntiAliasingPass::createSMAAResources() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx || !m_fullscreenVS) return;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

    // ============================================
    // Compile SMAA shaders
    // ============================================

    // Edge Detection
    SCompiledShader edgeCompiled = CompileShaderFromFile(shaderDir + "SMAAEdgeDetection.ps.hlsl", "main", "ps_5_0", nullptr, debugShaders);
    if (!edgeCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAAEdgeDetection.ps.hlsl: %s", edgeCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc edgeDesc;
    edgeDesc.type = EShaderType::Pixel;
    edgeDesc.bytecode = edgeCompiled.bytecode.data();
    edgeDesc.bytecodeSize = edgeCompiled.bytecode.size();
    m_smaaEdgePS.reset(ctx->CreateShader(edgeDesc));

    // Blending Weight
    SCompiledShader blendCompiled = CompileShaderFromFile(shaderDir + "SMAABlendingWeight.ps.hlsl", "main", "ps_5_0", nullptr, debugShaders);
    if (!blendCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAABlendingWeight.ps.hlsl: %s", blendCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc blendDesc;
    blendDesc.type = EShaderType::Pixel;
    blendDesc.bytecode = blendCompiled.bytecode.data();
    blendDesc.bytecodeSize = blendCompiled.bytecode.size();
    m_smaaBlendPS.reset(ctx->CreateShader(blendDesc));

    // Neighborhood Blending
    SCompiledShader neighborCompiled = CompileShaderFromFile(shaderDir + "SMAANeighborhoodBlend.ps.hlsl", "main", "ps_5_0", nullptr, debugShaders);
    if (!neighborCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAANeighborhoodBlend.ps.hlsl: %s", neighborCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc neighborDesc;
    neighborDesc.type = EShaderType::Pixel;
    neighborDesc.bytecode = neighborCompiled.bytecode.data();
    neighborDesc.bytecodeSize = neighborCompiled.bytecode.size();
    m_smaaNeighborPS.reset(ctx->CreateShader(neighborDesc));

    // ============================================
    // Create SMAA PSOs
    // ============================================

    // Base PSO description (shared settings)
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
    basePsoDesc.blend.blendEnable = false;
    basePsoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;
    basePsoDesc.depthStencilFormat = ETextureFormat::Unknown;

    // Edge Detection PSO (RG8 output)
    PipelineStateDesc edgePsoDesc = basePsoDesc;
    edgePsoDesc.pixelShader = m_smaaEdgePS.get();
    edgePsoDesc.renderTargetFormats = { ETextureFormat::R8G8_UNORM };
    edgePsoDesc.debugName = "SMAA_EdgeDetection_PSO";
    m_smaaEdgePSO.reset(ctx->CreatePipelineState(edgePsoDesc));

    // Blending Weight PSO (RGBA8 output)
    PipelineStateDesc blendPsoDesc = basePsoDesc;
    blendPsoDesc.pixelShader = m_smaaBlendPS.get();
    blendPsoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM };
    blendPsoDesc.debugName = "SMAA_BlendWeight_PSO";
    m_smaaBlendPSO.reset(ctx->CreatePipelineState(blendPsoDesc));

    // Neighborhood Blending PSO (SRGB output)
    PipelineStateDesc neighborPsoDesc = basePsoDesc;
    neighborPsoDesc.pixelShader = m_smaaNeighborPS.get();
    neighborPsoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    neighborPsoDesc.debugName = "SMAA_NeighborBlend_PSO";
    m_smaaNeighborPSO.reset(ctx->CreatePipelineState(neighborPsoDesc));

    // ============================================
    // Create SMAA Lookup Textures
    // ============================================

    // Area Texture (160x560 RG8)
    TextureDesc areaTexDesc;
    areaTexDesc.width = SMAALookupTextures::AREATEX_WIDTH;
    areaTexDesc.height = SMAALookupTextures::AREATEX_HEIGHT;
    areaTexDesc.format = ETextureFormat::R8G8_UNORM;
    areaTexDesc.usage = ETextureUsage::ShaderResource;
    areaTexDesc.debugName = "SMAA_AreaTex";

    const uint8_t* areaData = SMAALookupTextures::GetAreaTexData();
    m_smaaAreaTex.reset(ctx->CreateTexture(areaTexDesc, areaData));

    // Search Texture (64x16 R8)
    TextureDesc searchTexDesc;
    searchTexDesc.width = SMAALookupTextures::SEARCHTEX_WIDTH;
    searchTexDesc.height = SMAALookupTextures::SEARCHTEX_HEIGHT;
    searchTexDesc.format = ETextureFormat::R8_UNORM;
    searchTexDesc.usage = ETextureUsage::ShaderResource;
    searchTexDesc.debugName = "SMAA_SearchTex";

    const uint8_t* searchData = SMAALookupTextures::GetSearchTexData();
    m_smaaSearchTex.reset(ctx->CreateTexture(searchTexDesc, searchData));

    if (m_smaaEdgePSO && m_smaaBlendPSO && m_smaaNeighborPSO && m_smaaAreaTex && m_smaaSearchTex) {
        CFFLog::Info("[AntiAliasing] SMAA resources created");
    }
}

void CAntiAliasingPass::ensureSMAATextures(uint32_t width, uint32_t height) {
    if (width == m_cachedWidth && height == m_cachedHeight &&
        m_smaaEdgesTex && m_smaaBlendTex) {
        return;  // Already allocated at correct size
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    m_cachedWidth = width;
    m_cachedHeight = height;

    // Edge Texture (RG8)
    TextureDesc edgeTexDesc;
    edgeTexDesc.width = width;
    edgeTexDesc.height = height;
    edgeTexDesc.format = ETextureFormat::R8G8_UNORM;
    edgeTexDesc.usage = ETextureUsage::RenderTarget | ETextureUsage::ShaderResource;
    edgeTexDesc.debugName = "SMAA_EdgesTex";
    m_smaaEdgesTex.reset(ctx->CreateTexture(edgeTexDesc, nullptr));

    // Blend Weight Texture (RGBA8)
    TextureDesc blendTexDesc;
    blendTexDesc.width = width;
    blendTexDesc.height = height;
    blendTexDesc.format = ETextureFormat::R8G8B8A8_UNORM;
    blendTexDesc.usage = ETextureUsage::RenderTarget | ETextureUsage::ShaderResource;
    blendTexDesc.debugName = "SMAA_BlendTex";
    m_smaaBlendTex.reset(ctx->CreateTexture(blendTexDesc, nullptr));

    CFFLog::Info("[AntiAliasing] SMAA intermediate textures resized to %ux%u", width, height);
}
