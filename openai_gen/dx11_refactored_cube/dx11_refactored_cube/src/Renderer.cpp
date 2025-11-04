
#include "Renderer.h"
#include <array>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct alignas(16) CB_MVP { DirectX::XMMATRIX mvp; };

static auto gStartTime = std::chrono::steady_clock::now();

bool Renderer::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_width = width;
    m_height = height;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width  = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
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
            m_context.GetAddressOf())))
    {
        MessageBoxA(nullptr, "D3D11CreateDeviceAndSwapChain failed", "Error", MB_ICONERROR);
        return false;
    }

    createBackbufferAndDepth(m_width, m_height);
    createPipeline();
    return true;
}

void Renderer::createBackbufferAndDepth(UINT w, UINT h)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)back.GetAddressOf()))) {
        MessageBoxA(nullptr, "GetBuffer failed", "Error", MB_ICONERROR); return;
    }
    if (FAILED(m_device->CreateRenderTargetView(back.Get(), nullptr, m_rtv.GetAddressOf()))) {
        MessageBoxA(nullptr, "CreateRenderTargetView failed", "Error", MB_ICONERROR); return;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_depthTex.GetAddressOf()))) {
        MessageBoxA(nullptr, "CreateTexture2D(depth) failed", "Error", MB_ICONERROR); return;
    }
    if (FAILED(m_device->CreateDepthStencilView(m_depthTex.Get(), nullptr, m_dsv.GetAddressOf()))) {
        MessageBoxA(nullptr, "CreateDepthStencilView failed", "Error", MB_ICONERROR); return;
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    ds.StencilEnable = FALSE;
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
        cbuffer PerFrame : register(b0) { float4x4 mvp; }
        struct VSIn { float3 pos : POSITION; float3 col : COLOR; };
        struct VSOut { float4 pos : SV_Position; float3 col : COLOR; };
        VSOut main(VSIn i) {
            VSOut o;
            o.pos = mul(float4(i.pos,1), mvp);
            o.col = i.col;
            return o;
        }
    )";
    static const char* kPS = R"(
        struct PSIn { float4 pos : SV_Position; float3 col : COLOR; };
        float4 main(PSIn i) : SV_Target { return float4(i.col, 1.0); }
    )";

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err))) {
        MessageBoxA(nullptr, err ? (char*)err->GetBufferPointer() : "VS compile error", "HLSL Error", MB_ICONERROR); return;
    }
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err))) {
        MessageBoxA(nullptr, err ? (char*)err->GetBufferPointer() : "PS compile error", "HLSL Error", MB_ICONERROR); return;
    }

    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                           D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float)*3,             D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());

    struct V { float x,y,z; float r,g,b; };
    std::array<V, 8> verts{{
        {-1,-1,-1, 1,0,0},
        {-1, 1,-1, 0,1,0},
        { 1, 1,-1, 0,0,1},
        { 1,-1,-1, 1,1,0},
        {-1,-1, 1, 1,0,1},
        {-1, 1, 1, 0,1,1},
        { 1, 1, 1, 1,1,1},
        { 1,-1, 1, 0,0,0},
    }};
    std::array<uint16_t, 36> idx{{
        0,1,2,  0,2,3,
        4,6,5,  4,7,6,
        0,5,1,  0,4,5,
        3,2,6,  3,6,7,
        0,3,7,  0,7,4,
        1,5,6,  1,6,2
    }};

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(sizeof(verts));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initVB{ verts.data(), 0, 0 };
    m_device->CreateBuffer(&bd, &initVB, m_vbo.GetAddressOf());

    D3D11_BUFFER_DESC ib{};
    ib.ByteWidth = UINT(sizeof(idx));
    ib.Usage = D3D11_USAGE_DEFAULT;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initIB{ idx.data(), 0, 0 };
    m_device->CreateBuffer(&ib, &initIB, m_ibo.GetAddressOf());

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(CB_MVP);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    m_device->CreateBuffer(&cb, nullptr, m_cbuf.GetAddressOf());
}

void Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapchain) return;
    m_width = width; m_height = height;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    destroyBackbufferAndDepth();
    m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createBackbufferAndDepth(width, height);
}

void Renderer::Render()
{
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - gStartTime).count();

    XMMATRIX world = XMMatrixRotationY(t*0.9f) * XMMatrixRotationX(t*0.5f);
    XMVECTOR eye    = XMVectorSet(0.0f, 0.0f, -5.0f, 1.0f);
    XMVECTOR at     = XMVectorSet(0.0f, 0.0f,  0.0f, 1.0f);
    XMVECTOR up     = XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f);
    XMMATRIX view   = XMMatrixLookAtLH(eye, at, up);
    float aspect    = (float)m_width / (float)m_height;
    XMMATRIX proj   = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);

    CB_MVP cbData{ XMMatrixTranspose(world * view * proj) };
    m_context->UpdateSubresource(m_cbuf.Get(), 0, nullptr, &cbData, 0, 0);

    D3D11_VIEWPORT vp{};
    vp.Width  = (FLOAT)m_width;
    vp.Height = (FLOAT)m_height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    FLOAT clear[4] = { 0.07f, 0.07f, 0.1f, 1.0f };
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), m_dsv.Get());
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);
    m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL, 1.0f, 0);
    m_context->OMSetDepthStencilState(m_dss.Get(), 0);

    UINT stride = sizeof(float)*6;
    UINT offset = 0;
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetVertexBuffers(0, 1, m_vbo.GetAddressOf(), &stride, &offset);
    m_context->IASetIndexBuffer(m_ibo.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_cbuf.GetAddressOf());
    m_context->DrawIndexed(36, 0, 0);

    m_swapchain->Present(1, 0);
}

void Renderer::Shutdown()
{
    m_context.Reset();
    m_device.Reset();
    m_swapchain.Reset();
    destroyBackbufferAndDepth();
    m_vs.Reset();
    m_ps.Reset();
    m_inputLayout.Reset();
    m_vbo.Reset();
    m_ibo.Reset();
    m_cbuf.Reset();
}
