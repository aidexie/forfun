
#include "Renderer.h"
#include "ObjLoader.h"
#include "TextureLoader.h"
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>

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
static float getDeltaTime() {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<float> d = now - gPrev;
    gPrev = now;
    return d.count();
}

bool Renderer::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_width = width; m_height = height;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width  = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    if (FAILED(D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, _countof(levels), D3D11_SDK_VERSION,
        &sd, m_swapchain.GetAddressOf(),
        m_device.GetAddressOf(), &m_featureLevel,
        m_context.GetAddressOf()))) {
        MessageBoxA(nullptr, "D3D11CreateDeviceAndSwapChain failed", "Error", MB_ICONERROR);
        return false;
    }

    createBackbufferAndDepth(m_width, m_height);
    createPipeline();
    createRasterStates();

    // Load textures if present
    LoadTextureWIC(m_device.Get(), L"assets/bunny_albedo.png", m_albedoSRV, true);
    LoadTextureWIC(m_device.Get(), L"assets/bunny_normal.png", m_normalSRV, false);

    // Load OBJ bunny
    tryLoadOBJ("assets/bunny.obj", true, true, 2.0f, XMMatrixIdentity());

    gPrev = std::chrono::steady_clock::now();
    return true;
}

Renderer::GpuMesh Renderer::upload(const MeshCPU_PNT& m)
{
    GpuMesh g;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(m.vertices.size() * sizeof(VertexPNT));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initVB{ m.vertices.data(), 0, 0 };
    m_device->CreateBuffer(&bd, &initVB, g.vbo.GetAddressOf());

    D3D11_BUFFER_DESC ib{};
    ib.ByteWidth = (UINT)(m.indices.size() * sizeof(uint32_t));
    ib.Usage = D3D11_USAGE_DEFAULT;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initIB{ m.indices.data(), 0, 0 };
    m_device->CreateBuffer(&ib, &initIB, g.ibo.GetAddressOf());

    g.indexCount = (UINT)m.indices.size();
    return g;
}

bool Renderer::tryLoadOBJ(const std::string& path, bool flipZ, bool flipWinding, float targetDiag, XMMATRIX world)
{
    MeshCPU_PNT m;
    if (!LoadOBJ_PNT(path, m, flipZ, flipWinding)) {
        MessageBoxA(nullptr, ("OBJ not found or failed: "+path).c_str(), "Info", MB_ICONINFORMATION);
        return false;
    }
    RecenterAndScale(m, targetDiag);
    auto gm = upload(m);
    gm.world = world;
    m_meshes.push_back(std::move(gm));
    return true;
}

void Renderer::createBackbufferAndDepth(UINT w, UINT h)
{
    ComPtr<ID3D11Texture2D> back;
    m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)back.GetAddressOf());
    m_device->CreateRenderTargetView(back.Get(), nullptr, m_rtv.GetAddressOf());

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    m_device->CreateTexture2D(&td, nullptr, m_depthTex.GetAddressOf());
    m_device->CreateDepthStencilView(m_depthTex.Get(), nullptr, m_dsv.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    m_device->CreateDepthStencilState(&ds, m_dss.GetAddressOf());
}

void Renderer::destroyBackbufferAndDepth()
{
    m_dsv.Reset();
    m_depthTex.Reset();
    m_rtv.Reset();
}

void Renderer::createPipeline()
{
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

        float3 SRGBToLinear(float3 c){ return pow(c, 2.2); }
        float3 LinearToSRGB(float3 c){ return pow(saturate(c), 1.0/2.2); }

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
            float3 albedo = SRGBToLinear(gAlbedo.Sample(gSamp, i.uv).rgb);
            float3 nTS    = gNormal.Sample(gSamp, i.uv).xyz * 2.0 - 1.0;
            // nTS.y = -nTS.y; // enable if needed
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
            return float4( LinearToSRGB(colorLin), 1.0 );
        }
    )";
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,     0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,     0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,        0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());

    // constant buffers
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(CB_Frame);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    m_device->CreateBuffer(&cb, nullptr, m_cbFrame.GetAddressOf());
    cb.ByteWidth = sizeof(CB_Object);
    m_device->CreateBuffer(&cb, nullptr, m_cbObj.GetAddressOf());

    // sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sd, m_sampler.GetAddressOf());
}

void Renderer::createRasterStates()
{
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rd, m_rsSolid.GetAddressOf());

    rd.FillMode = D3D11_FILL_WIREFRAME;
    m_device->CreateRasterizerState(&rd, m_rsWire.GetAddressOf());
}

void Renderer::OnMouseDelta(int dx, int dy)
{
    if (!m_rmbLook) return;
    const float sens = 0.0022f;
    m_yaw   -= dx * sens;
    m_pitch += -dy * sens;
    const float limit = 1.5533f;
    m_pitch = std::clamp(m_pitch, -limit, limit);
}
void Renderer::OnRButton(bool down) { m_rmbLook = down; }

void Renderer::ResetCameraLookAt(const XMFLOAT3& eye, const XMFLOAT3& target)
{
    m_camPos = eye;
    XMVECTOR e = XMLoadFloat3(&m_camPos);
    XMVECTOR t = XMLoadFloat3(&target);
    XMVECTOR d = XMVector3Normalize(t - e);
    XMFLOAT3 f; XMStoreFloat3(&f, d);
    m_yaw   = std::atan2(f.z, f.x);
    m_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

void Renderer::updateInput(float dt)
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

void Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapchain) return;
    m_width = width; m_height = height;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    destroyBackbufferAndDepth();
    m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createBackbufferAndDepth(width, height);
    createRasterStates();
}

void Renderer::Render()
{
    float dt = getDeltaTime();
    updateInput(dt);

    m_context->RSSetState(m_rsSolid.Get());

    D3D11_VIEWPORT vp{}; vp.Width=(FLOAT)m_width; vp.Height=(FLOAT)m_height; vp.MinDepth=0.0f; vp.MaxDepth=1.0f;
    m_context->RSSetViewports(1, &vp);
    FLOAT clear[4] = { 0.05f, 0.06f, 0.10f, 1.0f };
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), m_dsv.Get());
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);
    m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL, 1.0f, 0);
    m_context->OMSetDepthStencilState(m_dss.Get(), 0);

    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_cbObj.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_cbFrame.GetAddressOf());
    m_context->PSSetConstantBuffers(1, 1, m_cbObj.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = { m_albedoSRV.Get(), m_normalSRV.Get() };
    m_context->PSSetShaderResources(0, 2, srvs);

    // Camera
    float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    XMVECTOR eye = XMLoadFloat3(&m_camPos);
    XMVECTOR forward = XMVector3Normalize(XMVectorSet(cp*cy, sp, cp*sy, 0.0f));
    XMVECTOR at  = eye + forward;
    XMVECTOR up  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
    float aspect = (float)m_width / (float)m_height;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);

    // Frame constants
    CB_Frame cf{};
    cf.view = XMMatrixTranspose(view);
    cf.proj = XMMatrixTranspose(proj);
    cf.lightDirWS = XMFLOAT3(0.4f,-1.0f,0.2f);
    {
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cf.lightDirWS));
        XMStoreFloat3(&cf.lightDirWS, L);
    }
    cf.lightColor = XMFLOAT3(1,1,1);
    cf.camPosWS = m_camPos;
    cf.ambient = 0.08f;
    cf.specPower = 64.0f;
    cf.specIntensity = 0.3f;
    cf.normalScale = 1.0f;
    m_context->UpdateSubresource(m_cbFrame.Get(), 0, nullptr, &cf, 0, 0);

    for (auto& gm : m_meshes) {
        CB_Object co{ XMMatrixTranspose(gm.world) };
        m_context->UpdateSubresource(m_cbObj.Get(), 0, nullptr, &co, 0, 0);

        UINT stride = sizeof(VertexPNT), offset = 0;
        ID3D11Buffer* vbo = gm.vbo.Get();
        m_context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
        m_context->IASetIndexBuffer(gm.ibo.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(gm.indexCount, 0, 0);
    }

    m_swapchain->Present(1, 0);
}

void Renderer::Shutdown()
{
    m_meshes.clear();
    m_cbFrame.Reset();
    m_cbObj.Reset();
    m_inputLayout.Reset();
    m_vs.Reset();
    m_ps.Reset();
    destroyBackbufferAndDepth();
    m_context.Reset();
    m_device.Reset();
    m_swapchain.Reset();
}
