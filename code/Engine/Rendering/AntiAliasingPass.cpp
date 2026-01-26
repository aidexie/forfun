#include "AntiAliasingPass.h"
#include "PassLayouts.h"
#include "SMAALookupTextures.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/SceneLightSettings.h"

using namespace RHI;

namespace {

#if defined(_DEBUG)
constexpr bool kDebugShaders = true;
#else
constexpr bool kDebugShaders = false;
#endif

struct FullscreenVertex {
    float x, y;
    float u, v;
};

struct alignas(16) CB_FXAA {
    DirectX::XMFLOAT2 rcpFrame;
    float subpixelQuality;
    float edgeThreshold;
    float edgeThresholdMin;
    float _pad[3];
};

struct alignas(16) CB_SMAA {
    DirectX::XMFLOAT4 rtMetrics;  // (1/width, 1/height, width, height)
};

} // anonymous namespace

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
    if (m_fxaaDescSet || m_fxaaLayout) {
        auto* renderCtx = CRHIManager::Instance().GetRenderContext();
        if (renderCtx) {
            if (m_fxaaDescSet) {
                renderCtx->FreeDescriptorSet(m_fxaaDescSet);
                m_fxaaDescSet = nullptr;
            }
            if (m_fxaaLayout) {
                renderCtx->DestroyDescriptorSetLayout(m_fxaaLayout);
                m_fxaaLayout = nullptr;
            }
        }
    }

    // SMAA resources
    m_smaaEdgeVS.reset();
    m_smaaEdgePS.reset();
    m_smaaEdgePSO.reset();
    m_smaaEdgesTex.reset();

    m_smaaBlendVS.reset();
    m_smaaBlendPS.reset();
    m_smaaBlendPSO.reset();
    m_smaaBlendTex.reset();

    m_smaaNeighborVS.reset();
    m_smaaNeighborPS.reset();
    m_smaaNeighborPSO.reset();

    m_smaaAreaTex.reset();
    m_smaaSearchTex.reset();

    m_cachedWidth = 0;
    m_cachedHeight = 0;
    m_initialized = false;

    CFFLog::Info("[AntiAliasing] Shutdown");
}

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

    // Use descriptor set binding if available (DX12), else fall back to legacy
    if (m_fxaaDescSet) {
        // Update bindings on the persistent descriptor set
        m_fxaaDescSet->Bind({
            BindingSetItem::VolatileCBV(0, &cb, sizeof(CB_FXAA)),
            BindingSetItem::Texture_SRV(0, input),
            BindingSetItem::Sampler(0, m_linearSampler.get())
        });

        // Bind descriptor set to pipeline (Set 1 = PerPass)
        cmdList->BindDescriptorSet(1, m_fxaaDescSet);
    } else {
        // Legacy binding (DX11 fallback)
        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CB_FXAA));
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, input);
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());
    }

    // Draw fullscreen quad
    cmdList->Draw(4, 0);

    // Cleanup
    cmdList->SetRenderTargets(0, nullptr, nullptr);
}

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

    // Pass 1: Edge Detection
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

    // Pass 2: Blending Weight Calculation
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

    // Pass 3: Neighborhood Blending
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
    SCompiledShader vsCompiled = CompileShaderFromFile(vsPath, "main", "vs_5_0", nullptr, kDebugShaders);
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

    // Create descriptor set layout for FXAA (PerPass = Set 1 = space1)
    // Uses PassLayouts factory with PerPassSlots constants
    m_fxaaLayout = PassLayouts::CreateFXAALayout(ctx, sizeof(CB_FXAA));

    // Allocate persistent descriptor set
    if (m_fxaaLayout) {
        m_fxaaDescSet = ctx->AllocateDescriptorSet(m_fxaaLayout);
    }

    // Compile shader with SM 5.1 for register spaces
    std::string psPath = FFPath::GetSourceDir() + "/Shader/FXAA.ps.hlsl";
    SCompiledShader psCompiled = CompileShaderFromFile(psPath, "main", "ps_5_1", nullptr, kDebugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile FXAA.ps.hlsl: %s", psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    m_fxaaPS.reset(ctx->CreateShader(psDesc));

    // Create FXAA PSO with descriptor set layout
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_fullscreenVS.get();
    psoDesc.pixelShader = m_fxaaPS.get();

    // Set descriptor set layouts (Set 1 = PerPass)
    psoDesc.setLayouts[0] = nullptr;        // Set 0: PerFrame (not used)
    psoDesc.setLayouts[1] = m_fxaaLayout;   // Set 1: PerPass (FXAA bindings)
    psoDesc.setLayouts[2] = nullptr;        // Set 2: PerMaterial (not used)
    psoDesc.setLayouts[3] = nullptr;        // Set 3: PerDraw (not used)

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
        CFFLog::Info("[AntiAliasing] FXAA resources created (descriptor set binding)");
    }
}

void CAntiAliasingPass::createSMAAResources() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

    // Include handler for SMAA.hlsl
    CDefaultShaderIncludeHandler includeHandler(shaderDir);

    // Compile SMAA Edge Detection shaders (VS + PS)
    SCompiledShader edgeVSCompiled = CompileShaderFromFile(shaderDir + "SMAAEdgeDetection.ps.hlsl", "VSMain", "vs_5_0", &includeHandler, kDebugShaders);
    if (!edgeVSCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAAEdgeDetection VS: %s", edgeVSCompiled.errorMessage.c_str());
        return;
    }
    ShaderDesc edgeVSDesc;
    edgeVSDesc.type = EShaderType::Vertex;
    edgeVSDesc.bytecode = edgeVSCompiled.bytecode.data();
    edgeVSDesc.bytecodeSize = edgeVSCompiled.bytecode.size();
    m_smaaEdgeVS.reset(ctx->CreateShader(edgeVSDesc));

    SCompiledShader edgePSCompiled = CompileShaderFromFile(shaderDir + "SMAAEdgeDetection.ps.hlsl", "main", "ps_5_0", &includeHandler, kDebugShaders);
    if (!edgePSCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAAEdgeDetection PS: %s", edgePSCompiled.errorMessage.c_str());
        return;
    }
    ShaderDesc edgePSDesc;
    edgePSDesc.type = EShaderType::Pixel;
    edgePSDesc.bytecode = edgePSCompiled.bytecode.data();
    edgePSDesc.bytecodeSize = edgePSCompiled.bytecode.size();
    m_smaaEdgePS.reset(ctx->CreateShader(edgePSDesc));

    // Compile SMAA Blending Weight shaders (VS + PS)
    SCompiledShader blendVSCompiled = CompileShaderFromFile(shaderDir + "SMAABlendingWeight.ps.hlsl", "VSMain", "vs_5_0", &includeHandler, kDebugShaders);
    if (!blendVSCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAABlendingWeight VS: %s", blendVSCompiled.errorMessage.c_str());
        return;
    }
    ShaderDesc blendVSDesc;
    blendVSDesc.type = EShaderType::Vertex;
    blendVSDesc.bytecode = blendVSCompiled.bytecode.data();
    blendVSDesc.bytecodeSize = blendVSCompiled.bytecode.size();
    m_smaaBlendVS.reset(ctx->CreateShader(blendVSDesc));

    SCompiledShader blendPSCompiled = CompileShaderFromFile(shaderDir + "SMAABlendingWeight.ps.hlsl", "main", "ps_5_0", &includeHandler, kDebugShaders);
    if (!blendPSCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAABlendingWeight PS: %s", blendPSCompiled.errorMessage.c_str());
        return;
    }
    ShaderDesc blendPSDesc;
    blendPSDesc.type = EShaderType::Pixel;
    blendPSDesc.bytecode = blendPSCompiled.bytecode.data();
    blendPSDesc.bytecodeSize = blendPSCompiled.bytecode.size();
    m_smaaBlendPS.reset(ctx->CreateShader(blendPSDesc));

    // Compile SMAA Neighborhood Blending shaders (VS + PS)
    SCompiledShader neighborVSCompiled = CompileShaderFromFile(shaderDir + "SMAANeighborhoodBlend.ps.hlsl", "VSMain", "vs_5_0", &includeHandler, kDebugShaders);
    if (!neighborVSCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAANeighborhoodBlend VS: %s", neighborVSCompiled.errorMessage.c_str());
        return;
    }
    ShaderDesc neighborVSDesc;
    neighborVSDesc.type = EShaderType::Vertex;
    neighborVSDesc.bytecode = neighborVSCompiled.bytecode.data();
    neighborVSDesc.bytecodeSize = neighborVSCompiled.bytecode.size();
    m_smaaNeighborVS.reset(ctx->CreateShader(neighborVSDesc));

    SCompiledShader neighborPSCompiled = CompileShaderFromFile(shaderDir + "SMAANeighborhoodBlend.ps.hlsl", "main", "ps_5_0", &includeHandler, kDebugShaders);
    if (!neighborPSCompiled.success) {
        CFFLog::Error("[AntiAliasing] Failed to compile SMAANeighborhoodBlend PS: %s", neighborPSCompiled.errorMessage.c_str());
        return;
    }
    ShaderDesc neighborPSDesc;
    neighborPSDesc.type = EShaderType::Pixel;
    neighborPSDesc.bytecode = neighborPSCompiled.bytecode.data();
    neighborPSDesc.bytecodeSize = neighborPSCompiled.bytecode.size();
    m_smaaNeighborPS.reset(ctx->CreateShader(neighborPSDesc));

    // Create SMAA PSOs - base description shared across all passes
    PipelineStateDesc basePsoDesc;
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
    edgePsoDesc.vertexShader = m_smaaEdgeVS.get();
    edgePsoDesc.pixelShader = m_smaaEdgePS.get();
    edgePsoDesc.renderTargetFormats = { ETextureFormat::R8G8_UNORM };
    edgePsoDesc.debugName = "SMAA_EdgeDetection_PSO";
    m_smaaEdgePSO.reset(ctx->CreatePipelineState(edgePsoDesc));

    // Blending Weight PSO (RGBA8 output)
    PipelineStateDesc blendPsoDesc = basePsoDesc;
    blendPsoDesc.vertexShader = m_smaaBlendVS.get();
    blendPsoDesc.pixelShader = m_smaaBlendPS.get();
    blendPsoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM };
    blendPsoDesc.debugName = "SMAA_BlendWeight_PSO";
    m_smaaBlendPSO.reset(ctx->CreatePipelineState(blendPsoDesc));

    // Neighborhood Blending PSO (SRGB output)
    PipelineStateDesc neighborPsoDesc = basePsoDesc;
    neighborPsoDesc.vertexShader = m_smaaNeighborVS.get();
    neighborPsoDesc.pixelShader = m_smaaNeighborPS.get();
    neighborPsoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    neighborPsoDesc.debugName = "SMAA_NeighborBlend_PSO";
    m_smaaNeighborPSO.reset(ctx->CreatePipelineState(neighborPsoDesc));

    // Create SMAA lookup textures
    TextureDesc areaTexDesc;
    areaTexDesc.width = SMAALookupTextures::AREATEX_WIDTH;
    areaTexDesc.height = SMAALookupTextures::AREATEX_HEIGHT;
    areaTexDesc.format = ETextureFormat::R8G8_UNORM;
    areaTexDesc.usage = ETextureUsage::ShaderResource;
    areaTexDesc.debugName = "SMAA_AreaTex";

    const uint8_t* areaData = SMAALookupTextures::GetAreaTexData();
    m_smaaAreaTex.reset(ctx->CreateTexture(areaTexDesc, areaData));

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
