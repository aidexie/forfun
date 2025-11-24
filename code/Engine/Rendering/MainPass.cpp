#include "MainPass.h"
#include "ShadowPass.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Scene.h"
#include "GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include "Components/Material.h"
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <d3d11.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Helper function to load shader source from file
static std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct alignas(16) CB_Frame {
    DirectX::XMMATRIX view;
    DirectX::XMMATRIX proj;

    // CSM parameters
    int cascadeCount;
    int debugShowCascades;  // 0=off, 1=on
    int enableSoftShadows;  // 0=hard, 1=soft (PCF)
    float cascadeBlendRange;  // Blend range at cascade boundaries (0-1)
    DirectX::XMFLOAT4 cascadeSplits;  // HLSL treats array as float4, use XMFLOAT4
    DirectX::XMMATRIX lightSpaceVPs[4];

    // Lighting (PBR - removed Blinn-Phong parameters)
    DirectX::XMFLOAT3 lightDirWS; float _pad1;
    DirectX::XMFLOAT3 lightColor; float _pad2;
    DirectX::XMFLOAT3 camPosWS;  float _pad3;
    float shadowBias;
    float iblIntensity;  // IBL ambient multiplier (0-1 typical, can go higher for artistic effect)
    DirectX::XMFLOAT2 _pad4;
};
struct alignas(16) CB_Object {
    DirectX::XMMATRIX world;
    DirectX::XMFLOAT3 albedo; float metallic;
    float roughness;
    int hasMetallicRoughnessTexture;  // 1 = use texture, 0 = use CB values
    DirectX::XMFLOAT2 _pad;
};

static auto gPrev = std::chrono::steady_clock::now();

bool CMainPass::Initialize()
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return false;

    // --- 管线与状态 ---
    createPipeline();
    createRasterStates();

    // --- 默认兜底纹理 ---
    auto MakeSolidSRV = [&](uint8_t r,uint8_t g,uint8_t b,uint8_t a, DXGI_FORMAT fmt){
        D3D11_TEXTURE2D_DESC td{}; td.Width=1; td.Height=1; td.MipLevels=1; td.ArraySize=1;
        td.Format=fmt; td.SampleDesc.Count=1; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        uint32_t px = (uint32_t(r) | (uint32_t(g)<<8) | (uint32_t(b)<<16) | (uint32_t(a)<<24));
        D3D11_SUBRESOURCE_DATA srd{ &px, 4, 0 };
        ComPtr<ID3D11Texture2D> tex; device->CreateTexture2D(&td, &srd, tex.GetAddressOf());
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=fmt; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> srv; device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());
        return srv;
    };
    m_defaultAlbedo = MakeSolidSRV(255,255,255,255, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); // sRGB (white)
    m_defaultNormal = MakeSolidSRV(128,128,255,255, DXGI_FORMAT_R8G8B8A8_UNORM);     // Linear (tangent-space up)
    m_defaultMetallicRoughness = MakeSolidSRV(255,255,255,255, DXGI_FORMAT_R8G8B8A8_UNORM);  // Linear (G=Roughness=1, B=Metallic=1)
                                                                                              // All white = Material component values take full effect

    // Skybox is now managed by Scene singleton
    // No need to initialize here - Scene::Instance().Initialize() handles it

    // --- Initialize Post-Process ---
    m_postProcess.Initialize();

    // --- Initialize Debug Line Pass ---
    m_debugLinePass.Initialize();

    // --- Initialize Grid Pass ---
    CGridPass::Instance().Initialize();

    gPrev = std::chrono::steady_clock::now();
    return true;
}


void CMainPass::createPipeline()
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Load shader source from files
    // Path is relative to E:\forfun\assets (working directory)
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

    // Compile Vertex Shader
    HRESULT hr = D3DCompile(vsSource.c_str(), vsSource.size(), "MainPass.vs.hlsl", nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== VERTEX SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
        return;
    }

    // Compile Pixel Shader
    hr = D3DCompile(psSource.c_str(), psSource.size(), "MainPass.ps.hlsl", nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("=== PIXEL SHADER COMPILATION ERROR ===");
            CFFLog::Error("%s", (const char*)err->GetBufferPointer());
        }
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

    // constant buffers
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(CB_Frame);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cb, nullptr, m_cbFrame.GetAddressOf());
    cb.ByteWidth = sizeof(CB_Object);
    device->CreateBuffer(&cb, nullptr, m_cbObj.GetAddressOf());

    // sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, m_sampler.GetAddressOf());
}

void CMainPass::createRasterStates()
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    // Rasterizer states
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, m_rsSolid.GetAddressOf());

    rd.FillMode = D3D11_FILL_WIREFRAME;
    device->CreateRasterizerState(&rd, m_rsWire.GetAddressOf());

    // Default depth stencil state (depth test enabled, depth write enabled)
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    dsd.StencilEnable = FALSE;
    device->CreateDepthStencilState(&dsd, m_depthStateDefault.GetAddressOf());
}

void CMainPass::OnMouseDelta(int dx, int dy)
{
    if (!m_rmbLook) return;
    const float sens = 0.0022f;
    m_yaw   -= dx * sens;
    m_pitch += -dy * sens;
    const float limit = 1.5533f;
    m_pitch = std::clamp(m_pitch, -limit, limit);
}
void CMainPass::OnRButton(bool down) { m_rmbLook = down; }

void CMainPass::UpdateCamera(UINT viewportWidth, UINT viewportHeight, float dt)
{
    updateInput(dt);

    // Calculate camera matrices
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    float cp = std::cos(m_pitch), sp = std::cos(m_pitch) == 0.f ? 0.f : std::sin(m_pitch);
    XMVECTOR eye = XMLoadFloat3(&m_camPos);
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(cp*cy, sp, cp*sy, 0.0f));
    XMVECTOR at = eye + forward;
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    m_cameraView = XMMatrixLookAtLH(eye, at, up);

    float aspect = (viewportHeight > 0) ? (float)viewportWidth / (float)viewportHeight : 1.0f;
    m_cameraProj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 1000.0f);
}

void CMainPass::ResetCameraLookAt(const XMFLOAT3& eye, const XMFLOAT3& target)
{
    m_camPos = eye;
    XMVECTOR e = XMLoadFloat3(&m_camPos);
    XMVECTOR t = XMLoadFloat3(&target);
    XMVECTOR d = XMVector3Normalize(t - e);
    XMFLOAT3 f; XMStoreFloat3(&f, d);
    m_yaw   = std::atan2(f.z, f.x);
    m_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

void CMainPass::updateInput(float dt)
{
    auto down = [](int vk){ return (GetAsyncKeyState(vk) & 0x8000) != 0; };

    const float speed = 5.0f;
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(cp*cy, sp, cp*sy, 0.0f));
    XMVECTOR right   = XMVector3Normalize(XMVector3Cross(forward, XMVectorSet(0,1,0,0)));

    XMVECTOR delta = XMVectorZero();
    if (down('W')) delta += forward * speed * dt;
    if (down('S')) delta -= forward * speed * dt;
    if (down('A')) delta += right   * speed * dt;
    if (down('D')) delta -= right   * speed * dt;

    XMFLOAT3 d; XMStoreFloat3(&d, delta);
    m_camPos.x += d.x; m_camPos.y += d.y; m_camPos.z += d.z;

    if (down('R')) { ResetCameraLookAt(XMFLOAT3(-6.0f,0.8f,0.0f), XMFLOAT3(0,0,0)); }
}

void CMainPass::renderScene(CScene& scene, float dt, const CShadowPass::Output* shadowData)
{
    ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
    if (!context) return;

    updateInput(dt);

    // 绑定视口与目标
    D3D11_VIEWPORT vp{}; vp.Width=(FLOAT)m_off.w; vp.Height=(FLOAT)m_off.h; vp.MinDepth=0.0f; vp.MaxDepth=1.0f;
    context->RSSetViewports(1, &vp);
    context->RSSetState(m_rsSolid.Get());
    context->OMSetDepthStencilState(m_depthStateDefault.Get(), 0);

    // 绑定管线
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, m_cbObj.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
    context->PSSetConstantBuffers(1, 1, m_cbObj.GetAddressOf());
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Bind shadow sampler if shadow data is available
    if (shadowData && shadowData->shadowSampler) {
        context->PSSetSamplers(1, 1, &shadowData->shadowSampler);
    }

    // Use cached camera matrices (computed in UpdateCamera)
    XMMATRIX view = m_cameraView;
    XMMATRIX proj = m_cameraProj;
    XMVECTOR eye = XMLoadFloat3(&m_camPos);

    // Collect DirectionalLight from scene
    SDirectionalLight* dirLight = nullptr;
    for (auto& objPtr : scene.GetWorld().Objects()) {
        dirLight = objPtr->GetComponent<SDirectionalLight>();
        if (dirLight) break;  // Use first DirectionalLight found
    }

    CB_Frame cf{};
    cf.view = XMMatrixTranspose(view);
    cf.proj = XMMatrixTranspose(proj);

    // Set CSM parameters from shadow data
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
        cf.enableSoftShadows = 1;  // Default to soft shadows
        cf.cascadeBlendRange = 0.0f;
        cf.cascadeSplits = XMFLOAT4(100.0f, 100.0f, 100.0f, 100.0f);
        for (int i = 0; i < 4; ++i) {
            cf.lightSpaceVPs[i] = XMMatrixTranspose(XMMatrixIdentity());
        }
    }

    // Set light direction and color from DirectionalLight component (or defaults)
    if (dirLight) {
        cf.lightDirWS = dirLight->GetDirection();
        cf.lightColor = XMFLOAT3(
            dirLight->color.x * dirLight->intensity,
            dirLight->color.y * dirLight->intensity,
            dirLight->color.z * dirLight->intensity
        );
        cf.shadowBias = dirLight->shadow_bias;
        cf.iblIntensity = dirLight->ibl_intensity;  // Read from DirectionalLight component
    } else {
        // Default light if no DirectionalLight component exists
        cf.lightDirWS = XMFLOAT3(0.4f, -1.0f, 0.2f);
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cf.lightDirWS));
        XMStoreFloat3(&cf.lightDirWS, L);
        cf.lightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        cf.shadowBias = 0.005f;
        cf.iblIntensity = 1.0f;  // Default IBL intensity (when no DirectionalLight exists)
    }

    cf.camPosWS   = m_camPos;
    context->UpdateSubresource(m_cbFrame.Get(), 0, nullptr, &cf, 0, 0);

    // Bind shadow map array
    if (shadowData && shadowData->shadowMapArray) {
        context->PSSetShaderResources(2, 1, &shadowData->shadowMapArray);
    }

    // Bind IBL textures (t3, t4, t5)
    CIBLGenerator& iblGen = CScene::Instance().GetIBLGenerator();
    ID3D11ShaderResourceView* iblSRVs[3] = {
        iblGen.GetIrradianceMapSRV(),    // t3: Irradiance cubemap
        iblGen.GetPreFilteredMapSRV(),   // t4: Pre-filtered environment cubemap
        iblGen.GetBrdfLutSRV()           // t5: BRDF LUT
    };
    context->PSSetShaderResources(3, 3, iblSRVs);

    // 遍历场景中的所有对象并渲染
    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) {
            continue;
        }

        // 确保资源已上传
        meshRenderer->EnsureUploaded();
        if (meshRenderer->meshes.empty()) {
            continue;
        }
        // 获取世界矩阵
        XMMATRIX worldMatrix = transform->WorldMatrix();

        // 获取材质组件（如果没有则使用默认值）
        auto* material = obj->GetComponent<SMaterial>();
        XMFLOAT3 albedo = material ? material->albedo : XMFLOAT3(1.0f, 1.0f, 1.0f);
        float metallic = material ? material->metallic : 0.0f;
        float roughness = material ? material->roughness : 0.5f;

        // 绘制所有子网格（glTF 可能有多个）
        for (auto& gpuMesh : meshRenderer->meshes) {
            if (!gpuMesh) continue;

            // Detect if using real texture or default fallback
            bool hasRealMetallicRoughnessTexture = gpuMesh->metallicRoughnessSRV.Get() &&
                                                   gpuMesh->metallicRoughnessSRV.Get() != m_defaultMetallicRoughness.Get();

            // 更新物体矩阵和材质属性
            CB_Object co{};
            co.world = XMMatrixTranspose(worldMatrix);
            co.albedo = albedo;
            co.metallic = metallic;
            co.roughness = roughness;
            co.hasMetallicRoughnessTexture = hasRealMetallicRoughnessTexture ? 1 : 0;
            context->UpdateSubresource(m_cbObj.Get(), 0, nullptr, &co, 0, 0);

            // 绑定顶点和索引缓冲
            UINT stride = sizeof(SVertexPNT), offset = 0;
            ID3D11Buffer* vbo = gpuMesh->vbo.Get();
            context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
            context->IASetIndexBuffer(gpuMesh->ibo.Get(), DXGI_FORMAT_R32_UINT, 0);

            // 绑定纹理（如果为空则使用默认纹理）
            // t0-t1: Albedo and Normal
            ID3D11ShaderResourceView* srvs[2] = {
                gpuMesh->albedoSRV.Get() ? gpuMesh->albedoSRV.Get() : m_defaultAlbedo.Get(),
                gpuMesh->normalSRV.Get() ? gpuMesh->normalSRV.Get() : m_defaultNormal.Get()
            };
            context->PSSetShaderResources(0, 2, srvs);

            // t6: Metallic/Roughness (G=Roughness, B=Metallic)
            ID3D11ShaderResourceView* metallicRoughnessSrv = gpuMesh->metallicRoughnessSRV.Get() ?
                                                             gpuMesh->metallicRoughnessSRV.Get() : m_defaultMetallicRoughness.Get();
            context->PSSetShaderResources(6, 1, &metallicRoughnessSrv);

            // 绘制
            context->DrawIndexed(gpuMesh->indexCount, 0, 0);
        }
    }

    // 渲染 Skybox（最后渲染，使用深度测试但不写入）
    // Skybox is now managed by Scene singleton
    CScene::Instance().GetSkybox().Render(view, proj);
}

void CMainPass::ensureOffscreen(UINT w, UINT h)
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    if (w == 0 || h == 0) return;
    if (m_off.color && w == m_off.w && h == m_off.h) return;

    m_off.Reset();
    m_off.w = w; m_off.h = h;

    // Color (HDR format for linear space intermediate rendering)
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR linear space
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&td, nullptr, m_off.color.GetAddressOf());

    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format = td.Format; rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_off.color.Get(), &rvd, m_off.rtv.GetAddressOf());

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_off.color.Get(), &sv, m_off.srv.GetAddressOf());

    // Depth (create as both depth-stencil and shader resource)
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS;  // Typeless to allow different view formats
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&dd, nullptr, m_off.depth.GetAddressOf());

    // Depth Stencil View (for writing)
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    device->CreateDepthStencilView(m_off.depth.Get(), &dsvDesc, m_off.dsv.GetAddressOf());

    // Shader Resource View (for reading depth in shaders)
    D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
    depthSRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;  // Read only depth channel
    depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSRVDesc.Texture2D.MostDetailedMip = 0;
    depthSRVDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_off.depth.Get(), &depthSRVDesc, m_off.depthSRV.GetAddressOf());

    // === Create LDR sRGB RT for final display ===
    m_offLDR.Reset();
    m_offLDR.w = w; m_offLDR.h = h;

    D3D11_TEXTURE2D_DESC ldrDesc{};
    ldrDesc.Width = w; ldrDesc.Height = h; ldrDesc.MipLevels = 1; ldrDesc.ArraySize = 1;
    ldrDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;  // TYPELESS: Allows creating views with different formats
    ldrDesc.SampleDesc.Count = 1;
    ldrDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&ldrDesc, nullptr, m_offLDR.color.GetAddressOf());

    D3D11_RENDER_TARGET_VIEW_DESC ldrRTVDesc{};
    ldrRTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // RTV: Write with gamma correction (linear → sRGB)
    ldrRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_offLDR.color.Get(), &ldrRTVDesc, m_offLDR.rtv.GetAddressOf());

    D3D11_SHADER_RESOURCE_VIEW_DESC ldrSRVDesc{};
    ldrSRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // SRV: Sample without sRGB decode (data is already gamma-corrected)
    ldrSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ldrSRVDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_offLDR.color.Get(), &ldrSRVDesc, m_offLDR.srv.GetAddressOf());
}

void CMainPass::Render(CScene& scene, UINT w, UINT h, float dt,
                      const CShadowPass::Output* shadowData)
{
    ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
    if (!context) return;

    // Unbind ALL resources from ALL stages BEFORE ensureOffscreen to avoid hazards
    ID3D11ShaderResourceView* nullSRV[8] = { nullptr };
    context->VSSetShaderResources(0, 8, nullSRV);
    context->PSSetShaderResources(0, 8, nullSRV);

    // Also unbind render targets before recreating resources
    context->OMSetRenderTargets(0, nullptr, nullptr);

    ensureOffscreen(w, h);

    ID3D11RenderTargetView* rtv = m_off.rtv.Get();
    ID3D11DepthStencilView* dsv = m_off.dsv.Get();
    context->OMSetRenderTargets(1, &rtv, dsv);

    D3D11_VIEWPORT vp{};
    vp.Width = float(m_off.w);
    vp.Height = float(m_off.h);
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    context->RSSetViewports(1, &vp);

    const float clear[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    context->ClearRenderTargetView(m_off.rtv.Get(), clear);
    if (m_off.dsv) context->ClearDepthStencilView(m_off.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Render scene to HDR RT (linear space)
    renderScene(scene, dt, shadowData);

    // UNBIND depth buffer before grid pass (to allow reading it as SRV)
    // Grid needs to sample depth buffer, but D3D11 doesn't allow SRV read from bound DSV
    context->OMSetRenderTargets(1, &rtv, nullptr);  // Unbind DSV

    // Render infinite grid (after skybox, before debug lines)
    CGridPass::Instance().Render(m_cameraView, m_cameraProj, m_camPos,
                                  m_off.depthSRV.Get(), w, h);

    // REBIND depth buffer for debug lines
    context->OMSetRenderTargets(1, &rtv, dsv);

    // Render debug lines on top of scene (with depth testing)
    m_debugLinePass.Render(m_cameraView, m_cameraProj, w, h);

    // Apply post-processing: Tone mapping + Gamma correction (HDR → LDR sRGB)
    m_postProcess.Render(m_off.srv.Get(), m_offLDR.rtv.Get(), w, h, 1.0f);
}

void CMainPass::Shutdown()
{
    // Skybox is now managed by Scene singleton - no need to shut down here
    m_postProcess.Shutdown();
    m_debugLinePass.Shutdown();
    CGridPass::Instance().Shutdown();
    m_cbFrame.Reset();
    m_cbObj.Reset();
    m_inputLayout.Reset();
    m_vs.Reset();
    m_ps.Reset();
    m_sampler.Reset();
    m_rsSolid.Reset();
    m_rsWire.Reset();
    m_depthStateDefault.Reset();
    m_defaultAlbedo.Reset();
    m_defaultNormal.Reset();
}
