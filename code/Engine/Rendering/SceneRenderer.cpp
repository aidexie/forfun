#include "SceneRenderer.h"
#include "ClusteredLightingPass.h"
#include "ShadowPass.h"
#include "ShowFlags.h"
#include "ReflectionProbeManager.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerFrameSlots.h"
#include "RHI/PerPassSlots.h"
#include "RHI/PerDrawSlots.h"
#include "RHI/ShaderCompiler.h"
#include "Engine/Material/MaterialConstants.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Core/PathManager.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Core/MaterialManager.h"
#include "Core/TextureManager.h"
#include "Engine/Camera.h"
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

// ============================================
// 辅助结构和函数（文件作用域）
// ============================================
namespace {

// RenderItem - 存储渲染一个 mesh 所需的所有数据
struct RenderItem {
    CGameObject* obj;
    SMeshRenderer* meshRenderer;
    STransform* transform;
    CMaterialAsset* material;
    XMMATRIX worldMatrix;
    float distanceToCamera;
    GpuMeshResource* gpuMesh;
    RHI::ITexture* albedoTex;
    RHI::ITexture* normalTex;
    RHI::ITexture* metallicRoughnessTex;
    RHI::ITexture* emissiveTex;
    bool hasRealMetallicRoughnessTexture;
    bool hasRealEmissiveMap;
    int probeIndex;  // Per-object probe selection (0 = global, 1-7 = local)
    int lightmapIndex;  // Per-object lightmap index (-1 = no lightmap)
};

// 加载 Shader 源文件
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

// ============================================
// Constant Buffer Structures for Descriptor Set Path
// ============================================

// PerPass constant buffer (space1, b0)
struct alignas(16) CB_ForwardPerPass {
    XMMATRIX view;
    XMMATRIX proj;
    int cascadeCount;
    int debugShowCascades;
    int enableSoftShadows;
    float cascadeBlendRange;
    XMFLOAT4 cascadeSplits;
    XMMATRIX lightSpaceVPs[4];
    XMFLOAT3 lightDirWS; float _pad1;
    XMFLOAT3 lightColor; float _pad2;
    XMFLOAT3 camPosWS; float _pad3;
    float shadowBias;
    float iblIntensity;
    int diffuseGIMode;  // EDiffuseGIMode: 0=VL, 1=GlobalIBL, 2=None
    float _pad4;
};

// 收集并分类渲染项
void collectRenderItems(
    CScene& scene,
    XMVECTOR eye,
    std::vector<RenderItem>& opaqueItems,
    std::vector<RenderItem>& transparentItems,
    const CReflectionProbeManager* probeManager)
{
    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) continue;

        meshRenderer->EnsureUploaded();
        if (meshRenderer->meshes.empty()) continue;

        CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
        if (!meshRenderer->materialPath.empty()) {
            material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
        }

        CTextureManager& texMgr = CTextureManager::Instance();
        RHI::ITexture* albedoTex = material->albedoTexture.empty() ?
            texMgr.GetDefaultWhite().get() : texMgr.LoadAsync(material->albedoTexture, true)->GetTexture();
        RHI::ITexture* normalTex = material->normalMap.empty() ?
            texMgr.GetDefaultNormal().get() : texMgr.LoadAsync(material->normalMap, false)->GetTexture();
        RHI::ITexture* metallicRoughnessTex = material->metallicRoughnessMap.empty() ?
            texMgr.GetDefaultWhite().get() : texMgr.LoadAsync(material->metallicRoughnessMap, false)->GetTexture();
        RHI::ITexture* emissiveTex = material->emissiveMap.empty() ?
            texMgr.GetDefaultBlack().get() : texMgr.LoadAsync(material->emissiveMap, true)->GetTexture();

        bool hasRealMetallicRoughnessTexture = !material->metallicRoughnessMap.empty();
        bool hasRealEmissiveMap = !material->emissiveMap.empty();

        XMMATRIX worldMatrix = transform->WorldMatrix();
        XMVECTOR objPos = worldMatrix.r[3];
        XMVECTOR delta = XMVectorSubtract(objPos, eye);
        float distance = XMVectorGetX(XMVector3Length(delta));

        // Per-object probe selection
        int probeIndex = 0;
        if (probeManager) {
            XMFLOAT3 objPosF;
            XMStoreFloat3(&objPosF, objPos);
            probeIndex = probeManager->SelectProbeForPosition(objPosF);
        }

        for (auto& gpuMesh : meshRenderer->meshes) {
            if (!gpuMesh) continue;

            RenderItem item;
            item.obj = obj;
            item.meshRenderer = meshRenderer;
            item.transform = transform;
            item.material = material;
            item.worldMatrix = worldMatrix;
            item.distanceToCamera = distance;
            item.gpuMesh = gpuMesh.get();
            item.albedoTex = albedoTex;
            item.normalTex = normalTex;
            item.metallicRoughnessTex = metallicRoughnessTex;
            item.emissiveTex = emissiveTex;
            item.hasRealMetallicRoughnessTexture = hasRealMetallicRoughnessTexture;
            item.hasRealEmissiveMap = hasRealEmissiveMap;
            item.probeIndex = probeIndex;
            item.lightmapIndex = meshRenderer->lightmapInfosIndex;

            if (item.material->alphaMode == EAlphaMode::Blend) {
                transparentItems.push_back(item);
            } else {
                opaqueItems.push_back(item);
            }
        }
    }

    if (!transparentItems.empty()) {
        std::sort(transparentItems.begin(), transparentItems.end(),
            [](const RenderItem& a, const RenderItem& b) {
                return a.distanceToCamera > b.distanceToCamera;
            });
    }
}

} // anonymous namespace

// ============================================
// CSceneRenderer 实现
// ============================================

bool CSceneRenderer::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    createPipeline();

    // Initialize descriptor set resources (DX12 only)
    initDescriptorSets();

    return true;
}

void CSceneRenderer::Shutdown()
{
    m_cbFrame.reset();
    m_cbObj.reset();
    m_vs.reset();
    m_ps.reset();
    m_psoOpaque.reset();
    m_psoTransparent.reset();
    m_sampler.reset();

    // Cleanup descriptor set resources
    m_vs_ds.reset();
    m_ps_ds.reset();
    m_psoOpaque_ds.reset();
    m_psoTransparent_ds.reset();
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

void CSceneRenderer::Render(
    const CCamera& camera,
    CScene& scene,
    RHI::ITexture* hdrRT,
    RHI::ITexture* depthRT,
    uint32_t w, uint32_t h,
    float dt,
    const CShadowPass::Output* shadowData,
    CClusteredLightingPass* clusteredLighting,
    RHI::IDescriptorSet* perFrameSet,
    const CReflectionProbeManager* probeManager)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Require descriptor set path
    if (!m_perPassSet || !perFrameSet || !m_psoOpaque_ds) {
        CFFLog::Warning("[SceneRenderer] Descriptor set resources not available, skipping render");
        return;
    }

    CScopedDebugEvent evt(cmdList, L"Scene Renderer (DS)");

    // Set render targets
    cmdList->SetRenderTargets(1, &hdrRT, depthRT);
    cmdList->SetViewport(0, 0, (float)w, (float)h);
    cmdList->SetScissorRect(0, 0, w, h);

    // Clear render targets
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTarget(hdrRT, clearColor);
    float clearDepth = UseReversedZ() ? 0.0f : 1.0f;
    cmdList->ClearDepthStencil(depthRT, true, clearDepth, true, 0);

    // Find directional light
    SDirectionalLight* dirLight = nullptr;
    for (auto& objPtr : scene.GetWorld().Objects()) {
        dirLight = objPtr->GetComponent<SDirectionalLight>();
        if (dirLight) break;
    }

    // Build PerPass constant buffer
    CB_ForwardPerPass cbPerPass = {};
    cbPerPass.view = XMMatrixTranspose(camera.GetViewMatrix());
    cbPerPass.proj = XMMatrixTranspose(camera.GetProjectionMatrix());

    if (shadowData) {
        cbPerPass.cascadeCount = shadowData->cascadeCount;
        cbPerPass.debugShowCascades = shadowData->debugShowCascades ? 1 : 0;
        cbPerPass.enableSoftShadows = shadowData->enableSoftShadows ? 1 : 0;
        cbPerPass.cascadeBlendRange = shadowData->cascadeBlendRange;
        cbPerPass.cascadeSplits = XMFLOAT4(
            (0 < shadowData->cascadeCount) ? shadowData->cascadeSplits[0] : 100.0f,
            (1 < shadowData->cascadeCount) ? shadowData->cascadeSplits[1] : 100.0f,
            (2 < shadowData->cascadeCount) ? shadowData->cascadeSplits[2] : 100.0f,
            (3 < shadowData->cascadeCount) ? shadowData->cascadeSplits[3] : 100.0f
        );
        for (int i = 0; i < 4; ++i) {
            cbPerPass.lightSpaceVPs[i] = XMMatrixTranspose(shadowData->lightSpaceVPs[i]);
        }
    } else {
        cbPerPass.cascadeCount = 1;
        cbPerPass.debugShowCascades = 0;
        cbPerPass.enableSoftShadows = 1;
        cbPerPass.cascadeBlendRange = 0.0f;
        cbPerPass.cascadeSplits = XMFLOAT4(100.0f, 100.0f, 100.0f, 100.0f);
        for (int i = 0; i < 4; ++i) {
            cbPerPass.lightSpaceVPs[i] = XMMatrixTranspose(XMMatrixIdentity());
        }
    }

    if (dirLight) {
        cbPerPass.lightDirWS = dirLight->GetDirection();
        cbPerPass.lightColor = XMFLOAT3(
            dirLight->color.x * dirLight->intensity,
            dirLight->color.y * dirLight->intensity,
            dirLight->color.z * dirLight->intensity
        );
        cbPerPass.shadowBias = dirLight->shadow_bias;
        cbPerPass.iblIntensity = dirLight->ibl_intensity;
    } else {
        cbPerPass.lightDirWS = XMFLOAT3(0.4f, -1.0f, 0.2f);
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cbPerPass.lightDirWS));
        XMStoreFloat3(&cbPerPass.lightDirWS, L);
        cbPerPass.lightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        cbPerPass.shadowBias = 0.005f;
        cbPerPass.iblIntensity = 1.0f;
    }

    cbPerPass.camPosWS = camera.position;
    cbPerPass.diffuseGIMode = static_cast<int>(scene.GetLightSettings().diffuseGIMode);

    // Bind PerPass set
    m_perPassSet->Bind(BindingSetItem::VolatileCBV(PerPassSlots::CB::PerPass, &cbPerPass, sizeof(cbPerPass)));
    cmdList->BindDescriptorSet(1, m_perPassSet);

    // Bind PerFrame set (IBL, shadows, clustered lighting)
    cmdList->BindDescriptorSet(0, perFrameSet);

    // Collect render items
    std::vector<RenderItem> opaqueItems;
    std::vector<RenderItem> transparentItems;
    XMVECTOR eye = XMLoadFloat3(&camera.position);
    collectRenderItems(scene, eye, opaqueItems, transparentItems, probeManager);

    // ============================================
    // Render Opaque Objects
    // ============================================
    if (!opaqueItems.empty()) {
        cmdList->SetPipelineState(m_psoOpaque_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

        for (auto& item : opaqueItems) {
            // Bind PerMaterial set
            MaterialConstants::CB_Material matData;
            matData.albedo = item.material->albedo;
            matData.metallic = item.material->metallic;
            matData.emissive = item.material->emissive;
            matData.roughness = item.material->roughness;
            matData.emissiveStrength = item.material->emissiveStrength;
            matData.hasMetallicRoughnessTexture = item.hasRealMetallicRoughnessTexture ? 1 : 0;
            matData.hasEmissiveMap = item.hasRealEmissiveMap ? 1 : 0;
            matData.alphaMode = static_cast<int>(item.material->alphaMode);
            matData.alphaCutoff = item.material->alphaCutoff;
            matData.materialID = static_cast<float>(item.material->materialType);

            m_perMaterialSet->Bind({
                BindingSetItem::VolatileCBV(0, &matData, sizeof(matData)),
                BindingSetItem::Texture_SRV(0, item.albedoTex),
                BindingSetItem::Texture_SRV(1, item.normalTex),
                BindingSetItem::Texture_SRV(2, item.metallicRoughnessTex),
                BindingSetItem::Texture_SRV(3, item.emissiveTex)
            });
            cmdList->BindDescriptorSet(2, m_perMaterialSet);

            // Bind PerDraw set
            PerDrawSlots::CB_PerDraw perDraw;
            XMStoreFloat4x4(&perDraw.World, XMMatrixTranspose(item.worldMatrix));
            XMStoreFloat4x4(&perDraw.WorldPrev, XMMatrixTranspose(item.worldMatrix));  // TODO: Track previous frame
            perDraw.lightmapIndex = item.lightmapIndex;
            perDraw.objectID = item.probeIndex;  // Store probe index in objectID for now

            m_perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
            cmdList->BindDescriptorSet(3, m_perDrawSet);

            // Draw
            cmdList->SetVertexBuffer(0, item.gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
            cmdList->SetIndexBuffer(item.gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
            cmdList->DrawIndexed(item.gpuMesh->indexCount, 0, 0);
        }
    }

    // ============================================
    // Render Transparent Objects (back-to-front)
    // ============================================
    if (!transparentItems.empty()) {
        cmdList->SetPipelineState(m_psoTransparent_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

        for (auto& item : transparentItems) {
            // Bind PerMaterial set
            MaterialConstants::CB_Material matData;
            matData.albedo = item.material->albedo;
            matData.metallic = item.material->metallic;
            matData.emissive = item.material->emissive;
            matData.roughness = item.material->roughness;
            matData.emissiveStrength = item.material->emissiveStrength;
            matData.hasMetallicRoughnessTexture = item.hasRealMetallicRoughnessTexture ? 1 : 0;
            matData.hasEmissiveMap = item.hasRealEmissiveMap ? 1 : 0;
            matData.alphaMode = static_cast<int>(item.material->alphaMode);
            matData.alphaCutoff = item.material->alphaCutoff;
            matData.materialID = static_cast<float>(item.material->materialType);

            m_perMaterialSet->Bind({
                BindingSetItem::VolatileCBV(0, &matData, sizeof(matData)),
                BindingSetItem::Texture_SRV(0, item.albedoTex),
                BindingSetItem::Texture_SRV(1, item.normalTex),
                BindingSetItem::Texture_SRV(2, item.metallicRoughnessTex),
                BindingSetItem::Texture_SRV(3, item.emissiveTex)
            });
            cmdList->BindDescriptorSet(2, m_perMaterialSet);

            // Bind PerDraw set
            PerDrawSlots::CB_PerDraw perDraw;
            XMStoreFloat4x4(&perDraw.World, XMMatrixTranspose(item.worldMatrix));
            XMStoreFloat4x4(&perDraw.WorldPrev, XMMatrixTranspose(item.worldMatrix));
            perDraw.lightmapIndex = item.lightmapIndex;
            perDraw.objectID = item.probeIndex;

            m_perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
            cmdList->BindDescriptorSet(3, m_perDrawSet);

            // Draw
            cmdList->SetVertexBuffer(0, item.gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
            cmdList->SetIndexBuffer(item.gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
            cmdList->DrawIndexed(item.gpuMesh->indexCount, 0, 0);
        }
    }
}

void CSceneRenderer::createPipeline()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Load and compile shaders using RHI ShaderCompiler
    std::string vsSource = LoadShaderSource("../source/code/Shader/MainPass.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/MainPass.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load shader files!");
        return;
    }

    CDefaultShaderIncludeHandler includeHandler("../source/code/Shader/");

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource, "main", "vs_5_0", &includeHandler, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("VS Error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    SCompiledShader psCompiled = CompileShaderFromSource(psSource, "main", "ps_5_0", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("PS Error: %s", psCompiled.errorMessage.c_str());
        return;
    }

    // Create shaders via RHI
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

    // Input layout (matches SVertexPNT)
    std::vector<VertexElement> inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 },
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 },
        { EVertexSemantic::Texcoord, 1, EVertexFormat::Float2, 64, 0 }  // UV2 for lightmap
    };

    // ============================================
    // Opaque Pipeline State
    // ============================================
    PipelineStateDesc psoOpaque;
    psoOpaque.vertexShader = m_vs.get();
    psoOpaque.pixelShader = m_ps.get();
    psoOpaque.inputLayout = inputLayout;

    // Rasterizer state
    psoOpaque.rasterizer.fillMode = EFillMode::Solid;
    psoOpaque.rasterizer.cullMode = ECullMode::Back;
    psoOpaque.rasterizer.frontCounterClockwise = false;
    psoOpaque.rasterizer.depthClipEnable = true;

    // Depth stencil state (depth test + write)
    psoOpaque.depthStencil.depthEnable = true;
    psoOpaque.depthStencil.depthWriteEnable = true;
    psoOpaque.depthStencil.depthFunc = GetDepthComparisonFunc(false);  // Less or Greater

    // No blending
    psoOpaque.blend.blendEnable = false;

    psoOpaque.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoOpaque.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoOpaque.depthStencilFormat = ETextureFormat::D24_UNORM_S8_UINT;
    psoOpaque.debugName = "Forward_Opaque_PSO";

    m_psoOpaque.reset(ctx->CreatePipelineState(psoOpaque));

    // ============================================
    // Transparent Pipeline State
    // ============================================
    PipelineStateDesc psoTransparent = psoOpaque;  // Copy opaque settings

    // Depth: read-only (no write)
    psoTransparent.depthStencil.depthWriteEnable = false;
    psoTransparent.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual

    // Alpha blending
    psoTransparent.blend.blendEnable = true;
    psoTransparent.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoTransparent.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoTransparent.blend.blendOp = EBlendOp::Add;
    psoTransparent.blend.srcBlendAlpha = EBlendFactor::One;
    psoTransparent.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoTransparent.blend.blendOpAlpha = EBlendOp::Add;
    psoTransparent.debugName = "Forward_Transparent_PSO";

    m_psoTransparent.reset(ctx->CreatePipelineState(psoTransparent));

    // ============================================
    // Constant Buffers (legacy - kept for compatibility)
    // ============================================
    BufferDesc cbFrameDesc;
    cbFrameDesc.size = sizeof(CB_ForwardPerPass);
    cbFrameDesc.usage = EBufferUsage::Constant;
    cbFrameDesc.cpuAccess = ECPUAccess::Write;
    m_cbFrame.reset(ctx->CreateBuffer(cbFrameDesc, nullptr));

    BufferDesc cbObjDesc;
    cbObjDesc.size = sizeof(PerDrawSlots::CB_PerDraw);
    cbObjDesc.usage = EBufferUsage::Constant;
    cbObjDesc.cpuAccess = ECPUAccess::Write;
    m_cbObj.reset(ctx->CreateBuffer(cbObjDesc, nullptr));

    // ============================================
    // Sampler
    // ============================================
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::Anisotropic;
    samplerDesc.maxAnisotropy = 8;
    samplerDesc.addressU = ETextureAddressMode::Wrap;
    samplerDesc.addressV = ETextureAddressMode::Wrap;
    samplerDesc.addressW = ETextureAddressMode::Wrap;
    samplerDesc.minLOD = 0.0f;
    samplerDesc.maxLOD = 3.402823466e+38f;
    m_sampler.reset(ctx->CreateSampler(samplerDesc));
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CSceneRenderer::initDescriptorSets()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[SceneRenderer] DX11 mode - descriptor sets not supported");
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
    std::string vsSource = LoadShaderSource(shaderDir + "MainPass_DS.vs.hlsl");
    if (vsSource.empty()) {
        CFFLog::Warning("[SceneRenderer] Failed to load MainPass_DS.vs.hlsl, using legacy shader");
        // Fall back to legacy shader for now
        vsSource = LoadShaderSource(shaderDir + "MainPass.vs.hlsl");
        if (vsSource.empty()) {
            CFFLog::Error("[SceneRenderer] Failed to load any vertex shader");
            return;
        }
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", &includeHandler, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("[SceneRenderer] MainPass_DS.vs.hlsl compile error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "Forward_DS_VS";
    m_vs_ds.reset(ctx->CreateShader(vsDesc));

    // Compile SM 5.1 pixel shader
    std::string psSource = LoadShaderSource(shaderDir + "MainPass_DS.ps.hlsl");
    if (psSource.empty()) {
        CFFLog::Warning("[SceneRenderer] Failed to load MainPass_DS.ps.hlsl, using legacy shader");
        psSource = LoadShaderSource(shaderDir + "MainPass.ps.hlsl");
        if (psSource.empty()) {
            CFFLog::Error("[SceneRenderer] Failed to load any pixel shader");
            return;
        }
    }

    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_1", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("[SceneRenderer] MainPass_DS.ps.hlsl compile error: %s", psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "Forward_DS_PS";
    m_ps_ds.reset(ctx->CreateShader(psDesc));

    if (!m_vs_ds || !m_ps_ds) {
        CFFLog::Error("[SceneRenderer] Failed to create SM 5.1 shaders");
        return;
    }

    // Create material sampler
    {
        SamplerDesc desc;
        desc.filter = EFilter::MinMagMipLinear;
        desc.addressU = ETextureAddressMode::Wrap;
        desc.addressV = ETextureAddressMode::Wrap;
        desc.addressW = ETextureAddressMode::Wrap;
        m_materialSampler.reset(ctx->CreateSampler(desc));
    }

    // Create PerPass layout (Set 1, space1)
    // CB_ForwardPerPass (b0)
    BindingLayoutDesc perPassLayoutDesc("Forward_PerPass");
    perPassLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(PerPassSlots::CB::PerPass, sizeof(CB_ForwardPerPass)));

    m_perPassLayout = ctx->CreateDescriptorSetLayout(perPassLayoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[SceneRenderer] Failed to create PerPass layout");
        return;
    }

    // Create PerMaterial layout (Set 2, space2)
    // CB_Material (b0), Albedo (t0), Normal (t1), MetallicRoughness (t2), Emissive (t3), Sampler (s0)
    BindingLayoutDesc perMaterialLayoutDesc("Forward_PerMaterial");
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(MaterialConstants::CB_Material)));
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));   // Albedo
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(1));   // Normal
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(2));   // MetallicRoughness
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Texture_SRV(3));   // Emissive
    perMaterialLayoutDesc.AddItem(BindingLayoutItem::Sampler(0));       // Material sampler

    m_perMaterialLayout = ctx->CreateDescriptorSetLayout(perMaterialLayoutDesc);
    if (!m_perMaterialLayout) {
        CFFLog::Error("[SceneRenderer] Failed to create PerMaterial layout");
        return;
    }

    // Create PerDraw layout (Set 3, space3)
    // CB_PerDraw (b0)
    BindingLayoutDesc perDrawLayoutDesc("Forward_PerDraw");
    perDrawLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(PerDrawSlots::CB_PerDraw)));

    m_perDrawLayout = ctx->CreateDescriptorSetLayout(perDrawLayoutDesc);
    if (!m_perDrawLayout) {
        CFFLog::Error("[SceneRenderer] Failed to create PerDraw layout");
        return;
    }

    // Allocate descriptor sets
    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    m_perMaterialSet = ctx->AllocateDescriptorSet(m_perMaterialLayout);
    m_perDrawSet = ctx->AllocateDescriptorSet(m_perDrawLayout);

    if (!m_perPassSet || !m_perMaterialSet || !m_perDrawSet) {
        CFFLog::Error("[SceneRenderer] Failed to allocate descriptor sets");
        return;
    }

    // Bind static sampler to PerMaterial set
    m_perMaterialSet->Bind(BindingSetItem::Sampler(0, m_materialSampler.get()));

    CFFLog::Info("[SceneRenderer] Descriptor set resources initialized");
}

void CSceneRenderer::CreatePSOWithLayouts(IDescriptorSetLayout* perFrameLayout)
{
    if (!m_perPassLayout || !perFrameLayout || !m_vs_ds || !m_ps_ds) {
        CFFLog::Warning("[SceneRenderer] Cannot create PSO with layouts - missing resources");
        return;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Input layout (matches SVertexPNT)
    std::vector<VertexElement> inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 },
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 },
        { EVertexSemantic::Texcoord, 1, EVertexFormat::Float2, 64, 0 }  // UV2 for lightmap
    };

    // ============================================
    // Opaque Pipeline State (Descriptor Set Path)
    // ============================================
    PipelineStateDesc psoOpaque;
    psoOpaque.vertexShader = m_vs_ds.get();
    psoOpaque.pixelShader = m_ps_ds.get();
    psoOpaque.inputLayout = inputLayout;

    // Rasterizer state
    psoOpaque.rasterizer.fillMode = EFillMode::Solid;
    psoOpaque.rasterizer.cullMode = ECullMode::Back;
    psoOpaque.rasterizer.frontCounterClockwise = false;
    psoOpaque.rasterizer.depthClipEnable = true;

    // Depth stencil state (depth test + write)
    psoOpaque.depthStencil.depthEnable = true;
    psoOpaque.depthStencil.depthWriteEnable = true;
    psoOpaque.depthStencil.depthFunc = GetDepthComparisonFunc(false);  // Less or Greater

    // No blending
    psoOpaque.blend.blendEnable = false;

    psoOpaque.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoOpaque.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoOpaque.depthStencilFormat = ETextureFormat::D24_UNORM_S8_UINT;

    // Set descriptor set layouts
    psoOpaque.setLayouts[0] = perFrameLayout;      // Set 0: PerFrame (space0)
    psoOpaque.setLayouts[1] = m_perPassLayout;     // Set 1: PerPass (space1)
    psoOpaque.setLayouts[2] = m_perMaterialLayout; // Set 2: PerMaterial (space2)
    psoOpaque.setLayouts[3] = m_perDrawLayout;     // Set 3: PerDraw (space3)

    psoOpaque.debugName = "Forward_Opaque_DS_PSO";
    m_psoOpaque_ds.reset(ctx->CreatePipelineState(psoOpaque));

    // ============================================
    // Transparent Pipeline State (Descriptor Set Path)
    // ============================================
    PipelineStateDesc psoTransparent = psoOpaque;  // Copy opaque settings

    // Depth: read-only (no write)
    psoTransparent.depthStencil.depthWriteEnable = false;
    psoTransparent.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual

    // Alpha blending
    psoTransparent.blend.blendEnable = true;
    psoTransparent.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoTransparent.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoTransparent.blend.blendOp = EBlendOp::Add;
    psoTransparent.blend.srcBlendAlpha = EBlendFactor::One;
    psoTransparent.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoTransparent.blend.blendOpAlpha = EBlendOp::Add;
    psoTransparent.debugName = "Forward_Transparent_DS_PSO";

    m_psoTransparent_ds.reset(ctx->CreatePipelineState(psoTransparent));

    if (m_psoOpaque_ds && m_psoTransparent_ds) {
        CFFLog::Info("[SceneRenderer] PSOs with descriptor set layouts created");
    } else {
        CFFLog::Error("[SceneRenderer] Failed to create PSOs with descriptor set layouts");
    }
}
