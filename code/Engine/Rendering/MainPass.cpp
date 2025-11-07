#include "MainPass.h"
#include "Core/DX11Context.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Scene.h"
#include "GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <d3d11.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct alignas(16) CB_Frame {
    DirectX::XMMATRIX view;
    DirectX::XMMATRIX proj;
    DirectX::XMFLOAT3 lightDirWS; float _pad0;
    DirectX::XMFLOAT3 lightColor; float _pad1;
    DirectX::XMFLOAT3 camPosWS;  float _pad2;
    float ambient; float specPower; float specIntensity; float normalScale;
};
struct alignas(16) CB_Object { DirectX::XMMATRIX world; };

static auto gPrev = std::chrono::steady_clock::now();

bool MainPass::Initialize()
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
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
    m_defaultAlbedo = MakeSolidSRV(255,255,255,255, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); // sRGB
    m_defaultNormal = MakeSolidSRV(128,128,255,255, DXGI_FORMAT_R8G8B8A8_UNORM);     // 线性

    gPrev = std::chrono::steady_clock::now();
    return true;
}


void MainPass::createPipeline()
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    if (!device) return;

    static const char* kVS = R"(
        cbuffer CB_Frame  : register(b0) {
            float4x4 gView;
            float4x4 gProj;
            float3   gLightDirWS; float _pad0;
            float3   gLightColor; float _pad1;
            float3   gCamPosWS;   float _pad2;
            float    gAmbient; float gSpecPower; float gSpecIntensity; float gNormalScale;
        }
        cbuffer CB_Object : register(b1) { float4x4 gWorld; }

        struct VSIn { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; float4 t:TANGENT; };
        struct VSOut{
            float4 posH:SV_Position;
            float3 posWS:TEXCOORD0;
            float2 uv:TEXCOORD1;
            float3x3 TBN:TEXCOORD2;
        };
        VSOut main(VSIn i){
            VSOut o;
            float4 posWS = mul(float4(i.p,1), gWorld);
            float3 nWS = normalize(mul(float4(i.n,0), gWorld).xyz);
            float3 tWS = normalize(mul(float4(i.t.xyz,0), gWorld).xyz);
            float3 bWS = normalize(cross(nWS, tWS) * i.t.w);
            o.TBN = float3x3(tWS, bWS, nWS);
            o.posWS = posWS.xyz;
            o.uv = i.uv;
            float4 posV = mul(posWS, gView);
            o.posH = mul(posV, gProj);
            return o;
        }
    )";
    static const char* kPS = R"(
        Texture2D gAlbedo : register(t0);
        Texture2D gNormal : register(t1);
        SamplerState gSamp: register(s0);

        cbuffer CB_Frame  : register(b0) {
            float4x4 gView;
            float4x4 gProj;
            float3   gLightDirWS; float _pad0;
            float3   gLightColor; float _pad1;
            float3   gCamPosWS;   float _pad2;
            float    gAmbient; float gSpecPower; float gSpecIntensity; float gNormalScale;
        }
        cbuffer CB_Object : register(b1) { float4x4 gWorld; }

        struct PSIn{
            float4 posH:SV_Position;
            float3 posWS:TEXCOORD0;
            float2 uv:TEXCOORD1;
            float3x3 TBN:TEXCOORD2;
        };

        float4 main(PSIn i):SV_Target{
            float3 albedo = gAlbedo.Sample(gSamp, i.uv).rgb;
            float3 nTS    = gNormal.Sample(gSamp, i.uv).xyz * 2.0 - 1.0;
            nTS.xy *= gNormalScale;
            nTS = normalize(nTS);
            float3 nWS = normalize(mul(nTS, i.TBN));

            float3 L = normalize(-gLightDirWS);
            float3 V = normalize(gCamPosWS - i.posWS);
            float3 H = normalize(L+V);
            float NdotL = saturate(dot(nWS,L));
            float NdotH = saturate(dot(nWS,H));

            float3 diff = albedo * NdotL;
            float3 spec = gSpecIntensity * pow(NdotH, gSpecPower) * NdotL * gLightColor;

            float3 colorLin = gAmbient * albedo + diff + spec;
            return float4(colorLin, 1.0);
        }
    )";
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,     0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,     0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());

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

void MainPass::createRasterStates()
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    if (!device) return;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, m_rsSolid.GetAddressOf());

    rd.FillMode = D3D11_FILL_WIREFRAME;
    device->CreateRasterizerState(&rd, m_rsWire.GetAddressOf());
}

void MainPass::OnMouseDelta(int dx, int dy)
{
    if (!m_rmbLook) return;
    const float sens = 0.0022f;
    m_yaw   -= dx * sens;
    m_pitch += -dy * sens;
    const float limit = 1.5533f;
    m_pitch = std::clamp(m_pitch, -limit, limit);
}
void MainPass::OnRButton(bool down) { m_rmbLook = down; }

void MainPass::ResetCameraLookAt(const XMFLOAT3& eye, const XMFLOAT3& target)
{
    m_camPos = eye;
    XMVECTOR e = XMLoadFloat3(&m_camPos);
    XMVECTOR t = XMLoadFloat3(&target);
    XMVECTOR d = XMVector3Normalize(t - e);
    XMFLOAT3 f; XMStoreFloat3(&f, d);
    m_yaw   = std::atan2(f.z, f.x);
    m_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

void MainPass::updateInput(float dt)
{
    auto down = [](int vk){ return (GetAsyncKeyState(vk) & 0x8000) != 0; };

    const float speed = 1.8f;
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(cp*cy, sp, cp*sy, 0.0f));
    XMVECTOR right   = XMVector3Normalize(XMVector3Cross(forward, XMVectorSet(0,1,0,0)));

    XMVECTOR delta = XMVectorZero();
    if (down('W')) delta += forward * speed * dt;
    if (down('S')) delta -= forward * speed * dt;
    if (down('A')) delta -= right   * speed * dt;
    if (down('D')) delta += right   * speed * dt;

    XMFLOAT3 d; XMStoreFloat3(&d, delta);
    m_camPos.x += d.x; m_camPos.y += d.y; m_camPos.z += d.z;

    if (down('R')) { ResetCameraLookAt(XMFLOAT3(-6.0f,0.8f,0.0f), XMFLOAT3(0,0,0)); }
}

void MainPass::renderScene(Scene& scene, float dt)
{
    ID3D11DeviceContext* context = DX11Context::Instance().GetContext();
    if (!context) return;

    updateInput(dt);

    // 绑定视口与目标
    D3D11_VIEWPORT vp{}; vp.Width=(FLOAT)m_off.w; vp.Height=(FLOAT)m_off.h; vp.MinDepth=0.0f; vp.MaxDepth=1.0f;
    context->RSSetViewports(1, &vp);
    context->RSSetState(m_rsSolid.Get());

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

    // Camera
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    float cp = std::cos(m_pitch), sp = std::cos(m_pitch) == 0.f ? 0.f : std::sin(m_pitch);
    XMVECTOR eye = XMLoadFloat3(&m_camPos);
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(cp*cy, sp, cp*sy, 0.0f));
    XMVECTOR at  = eye + forward;
    XMVECTOR up  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
    float aspect = (m_off.h > 0) ? (float)m_off.w / (float)m_off.h : 1.0f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);

    CB_Frame cf{};
    cf.view = XMMatrixTranspose(view);
    cf.proj = XMMatrixTranspose(proj);
    cf.lightDirWS = XMFLOAT3(0.4f,-1.0f,0.2f);
    {
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cf.lightDirWS));
        XMStoreFloat3(&cf.lightDirWS, L);
    }
    cf.lightColor = XMFLOAT3(1,1,1);
    cf.camPosWS   = m_camPos;
    cf.ambient = 0.38f;
    cf.specPower = 64.0f;
    cf.specIntensity = 0.3f;
    cf.normalScale   = 1.0f;
    context->UpdateSubresource(m_cbFrame.Get(), 0, nullptr, &cf, 0, 0);

    // 遍历场景中的所有对象并渲染
    for (auto& objPtr : scene.world.Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<MeshRenderer>();
        auto* transform = obj->GetComponent<Transform>();

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

        // 绘制所有子网格（glTF 可能有多个）
        for (auto& gpuMesh : meshRenderer->meshes) {
            if (!gpuMesh) continue;

            // 更新物体矩阵
            CB_Object co{ XMMatrixTranspose(worldMatrix) };
            context->UpdateSubresource(m_cbObj.Get(), 0, nullptr, &co, 0, 0);

            // 绑定顶点和索引缓冲
            UINT stride = sizeof(VertexPNT), offset = 0;
            ID3D11Buffer* vbo = gpuMesh->vbo.Get();
            context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
            context->IASetIndexBuffer(gpuMesh->ibo.Get(), DXGI_FORMAT_R32_UINT, 0);

            // 绑定纹理（如果为空则使用默认纹理）
            ID3D11ShaderResourceView* srvs[2] = {
                gpuMesh->albedoSRV.Get() ? gpuMesh->albedoSRV.Get() : m_defaultAlbedo.Get(),
                gpuMesh->normalSRV.Get() ? gpuMesh->normalSRV.Get() : m_defaultNormal.Get()
            };
            context->PSSetShaderResources(0, 2, srvs);

            // 绘制
            context->DrawIndexed(gpuMesh->indexCount, 0, 0);
        }
    }
}

void MainPass::ensureOffscreen(UINT w, UINT h)
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    if (!device) return;

    if (w == 0 || h == 0) return;
    if (m_off.color && w == m_off.w && h == m_off.h) return;

    m_off.Reset();
    m_off.w = w; m_off.h = h;

    // Color
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // keep linear; switch to _SRGB if you want post-sRGB
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

    // Depth
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    device->CreateTexture2D(&dd, nullptr, m_off.depth.GetAddressOf());
    device->CreateDepthStencilView(m_off.depth.Get(), nullptr, m_off.dsv.GetAddressOf());
}

void MainPass::Render(Scene& scene, UINT w, UINT h, float dt)
{
    ID3D11DeviceContext* context = DX11Context::Instance().GetContext();
    if (!context) return;

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

    renderScene(scene, dt);
}

void MainPass::Shutdown()
{
    m_cbFrame.Reset();
    m_cbObj.Reset();
    m_inputLayout.Reset();
    m_vs.Reset();
    m_ps.Reset();
    m_sampler.Reset();
    m_rsSolid.Reset();
    m_rsWire.Reset();
    m_defaultAlbedo.Reset();
    m_defaultNormal.Reset();
}
