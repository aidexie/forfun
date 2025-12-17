#include "SceneRenderer.h"
#include "ShadowPass.h"
#include "ShowFlags.h"
#include "ReflectionProbeManager.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
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

// Constant Buffer 结构
struct alignas(16) CB_Frame {
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

struct alignas(16) CB_Object {
    XMMATRIX world;
    XMFLOAT3 albedo; float metallic;
    XMFLOAT3 emissive; float roughness;
    float emissiveStrength;
    int hasMetallicRoughnessTexture;
    int hasEmissiveMap;
    int alphaMode;
    float alphaCutoff;
    int probeIndex;  // Per-object probe selection (0 = global, 1-7 = local)
    XMFLOAT2 _padObj;
};

struct alignas(16) CB_ClusteredParams {
    float nearZ;
    float farZ;
    uint32_t numClustersX;
    uint32_t numClustersY;
    uint32_t numClustersZ;
    uint32_t _pad[3];
};

// 更新帧常量 via RHI Map/Unmap
CB_Frame updateFrameConstants(
    IBuffer* cbFrame,
    const XMMATRIX& view,
    const XMMATRIX& proj,
    const XMFLOAT3& camPos,
    SDirectionalLight* dirLight,
    const CShadowPass::Output* shadowData,
    int diffuseGIMode)
{
    CB_Frame cf{};
    cf.view = XMMatrixTranspose(view);
    cf.proj = XMMatrixTranspose(proj);

    if (shadowData) {
        cf.cascadeCount = shadowData->cascadeCount;
        cf.debugShowCascades = shadowData->debugShowCascades ? 1 : 0;
        cf.enableSoftShadows = shadowData->enableSoftShadows ? 1 : 0;
        cf.cascadeBlendRange = shadowData->cascadeBlendRange;
        cf.cascadeSplits = XMFLOAT4(
            (0 < shadowData->cascadeCount) ? shadowData->cascadeSplits[0] : 100.0f,
            (1 < shadowData->cascadeCount) ? shadowData->cascadeSplits[1] : 100.0f,
            (2 < shadowData->cascadeCount) ? shadowData->cascadeSplits[2] : 100.0f,
            (3 < shadowData->cascadeCount) ? shadowData->cascadeSplits[3] : 100.0f
        );
        for (int i = 0; i < 4; ++i) {
            cf.lightSpaceVPs[i] = XMMatrixTranspose(shadowData->lightSpaceVPs[i]);
        }
    } else {
        cf.cascadeCount = 1;
        cf.debugShowCascades = 0;
        cf.enableSoftShadows = 1;
        cf.cascadeBlendRange = 0.0f;
        cf.cascadeSplits = XMFLOAT4(100.0f, 100.0f, 100.0f, 100.0f);
        for (int i = 0; i < 4; ++i) {
            cf.lightSpaceVPs[i] = XMMatrixTranspose(XMMatrixIdentity());
        }
    }

    if (dirLight) {
        cf.lightDirWS = dirLight->GetDirection();
        cf.lightColor = XMFLOAT3(
            dirLight->color.x * dirLight->intensity,
            dirLight->color.y * dirLight->intensity,
            dirLight->color.z * dirLight->intensity
        );
        cf.shadowBias = dirLight->shadow_bias;
        cf.iblIntensity = dirLight->ibl_intensity;
    } else {
        cf.lightDirWS = XMFLOAT3(0.4f, -1.0f, 0.2f);
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cf.lightDirWS));
        XMStoreFloat3(&cf.lightDirWS, L);
        cf.lightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        cf.shadowBias = 0.005f;
        cf.iblIntensity = 1.0f;
    }

    cf.camPosWS = camPos;
    cf.diffuseGIMode = diffuseGIMode;
    return cf;
}

// 更新物体常量 via SetConstantBufferData (unified API for both DX11 and DX12)
void updateObjectConstants(
    ICommandList* cmdList,
    const RenderItem& item)
{
    CB_Object co{};
    co.world = XMMatrixTranspose(item.worldMatrix);
    co.albedo = item.material->albedo;
    co.metallic = item.material->metallic;
    co.roughness = item.material->roughness;
    co.hasMetallicRoughnessTexture = item.hasRealMetallicRoughnessTexture ? 1 : 0;
    co.emissive = item.material->emissive;
    co.emissiveStrength = item.material->emissiveStrength;
    co.hasEmissiveMap = item.hasRealEmissiveMap ? 1 : 0;
    co.alphaMode = static_cast<int>(item.material->alphaMode);
    co.alphaCutoff = item.material->alphaCutoff;
    co.probeIndex = item.probeIndex;

    // Use unified SetConstantBufferData API - works on both DX11 and DX12
    cmdList->SetConstantBufferData(EShaderStage::Vertex, 1, &co, sizeof(CB_Object));
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 1, &co, sizeof(CB_Object));
}

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
            texMgr.GetDefaultWhite() : texMgr.Load(material->albedoTexture, true);
        RHI::ITexture* normalTex = material->normalMap.empty() ?
            texMgr.GetDefaultNormal() : texMgr.Load(material->normalMap, false);
        RHI::ITexture* metallicRoughnessTex = material->metallicRoughnessMap.empty() ?
            texMgr.GetDefaultWhite() : texMgr.Load(material->metallicRoughnessMap, false);
        RHI::ITexture* emissiveTex = material->emissiveMap.empty() ?
            texMgr.GetDefaultBlack() : texMgr.Load(material->emissiveMap, true);

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

// 渲染物体列表 (统一处理 opaque 和 transparent)
void renderItems(
    ICommandList* cmdList,
    const std::vector<RenderItem>& items)
{
    for (auto& item : items) {
        updateObjectConstants(cmdList, item);

        // Bind vertex/index buffers
        cmdList->SetVertexBuffer(0, item.gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
        cmdList->SetIndexBuffer(item.gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);

        // Bind textures (t0=albedo, t1=normal, t2=metallicRoughness, t3=emissive)
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, item.albedoTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, item.normalTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 2, item.metallicRoughnessTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 3, item.emissiveTex);

        // Draw
        cmdList->DrawIndexed(item.gpuMesh->indexCount, 0, 0);
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

    m_clusteredLighting.Initialize();

    return true;
}

void CSceneRenderer::Shutdown()
{
    m_cbFrame.reset();
    m_cbObj.reset();
    m_cbClusteredParams.reset();
    m_vs.reset();
    m_ps.reset();
    m_psoOpaque.reset();
    m_psoTransparent.reset();
    m_sampler.reset();
}

void CSceneRenderer::Render(
    const CCamera& camera,
    CScene& scene,
    RHI::ITexture* hdrRT,
    RHI::ITexture* depthRT,
    uint32_t w, uint32_t h,
    float dt,
    const CShadowPass::Output* shadowData)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Set render targets via RHI
    ITexture* rts[] = { hdrRT };
    cmdList->SetRenderTargets(1, rts, depthRT);

    // Setup viewport and scissor rect (DX12 requires both)
    cmdList->SetViewport(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, w, h);

    // Camera matrices
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = camera.GetProjectionMatrix();
    XMVECTOR eye = XMLoadFloat3(&camera.position);

    // DX12 minimal mode: enable features incrementally
    // TODO: Remove this flag once all DX12 features are working
    const bool dx12Mode = (CRHIManager::Instance().GetBackend() == EBackend::DX12);

    // Find DirectionalLight
    SDirectionalLight* dirLight = nullptr;
    for (auto& objPtr : scene.GetWorld().Objects()) {
        dirLight = objPtr->GetComponent<SDirectionalLight>();
        if (dirLight) break;
    }

    // Update frame constants
    int diffuseGIMode = static_cast<int>(scene.GetLightSettings().diffuseGIMode);
    auto cf = updateFrameConstants(m_cbFrame.get(), view, proj, camera.position, dirLight, shadowData, diffuseGIMode);
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cf, sizeof(cf));

    // Bind shadow resources (t4=shadowMap, s1=shadowSampler)
    if (shadowData && shadowData->shadowMapArray) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 4, shadowData->shadowMapArray);
    }
    if (shadowData && shadowData->shadowSampler) {
        cmdList->SetSampler(EShaderStage::Pixel, 1, shadowData->shadowSampler);
    }

    // Bind BRDF LUT (t5) - modules use RHI::ICommandList*
    auto& probeManager = scene.GetProbeManager();
    probeManager.Bind(cmdList);
    // Skip advanced features in DX12 mode for now
    if (!dx12Mode) {

        // Bind Light Probes (t15, b5)
        auto& lightProbeManager = scene.GetLightProbeManager();
        lightProbeManager.Bind(cmdList);

        // Bind Volumetric Lightmap (t20-t24, b6)
        auto& volumetricLightmap = scene.GetVolumetricLightmap();
        volumetricLightmap.Bind(cmdList);
    }

    // Bind sampler (s0)
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

    // Skip clustered lighting in DX12 mode for now
    if (!dx12Mode) {
        // Clustered lighting setup
        {
            RHI::CScopedDebugEvent evt(cmdList, L"clustered Lighting");
            m_clusteredLighting.Resize(w, h);
            m_clusteredLighting.BuildClusterGrid(cmdList, proj, camera.nearZ, camera.farZ);
            m_clusteredLighting.CullLights(cmdList, &scene, view);
        }
        // Update clustered params constant buffer (b3)
        CB_ClusteredParams clusteredParams{};
        clusteredParams.nearZ = camera.nearZ;
        clusteredParams.farZ = camera.farZ;
        clusteredParams.numClustersX = m_clusteredLighting.GetNumClustersX();
        clusteredParams.numClustersY = m_clusteredLighting.GetNumClustersY();
        clusteredParams.numClustersZ = m_clusteredLighting.GetNumClustersZ();

        void* mappedCluster = m_cbClusteredParams->Map();
        if (mappedCluster) {
            memcpy(mappedCluster, &clusteredParams, sizeof(CB_ClusteredParams));
            m_cbClusteredParams->Unmap();
        }
        cmdList->SetConstantBuffer(EShaderStage::Pixel, 3, m_cbClusteredParams.get());
        m_clusteredLighting.BindToMainPass(cmdList);
    }

    // Collect render items
    std::vector<RenderItem> opaqueItems;
    std::vector<RenderItem> transparentItems;
    collectRenderItems(scene, eye, opaqueItems, transparentItems, &probeManager);

    // Render Opaque
    {
        RHI::CScopedDebugEvent evt(cmdList, L"Opaque Pass");
        cmdList->SetPipelineState(m_psoOpaque.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);
        renderItems(cmdList, opaqueItems);
    }

    // Render Skybox
    {
        RHI::CScopedDebugEvent evt(cmdList, L"Skybox");
        CScene::Instance().GetSkybox().Render(view, proj);
    }

    // Render Transparent (after Skybox, need to restore state)
    // Skybox overwrites: CB b0 (vertex), SRV t0, Sampler s0
    {
        RHI::CScopedDebugEvent evt(cmdList, L"Transparent Pass");

        // Restore frame constants (b0) - Skybox overwrote it with its own CB
        cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cf, sizeof(cf));

        // Restore sampler (s0) - Skybox overwrote it
        cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

        // Set transparent PSO and topology
        cmdList->SetPipelineState(m_psoTransparent.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

        // renderItems will set per-object CB b1, SRV t0-t3
        renderItems(cmdList, transparentItems);
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
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 }
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
    psoOpaque.depthStencil.depthFunc = EComparisonFunc::Less;

    // No blending
    psoOpaque.blend.blendEnable = false;

    psoOpaque.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoOpaque.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoOpaque.depthStencilFormat = ETextureFormat::D24_UNORM_S8_UINT;

    m_psoOpaque.reset(ctx->CreatePipelineState(psoOpaque));

    // ============================================
    // Transparent Pipeline State
    // ============================================
    PipelineStateDesc psoTransparent = psoOpaque;  // Copy opaque settings

    // Depth: read-only (no write) with LessEqual
    psoTransparent.depthStencil.depthWriteEnable = false;
    psoTransparent.depthStencil.depthFunc = EComparisonFunc::LessEqual;

    // Alpha blending
    psoTransparent.blend.blendEnable = true;
    psoTransparent.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoTransparent.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoTransparent.blend.blendOp = EBlendOp::Add;
    psoTransparent.blend.srcBlendAlpha = EBlendFactor::One;
    psoTransparent.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoTransparent.blend.blendOpAlpha = EBlendOp::Add;

    m_psoTransparent.reset(ctx->CreatePipelineState(psoTransparent));

    // ============================================
    // Constant Buffers (CPU-writable for Map/Unmap)
    // ============================================
    BufferDesc cbFrameDesc;
    cbFrameDesc.size = sizeof(CB_Frame);
    cbFrameDesc.usage = EBufferUsage::Constant;
    cbFrameDesc.cpuAccess = ECPUAccess::Write;
    m_cbFrame.reset(ctx->CreateBuffer(cbFrameDesc, nullptr));

    BufferDesc cbObjDesc;
    cbObjDesc.size = sizeof(CB_Object);
    cbObjDesc.usage = EBufferUsage::Constant;
    cbObjDesc.cpuAccess = ECPUAccess::Write;
    m_cbObj.reset(ctx->CreateBuffer(cbObjDesc, nullptr));

    BufferDesc cbClusteredDesc;
    cbClusteredDesc.size = sizeof(CB_ClusteredParams);
    cbClusteredDesc.usage = EBufferUsage::Constant;
    cbClusteredDesc.cpuAccess = ECPUAccess::Write;
    m_cbClusteredParams.reset(ctx->CreateBuffer(cbClusteredDesc, nullptr));

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
    // DX12 doesn't support GenerateMips yet, so disable mipmap sampling
    bool dx12Mode = (ctx->GetBackend() == EBackend::DX12);
    samplerDesc.maxLOD = dx12Mode ? 0.0f : 3.402823466e+38f;
    m_sampler.reset(ctx->CreateSampler(samplerDesc));
}
