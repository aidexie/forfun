#include "SceneRenderer.h"
#include "ShadowPass.h"
#include "ShowFlags.h"
#include "ReflectionProbeManager.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIResources.h"
#include "Core/FFLog.h"
#include "Core/DebugEvent.h"
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
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

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
    ID3D11ShaderResourceView* albedoSRV;
    ID3D11ShaderResourceView* normalSRV;
    ID3D11ShaderResourceView* metallicRoughnessSRV;
    ID3D11ShaderResourceView* emissiveSRV;
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

// Custom include handler for D3DCompile
class CShaderIncludeHandler : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName,
                          LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override {
        // Build full path relative to Shader directory
        std::string fullPath = std::string("../source/code/Shader/") + pFileName;

        std::ifstream file(fullPath, std::ios::binary);
        if (!file.is_open()) {
            CFFLog::Error("Failed to open include file: %s (tried path: %s)", pFileName, fullPath.c_str());
            return E_FAIL;
        }

        // Read file content
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);

        char* buffer = new char[size];
        file.read(buffer, size);

        *ppData = buffer;
        *pBytes = static_cast<UINT>(size);

        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID pData) override {
        delete[] static_cast<const char*>(pData);
        return S_OK;
    }
};

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

// 更新帧常量
void updateFrameConstants(
    ID3D11DeviceContext* context,
    ID3D11Buffer* cbFrame,
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
    context->UpdateSubresource(cbFrame, 0, nullptr, &cf, 0, 0);
}

// 绑定全局资源（Shadow, BRDF LUT）
// Note: IBL binding moved to ReflectionProbeManager::Bind() (t3, t4, b4)
void bindGlobalResources(
    ID3D11DeviceContext* context,
    const CShadowPass::Output* shadowData)
{
    if (shadowData && shadowData->shadowSampler) {
        context->PSSetSamplers(1, 1, &shadowData->shadowSampler);
    }
    if (shadowData && shadowData->shadowMapArray) {
        context->PSSetShaderResources(2, 1, &shadowData->shadowMapArray);
    }

    // t5: BRDF LUT (now from ReflectionProbeManager)
    ID3D11ShaderResourceView* brdfLutSRV = CScene::Instance().GetProbeManager().GetBrdfLutSRV();
    context->PSSetShaderResources(5, 1, &brdfLutSRV);
}

// 更新物体常量
void updateObjectConstants(
    ID3D11DeviceContext* context,
    ID3D11Buffer* cbObj,
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
    co.probeIndex = item.probeIndex;  // Per-object probe selection
    context->UpdateSubresource(cbObj, 0, nullptr, &co, 0, 0);
}

// 收集并分类渲染项
// Per-object probe selection via CReflectionProbeManager::SelectProbeForPosition()
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
        ID3D11ShaderResourceView* albedoSRV = material->albedoTexture.empty() ?
            texMgr.GetDefaultWhite() : texMgr.Load(material->albedoTexture, true);
        ID3D11ShaderResourceView* normalSRV = material->normalMap.empty() ?
            texMgr.GetDefaultNormal() : texMgr.Load(material->normalMap, false);
        ID3D11ShaderResourceView* metallicRoughnessSRV = material->metallicRoughnessMap.empty() ?
            texMgr.GetDefaultWhite() : texMgr.Load(material->metallicRoughnessMap, false);
        ID3D11ShaderResourceView* emissiveSRV = material->emissiveMap.empty() ?
            texMgr.GetDefaultBlack() : texMgr.Load(material->emissiveMap, true);

        bool hasRealMetallicRoughnessTexture = !material->metallicRoughnessMap.empty();
        bool hasRealEmissiveMap = !material->emissiveMap.empty();

        XMMATRIX worldMatrix = transform->WorldMatrix();
        XMVECTOR objPos = worldMatrix.r[3];
        XMVECTOR delta = XMVectorSubtract(objPos, eye);
        float distance = XMVectorGetX(XMVector3Length(delta));

        // Per-object probe selection (using object center position)
        int probeIndex = 0;  // Default: global IBL
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
            item.albedoSRV = albedoSRV;
            item.normalSRV = normalSRV;
            item.metallicRoughnessSRV = metallicRoughnessSRV;
            item.emissiveSRV = emissiveSRV;
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

// 渲染不透明Pass
// Note: Reflection Probe binding is now via ReflectionProbeManager::Bind() (once per frame)
void renderOpaquePass(
    ID3D11DeviceContext* context,
    const std::vector<RenderItem>& opaqueItems,
    ID3D11InputLayout* inputLayout,
    ID3D11VertexShader* vs,
    ID3D11PixelShader* ps,
    ID3D11RasterizerState* rasterState,
    ID3D11DepthStencilState* depthState,
    ID3D11Buffer* cbFrame,
    ID3D11Buffer* cbObj,
    ID3D11SamplerState* sampler)
{
    if (opaqueItems.empty()) return;

    context->IASetInputLayout(inputLayout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->RSSetState(rasterState);
    context->OMSetDepthStencilState(depthState, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    context->VSSetConstantBuffers(0, 1, &cbFrame);
    context->VSSetConstantBuffers(1, 1, &cbObj);
    context->PSSetConstantBuffers(0, 1, &cbFrame);
    context->PSSetConstantBuffers(1, 1, &cbObj);
    context->PSSetSamplers(0, 1, &sampler);

    for (auto& item : opaqueItems) {
        updateObjectConstants(context, cbObj, item);

        UINT stride = sizeof(SVertexPNT), offset = 0;
        ID3D11Buffer* vbo = static_cast<ID3D11Buffer*>(item.gpuMesh->vbo->GetNativeHandle());
        context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
        context->IASetIndexBuffer(static_cast<ID3D11Buffer*>(item.gpuMesh->ibo->GetNativeHandle()), DXGI_FORMAT_R32_UINT, 0);

        ID3D11ShaderResourceView* srvs[2] = { item.albedoSRV, item.normalSRV };
        context->PSSetShaderResources(0, 2, srvs);
        context->PSSetShaderResources(6, 1, &item.metallicRoughnessSRV);
        context->PSSetShaderResources(7, 1, &item.emissiveSRV);

        context->DrawIndexed(item.gpuMesh->indexCount, 0, 0);
    }
}

// 渲染透明Pass
// Note: Reflection Probe binding is now via ReflectionProbeManager::Bind() (once per frame)
void renderTransparentPass(
    ID3D11DeviceContext* context,
    const std::vector<RenderItem>& transparentItems,
    ID3D11InputLayout* inputLayout,
    ID3D11VertexShader* vs,
    ID3D11PixelShader* ps,
    ID3D11RasterizerState* rasterState,
    ID3D11DepthStencilState* depthStateTransparent,
    ID3D11BlendState* blendStateTransparent,
    ID3D11Buffer* cbFrame,
    ID3D11Buffer* cbObj,
    ID3D11SamplerState* sampler)
{
    if (transparentItems.empty()) return;

    context->IASetInputLayout(inputLayout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->RSSetState(rasterState);
    context->OMSetDepthStencilState(depthStateTransparent, 0);

    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    context->OMSetBlendState(blendStateTransparent, blendFactor, 0xFFFFFFFF);

    context->VSSetConstantBuffers(0, 1, &cbFrame);
    context->VSSetConstantBuffers(1, 1, &cbObj);
    context->PSSetConstantBuffers(0, 1, &cbFrame);
    context->PSSetConstantBuffers(1, 1, &cbObj);
    context->PSSetSamplers(0, 1, &sampler);

    for (auto& item : transparentItems) {
        updateObjectConstants(context, cbObj, item);

        UINT stride = sizeof(SVertexPNT), offset = 0;
        ID3D11Buffer* vbo = static_cast<ID3D11Buffer*>(item.gpuMesh->vbo->GetNativeHandle());
        context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
        context->IASetIndexBuffer(static_cast<ID3D11Buffer*>(item.gpuMesh->ibo->GetNativeHandle()), DXGI_FORMAT_R32_UINT, 0);

        ID3D11ShaderResourceView* srvs[2] = { item.albedoSRV, item.normalSRV };
        context->PSSetShaderResources(0, 2, srvs);
        context->PSSetShaderResources(6, 1, &item.metallicRoughnessSRV);
        context->PSSetShaderResources(7, 1, &item.emissiveSRV);

        context->DrawIndexed(item.gpuMesh->indexCount, 0, 0);
    }
}

} // anonymous namespace

// ============================================
// CSceneRenderer 实现
// ============================================

bool CSceneRenderer::Initialize()
{
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return false;

    createPipeline();
    createRasterStates();

    m_clusteredLighting.Initialize(device);

    return true;
}

void CSceneRenderer::Shutdown()
{
    m_cbFrame.Reset();
    m_cbObj.Reset();
    m_cbClusteredParams.Reset();
    m_inputLayout.Reset();
    m_vs.Reset();
    m_ps.Reset();
    m_sampler.Reset();
    m_rsSolid.Reset();
    m_rsWire.Reset();
    m_depthStateDefault.Reset();
    m_depthStateTransparent.Reset();
    m_blendStateTransparent.Reset();
}

void CSceneRenderer::Render(
    const CCamera& camera,
    CScene& scene,
    RHI::ITexture* hdrRT,
    RHI::ITexture* depthRT,
    UINT w, UINT h,
    float dt,
    const CShadowPass::Output* shadowData)
{
    ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeContext());
    if (!context) return;

    // Get D3D11 views from RHI textures
    ID3D11RenderTargetView* hdrRTV = hdrRT ? static_cast<ID3D11RenderTargetView*>(hdrRT->GetRTV()) : nullptr;
    ID3D11DepthStencilView* dsv = depthRT ? static_cast<ID3D11DepthStencilView*>(depthRT->GetDSV()) : nullptr;

    // Bind render targets
    context->OMSetRenderTargets(1, &hdrRTV, dsv);

    // Setup viewport
    D3D11_VIEWPORT vp{};
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // Camera matrices
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = camera.GetProjectionMatrix();
    XMVECTOR eye = XMLoadFloat3(&camera.position);

    // Find DirectionalLight
    SDirectionalLight* dirLight = nullptr;
    for (auto& objPtr : scene.GetWorld().Objects()) {
        dirLight = objPtr->GetComponent<SDirectionalLight>();
        if (dirLight) break;
    }

    // Update frame constants (includes diffuseGIMode from scene settings)
    int diffuseGIMode = static_cast<int>(scene.GetLightSettings().diffuseGIMode);
    updateFrameConstants(context, m_cbFrame.Get(), view, proj, camera.position, dirLight, shadowData, diffuseGIMode);
    bindGlobalResources(context, shadowData);

    // Clustered lighting setup
    m_clusteredLighting.Resize(w, h);
    m_clusteredLighting.BuildClusterGrid(context, proj, camera.nearZ, camera.farZ);
    m_clusteredLighting.CullLights(context, &scene, view);

    CB_ClusteredParams clusteredParams{};
    clusteredParams.nearZ = camera.nearZ;
    clusteredParams.farZ = camera.farZ;
    clusteredParams.numClustersX = m_clusteredLighting.GetNumClustersX();
    clusteredParams.numClustersY = m_clusteredLighting.GetNumClustersY();
    clusteredParams.numClustersZ = m_clusteredLighting.GetNumClustersZ();
    context->UpdateSubresource(m_cbClusteredParams.Get(), 0, nullptr, &clusteredParams, 0, 0);
    context->PSSetConstantBuffers(3, 1, m_cbClusteredParams.GetAddressOf());
    m_clusteredLighting.BindToMainPass(context);

    // Bind Reflection Probe TextureCubeArray (t3, t4, b4)
    // ProbeManager is initialized and loaded in CScene::Initialize()
    auto& probeManager = scene.GetProbeManager();
    probeManager.Bind(context);

    // Bind Light Probe StructuredBuffer (t15, b5)
    // LightProbeManager provides SH coefficients for diffuse IBL
    auto& lightProbeManager = scene.GetLightProbeManager();
    lightProbeManager.Bind(context);

    // Bind Volumetric Lightmap (t20-t24, b6)
    // VolumetricLightmap provides per-pixel SH sampling (highest quality GI)
    auto& volumetricLightmap = scene.GetVolumetricLightmap();
    volumetricLightmap.Bind(context);

    // Collect render items (with per-object probe selection)
    std::vector<RenderItem> opaqueItems;
    std::vector<RenderItem> transparentItems;
    collectRenderItems(scene, eye, opaqueItems, transparentItems, &probeManager);

    // Render Opaque
    { CScopedDebugEvent evt(context, L"Opaque Pass");
    renderOpaquePass(context, opaqueItems,
                     m_inputLayout.Get(), m_vs.Get(), m_ps.Get(),
                     m_rsSolid.Get(), m_depthStateDefault.Get(),
                     m_cbFrame.Get(), m_cbObj.Get(), m_sampler.Get());
    }

    // Render Skybox
    { CScopedDebugEvent evt(context, L"Skybox");
    CScene::Instance().GetSkybox().Render(view, proj);
    }

    // Render Transparent
    { CScopedDebugEvent evt(context, L"Transparent Pass");
    renderTransparentPass(context, transparentItems,
                          m_inputLayout.Get(), m_vs.Get(), m_ps.Get(),
                          m_rsSolid.Get(), m_depthStateTransparent.Get(),
                          m_blendStateTransparent.Get(),
                          m_cbFrame.Get(), m_cbObj.Get(), m_sampler.Get());
    }
}

void CSceneRenderer::createPipeline()
{
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return;

    std::string vsSource = LoadShaderSource("../source/code/Shader/MainPass.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/MainPass.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load shader files!");
        return;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    CShaderIncludeHandler includeHandler;

    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "MainPass.vs.hlsl", nullptr,
                           &includeHandler, "main", "vs_5_0",
                           compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) CFFLog::Error("VS Error: %s", (const char*)err->GetBufferPointer());
        return;
    }

    hr = D3DCompile(psSource.c_str(), psSource.size(), "MainPass.ps.hlsl", nullptr,
                   &includeHandler, "main", "ps_5_0",
                   compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) CFFLog::Error("PS Error: %s", (const char*)err->GetBufferPointer());
        return;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,     0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,     0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, 5, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(CB_Frame);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cb, nullptr, m_cbFrame.GetAddressOf());
    cb.ByteWidth = sizeof(CB_Object);
    device->CreateBuffer(&cb, nullptr, m_cbObj.GetAddressOf());
    cb.ByteWidth = sizeof(CB_ClusteredParams);
    device->CreateBuffer(&cb, nullptr, m_cbClusteredParams.GetAddressOf());

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, m_sampler.GetAddressOf());
}

void CSceneRenderer::createRasterStates()
{
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) return;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, m_rsSolid.GetAddressOf());

    rd.FillMode = D3D11_FILL_WIREFRAME;
    device->CreateRasterizerState(&rd, m_rsWire.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    dsd.StencilEnable = FALSE;
    device->CreateDepthStencilState(&dsd, m_depthStateDefault.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsdTransparent{};
    dsdTransparent.DepthEnable = TRUE;
    dsdTransparent.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsdTransparent.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dsdTransparent.StencilEnable = FALSE;
    device->CreateDepthStencilState(&dsdTransparent, m_depthStateTransparent.GetAddressOf());

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blendDesc, m_blendStateTransparent.GetAddressOf());
}
