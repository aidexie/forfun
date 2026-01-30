#include "GBufferPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerDrawSlots.h"
#include "RHI/ShaderCompiler.h"
#include "Engine/Material/MaterialConstants.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Core/PathManager.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Core/MaterialManager.h"
#include "Core/TextureManager.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

// ============================================
// Constant Buffer Structures
// ============================================
namespace {

struct alignas(16) CB_GBufferFrame {
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX viewProjPrev;
    XMFLOAT3 camPosWS;
    float _pad0;
};

struct alignas(16) CB_GBufferObject {
    XMMATRIX world;
    XMMATRIX worldPrev;
    XMFLOAT3 albedo; float metallic;
    XMFLOAT3 emissive; float roughness;
    float emissiveStrength;
    int hasMetallicRoughnessTexture;
    int hasEmissiveMap;
    int alphaMode;
    float alphaCutoff;
    int lightmapIndex;
    float materialID;
    float _padObj;
};

std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // anonymous namespace

CGBufferPass::~CGBufferPass()
{
    Shutdown();
}

bool CGBufferPass::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Shader directory
    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

    // Compile vertex shader
    std::string vsSource = LoadShaderSource(shaderDir + "GBuffer.vs.hlsl");
    if (vsSource.empty()) {
        CFFLog::Error("GBufferPass: Failed to load vertex shader");
        return false;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("GBufferPass VS compilation error: %s", vsCompiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "GBuffer_VS";
    m_vs.reset(ctx->CreateShader(vsDesc));

    // Compile pixel shader with include path for Lightmap2D.hlsl
    std::string psSource = LoadShaderSource(shaderDir + "GBuffer.ps.hlsl");
    if (psSource.empty()) {
        CFFLog::Error("GBufferPass: Failed to load pixel shader");
        return false;
    }

    // Use include handler for Lightmap2D.hlsl
    CDefaultShaderIncludeHandler includeHandler(shaderDir);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_0", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("GBufferPass PS compilation error: %s", psCompiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "GBuffer_PS";
    m_ps.reset(ctx->CreateShader(psDesc));

    // Create pipeline state
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();

    // Input layout (matches SVertexPNT)
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 },
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 },
        { EVertexSemantic::Texcoord, 1, EVertexFormat::Float2, 64, 0 }
    };

    // Rasterizer state
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.cullMode = ECullMode::Back;
    psoDesc.rasterizer.depthClipEnable = true;

    // Depth bias: compensate for FP precision difference between passes
    // DepthPrePass uses: posWS * ViewProj (single matrix multiply)
    // GBuffer uses: (posWS * View) * Proj (two matrix multiplies)
    // Matrix multiplication is NOT associative in FP: (A*B)*C ≠ A*(B*C)
    // The extra multiply causes GBuffer depth to be slightly different than pre-pass
    // Depth bias pushes GBuffer depth to match pre-pass value
    // For reversed-Z, we need positive bias (opposite direction)
    int depthBias = UseReversedZ() ? 1 : -1;
    float slopeScaledBias = UseReversedZ() ? 1.0f : -1.0f;
    psoDesc.rasterizer.depthBias = depthBias;
    psoDesc.rasterizer.slopeScaledDepthBias = slopeScaledBias;

    // Depth stencil state: test (with bias, effectively matches pre-pass depth)
    // Write OFF since depth was already written by DepthPrePass
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual

    // No blending
    psoDesc.blend.blendEnable = false;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    // 5 render targets (G-Buffer layout)
    psoDesc.renderTargetFormats = {
        ETextureFormat::R16G16B16A16_FLOAT,   // RT0: WorldPosMetallic
        ETextureFormat::R16G16B16A16_FLOAT,   // RT1: NormalRoughness
        ETextureFormat::R8G8B8A8_UNORM_SRGB,  // RT2: AlbedoAO
        ETextureFormat::R16G16B16A16_FLOAT,   // RT3: EmissiveMaterialID
        ETextureFormat::R16G16_FLOAT          // RT4: Velocity
    };
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;

    psoDesc.debugName = "GBufferPass_PSO";
    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    if (!m_pso) {
        CFFLog::Error("Failed to create GBufferPass PSO");
        return false;
    }

    // Create sampler
    SamplerDesc sampDesc;
    sampDesc.filter = EFilter::MinMagMipLinear;
    sampDesc.addressU = ETextureAddressMode::Wrap;
    sampDesc.addressV = ETextureAddressMode::Wrap;
    sampDesc.addressW = ETextureAddressMode::Wrap;
    m_sampler.reset(ctx->CreateSampler(sampDesc));

    // Initialize descriptor set resources (DX12 only)
    initDescriptorSets();

    CFFLog::Info("GBufferPass initialized");
    return true;
}

void CGBufferPass::Shutdown()
{
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_sampler.reset();

    // Cleanup descriptor set resources
    m_pso_ds.reset();
    m_vs_ds.reset();
    m_ps_ds.reset();
    m_lightmapSampler.reset();
    m_materialSampler.reset();

    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perPassSet) {
            ctx->FreeDescriptorSet(m_perPassSet);
            m_perPassSet = nullptr;
        }
        if (m_perMaterialSet) {
            ctx->FreeDescriptorSet(m_perMaterialSet);
            m_perMaterialSet = nullptr;
        }
        if (m_perDrawSet) {
            ctx->FreeDescriptorSet(m_perDrawSet);
            m_perDrawSet = nullptr;
        }
        if (m_perPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_perPassLayout);
            m_perPassLayout = nullptr;
        }
        if (m_perMaterialLayout) {
            ctx->DestroyDescriptorSetLayout(m_perMaterialLayout);
            m_perMaterialLayout = nullptr;
        }
        if (m_perDrawLayout) {
            ctx->DestroyDescriptorSetLayout(m_perDrawLayout);
            m_perDrawLayout = nullptr;
        }
    }
}


// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CGBufferPass::initDescriptorSets()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[GBufferPass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";
    CDefaultShaderIncludeHandler includeHandler(shaderDir);

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile SM 5.1 vertex shader
    std::string vsSource = LoadShaderSource(shaderDir + "GBuffer_DS.vs.hlsl");
    if (vsSource.empty()) {
        CFFLog::Warning("[GBufferPass] Failed to load GBuffer_DS.vs.hlsl");
        return;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("[GBufferPass] GBuffer_DS.vs.hlsl compile error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "GBuffer_DS_VS";
    m_vs_ds.reset(ctx->CreateShader(vsDesc));

    // Compile SM 5.1 pixel shader
    std::string psSource = LoadShaderSource(shaderDir + "GBuffer_DS.ps.hlsl");
    if (psSource.empty()) {
        CFFLog::Warning("[GBufferPass] Failed to load GBuffer_DS.ps.hlsl");
        return;
    }

    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_1", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("[GBufferPass] GBuffer_DS.ps.hlsl compile error: %s", psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "GBuffer_DS_PS";
    m_ps_ds.reset(ctx->CreateShader(psDesc));

    if (!m_vs_ds || !m_ps_ds) {
        CFFLog::Error("[GBufferPass] Failed to create SM 5.1 shaders");
        return;
    }

    // Create samplers
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        m_lightmapSampler.reset(ctx->CreateSampler(desc));
    }
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Wrap;
        desc.addressV = ETextureAddressMode::Wrap;
        desc.addressW = ETextureAddressMode::Wrap;
        m_materialSampler.reset(ctx->CreateSampler(desc));
    }

    // Create PerPass layout (Set 1, space1)
    // CB_GBufferFrame (b0), Lightmap (t12), LightmapInfos (t13), Sampler (s2)
    BindingLayoutDesc perPassLayoutDesc("GBuffer_PerPass");
    perPassLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_GBufferFrame)));
    perPassLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(12));           // Lightmap atlas
    perPassLayoutDesc.AddItem(BindingLayoutItem::Buffer_SRV(13));  // Lightmap infos
    perPassLayoutDesc.AddItem(BindingLayoutItem::Sampler(2));                // Lightmap sampler

    m_perPassLayout = ctx->CreateDescriptorSetLayout(perPassLayoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[GBufferPass] Failed to create PerPass layout");
        return;
    }

    // Create PerMaterial layout (Set 2, space2)
    // CB_Material (b0), Albedo (t0), Normal (t1), MetallicRoughness (t2), Emissive (t3), Sampler (s0)
    BindingLayoutDesc perMaterialLayoutDesc("GBuffer_PerMaterial");
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(MaterialConstants::CB_Material)));
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));   // Albedo
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(1));   // Normal
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(2));   // MetallicRoughness
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(3));   // Emissive
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Sampler(0));       // Material sampler

    m_perMaterialLayout = ctx->CreateDescriptorSetLayout(perMaterialLayoutDesc);
    if (!m_perMaterialLayout) {
        CFFLog::Error("[GBufferPass] Failed to create PerMaterial layout");
        return;
    }

    // Create PerDraw layout (Set 3, space3)
    // CB_PerDraw (b0)
    BindingLayoutDesc perDrawLayoutDesc("GBuffer_PerDraw");
    perDrawLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(PerDrawSlots::CB_PerDraw)));

    m_perDrawLayout = ctx->CreateDescriptorSetLayout(perDrawLayoutDesc);
    if (!m_perDrawLayout) {
        CFFLog::Error("[GBufferPass] Failed to create PerDraw layout");
        return;
    }

    // Allocate descriptor sets
    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    m_perMaterialSet = ctx->AllocateDescriptorSet(m_perMaterialLayout);
    m_perDrawSet = ctx->AllocateDescriptorSet(m_perDrawLayout);

    if (!m_perPassSet || !m_perMaterialSet || !m_perDrawSet) {
        CFFLog::Error("[GBufferPass] Failed to allocate descriptor sets");
        return;
    }

    // Bind static samplers to PerPass set
    m_perPassSet->Bind(BindingSetItem::Sampler(2, m_lightmapSampler.get()));

    // Bind static sampler to PerMaterial set
    m_perMaterialSet->Bind(BindingSetItem::Sampler(0, m_materialSampler.get()));

    CFFLog::Info("[GBufferPass] Descriptor set resources initialized");
}

void CGBufferPass::CreatePSOWithLayouts(IDescriptorSetLayout* perFrameLayout)
{
    if (!m_perPassLayout || !perFrameLayout || !m_vs_ds || !m_ps_ds) {
        CFFLog::Warning("[GBufferPass] Cannot create PSO with layouts - missing resources");
        return;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs_ds.get();
    psoDesc.pixelShader = m_ps_ds.get();

    // Input layout (matches SVertexPNT)
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 },
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 },
        { EVertexSemantic::Texcoord, 1, EVertexFormat::Float2, 64, 0 }
    };

    // Rasterizer state
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.cullMode = ECullMode::Back;
    psoDesc.rasterizer.depthClipEnable = true;

    // Depth bias (same as legacy PSO)
    int depthBias = UseReversedZ() ? 1 : -1;
    float slopeScaledBias = UseReversedZ() ? 1.0f : -1.0f;
    psoDesc.rasterizer.depthBias = depthBias;
    psoDesc.rasterizer.slopeScaledDepthBias = slopeScaledBias;

    // Depth stencil state
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);

    // No blending
    psoDesc.blend.blendEnable = false;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    // 5 render targets (G-Buffer layout)
    psoDesc.renderTargetFormats = {
        ETextureFormat::R16G16B16A16_FLOAT,   // RT0: WorldPosMetallic
        ETextureFormat::R16G16B16A16_FLOAT,   // RT1: NormalRoughness
        ETextureFormat::R8G8B8A8_UNORM_SRGB,  // RT2: AlbedoAO
        ETextureFormat::R16G16B16A16_FLOAT,   // RT3: EmissiveMaterialID
        ETextureFormat::R16G16_FLOAT          // RT4: Velocity
    };
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;

    // Set descriptor set layouts
    // Note: GBufferPass doesn't use Set 0 (PerFrame), so we leave it as nullptr
    // This allows the PSO to work without binding perFrameSet
    psoDesc.setLayouts[0] = nullptr;             // Set 0: Not used by GBufferPass
    psoDesc.setLayouts[1] = m_perPassLayout;     // Set 1: PerPass (space1)
    psoDesc.setLayouts[2] = m_perMaterialLayout; // Set 2: PerMaterial (space2)
    psoDesc.setLayouts[3] = m_perDrawLayout;     // Set 3: PerDraw (space3)

    psoDesc.debugName = "GBufferPass_DS_PSO";
    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));

    if (m_pso_ds) {
        CFFLog::Info("[GBufferPass] PSO with descriptor set layouts created");
    } else {
        CFFLog::Error("[GBufferPass] Failed to create PSO with descriptor set layouts");
    }
}

// ============================================
// Descriptor Set Render Method (DX12)
// ============================================
void CGBufferPass::Render(
    const CCamera& camera,
    CScene& scene,
    CGBuffer& gbuffer,
    const DirectX::XMMATRIX& viewProjPrev,
    uint32_t width,
    uint32_t height,
    IDescriptorSet* perFrameSet)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Descriptor set resources must be available
    if (!m_perPassSet || !m_pso_ds) {
        CFFLog::Error("[GBufferPass] Descriptor set resources not initialized");
        return;
    }

    CScopedDebugEvent evt(cmdList, L"G-Buffer Pass (DS)");

    // Get G-Buffer render targets
    ITexture* rts[CGBuffer::RT_Count];
    uint32_t rtCount;
    gbuffer.GetRenderTargets(rts, rtCount);

    // Set render targets
    cmdList->SetRenderTargets(rtCount, rts, gbuffer.GetDepthBuffer());

    // Set viewport and scissor
    cmdList->SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    // Clear G-Buffer render targets (not depth - already populated)
    const float clearBlack[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0; i < rtCount; ++i) {
        cmdList->ClearRenderTarget(rts[i], clearBlack);
    }

    // Set pipeline state
    cmdList->SetPipelineState(m_pso_ds.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Note: Set 0 (PerFrame) is not used by GBufferPass, so we don't bind it

    // Update and bind Set 1 (PerPass)
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = camera.GetJitteredProjectionMatrix(width, height);

    CB_GBufferFrame frameData;
    frameData.view = XMMatrixTranspose(view);
    frameData.proj = XMMatrixTranspose(proj);
    frameData.viewProjPrev = XMMatrixTranspose(viewProjPrev);
    frameData.camPosWS = camera.position;

    // Get lightmap resources (may be null if no lightmap loaded)
    ITexture* lightmapAtlas = scene.GetLightmap2D().GetAtlasTexture();
    IBuffer* lightmapInfos = scene.GetLightmap2D().GetScaleOffsetBuffer();

    // Use fallback textures if lightmap not loaded
    CTextureManager& texMgr = CTextureManager::Instance();
    if (!lightmapAtlas) {
        lightmapAtlas = texMgr.GetDefaultBlack().get();
    }

    // Bind PerPass set - only bind lightmap texture if available
    // Buffer binding is optional (shader handles lightmapIndex < 0)
    m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &frameData, sizeof(frameData)));
    m_perPassSet->Bind(BindingSetItem::Texture_SRV(12, lightmapAtlas));
    if (lightmapInfos) {
        m_perPassSet->Bind(BindingSetItem::Buffer_SRV(13, lightmapInfos));
    }
    cmdList->BindDescriptorSet(1, m_perPassSet);

    // Render all opaque objects
    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) continue;

        meshRenderer->EnsureUploaded();
        if (meshRenderer->meshes.empty()) continue;

        // Get material
        CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
        if (!meshRenderer->materialPath.empty()) {
            material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
        }

        // Skip transparent objects
        if (material && material->alphaMode == EAlphaMode::Blend) {
            continue;
        }

        // Get textures
        ITexture* albedoTex = material->albedoTexture.empty() ?
            texMgr.GetDefaultWhite().get() : texMgr.LoadAsync(material->albedoTexture, true)->GetTexture();
        ITexture* normalTex = material->normalMap.empty() ?
            texMgr.GetDefaultNormal().get() : texMgr.LoadAsync(material->normalMap, false)->GetTexture();
        ITexture* metallicRoughnessTex = material->metallicRoughnessMap.empty() ?
            texMgr.GetDefaultWhite().get() : texMgr.LoadAsync(material->metallicRoughnessMap, false)->GetTexture();
        ITexture* emissiveTex = material->emissiveMap.empty() ?
            texMgr.GetDefaultBlack().get() : texMgr.LoadAsync(material->emissiveMap, true)->GetTexture();

        bool hasRealMetallicRoughnessTexture = !material->metallicRoughnessMap.empty();
        bool hasRealEmissiveMap = !material->emissiveMap.empty();

        // Bind Set 2 (PerMaterial)
        MaterialConstants::CB_Material matData;
        matData.albedo = material->albedo;
        matData.metallic = material->metallic;
        matData.emissive = material->emissive;
        matData.roughness = material->roughness;
        matData.emissiveStrength = material->emissiveStrength;
        matData.hasMetallicRoughnessTexture = hasRealMetallicRoughnessTexture ? 1 : 0;
        matData.hasEmissiveMap = hasRealEmissiveMap ? 1 : 0;
        matData.alphaMode = static_cast<int>(material->alphaMode);
        matData.alphaCutoff = material->alphaCutoff;
        matData.materialID = static_cast<float>(material->materialType);

        m_perMaterialSet->Bind({
            BindingSetItem::VolatileCBV(0, &matData, sizeof(matData)),
            BindingSetItem::Texture_SRV(0, albedoTex),
            BindingSetItem::Texture_SRV(1, normalTex),
            BindingSetItem::Texture_SRV(2, metallicRoughnessTex),
            BindingSetItem::Texture_SRV(3, emissiveTex)
        });
        cmdList->BindDescriptorSet(2, m_perMaterialSet);

        // Bind Set 3 (PerDraw)
        XMMATRIX worldMatrix = transform->WorldMatrix();

        PerDrawSlots::CB_PerDraw perDraw;
        XMStoreFloat4x4(&perDraw.World, XMMatrixTranspose(worldMatrix));
        XMStoreFloat4x4(&perDraw.WorldPrev, XMMatrixTranspose(worldMatrix));  // TODO: Track previous frame
        perDraw.lightmapIndex = meshRenderer->lightmapInfosIndex;
        perDraw.objectID = 0;  // TODO: Add object ID to CGameObject

        m_perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
        cmdList->BindDescriptorSet(3, m_perDrawSet);

        // Draw all meshes
        for (auto& gpuMesh : meshRenderer->meshes) {
            if (!gpuMesh) continue;

            cmdList->SetVertexBuffer(0, gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
            cmdList->SetIndexBuffer(gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
            cmdList->DrawIndexed(gpuMesh->indexCount, 0, 0);
        }
    }

    // Unbind render targets before transitioning
    cmdList->SetRenderTargets(0, nullptr, nullptr);

    // Transition G-Buffer textures from RenderTarget to ShaderResource for consumers
    cmdList->Barrier(gbuffer.GetAlbedoAO(), EResourceState::RenderTarget, EResourceState::ShaderResource);
    cmdList->Barrier(gbuffer.GetNormalRoughness(), EResourceState::RenderTarget, EResourceState::ShaderResource);
    cmdList->Barrier(gbuffer.GetWorldPosMetallic(), EResourceState::RenderTarget, EResourceState::ShaderResource);
    cmdList->Barrier(gbuffer.GetEmissiveMaterialID(), EResourceState::RenderTarget, EResourceState::ShaderResource);
    cmdList->Barrier(gbuffer.GetVelocity(), EResourceState::RenderTarget, EResourceState::ShaderResource);
    cmdList->Barrier(gbuffer.GetDepthBuffer(), EResourceState::DepthWrite, EResourceState::ShaderResource);
}
