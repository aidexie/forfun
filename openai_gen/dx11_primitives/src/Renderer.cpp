
#include "Renderer.h"
#include <array>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct alignas(16) CB_MVP { DirectX::XMMATRIX mvp; };
static auto gStart = std::chrono::steady_clock::now();

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

    // Upload primitives
    m_meshes.push_back(upload(MakeCube(1.2f)));
    m_meshes.push_back(upload(MakeCuboid(1.2f, 0.8f, 0.6f)));
    m_meshes.push_back(upload(MakeCylinder(0.5f, 1.3f, 36)));
    m_meshes.push_back(upload(MakeSphere(0.6f, 32, 16)));

    return true;
}

Renderer::GpuMesh Renderer::upload(const MeshCPU& m)
{
    GpuMesh g;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(m.vertices.size() * sizeof(VertexPC));
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
        cbuffer PerDraw : register(b0) { float4x4 mvp; }
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
    D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);
    D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &err);
    m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                           D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float)*3,             D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());

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
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - gStart).count();
    XMVECTOR eye = XMVectorSet(0.0f, 1.2f, -6.0f, 1.0f);
    XMVECTOR at  = XMVectorSet(0.0f, 0.7f,  0.0f, 1.0f);
    XMVECTOR up  = XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
    float aspect = (float)m_width / (float)m_height;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);

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
    m_context->VSSetConstantBuffers(0, 1, m_cbuf.GetAddressOf());

    std::array<XMMATRIX,4> worlds = {
        XMMatrixRotationY(t*0.8f) * XMMatrixTranslation(-1.8f, 0.0f, 0.0f),
        XMMatrixRotationX(t*0.6f) * XMMatrixTranslation( 1.8f, 0.0f, 0.0f),
        XMMatrixRotationZ(t*0.7f) * XMMatrixTranslation(-1.8f, 1.8f, 0.0f),
        XMMatrixRotationY(t*1.1f) * XMMatrixTranslation( 1.8f, 1.8f, 0.0f)
    };

    for (size_t k=0; k<m_meshes.size(); ++k) {
        XMMATRIX mvpT = XMMatrixTranspose(worlds[k] * view * proj);
        CB_MVP cb{ mvpT };
        m_context->UpdateSubresource(m_cbuf.Get(), 0, nullptr, &cb, 0, 0);

        UINT stride = sizeof(VertexPC), offset = 0;
        ID3D11Buffer* vbo = m_meshes[k].vbo.Get();
        m_context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
        m_context->IASetIndexBuffer(m_meshes[k].ibo.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_meshes[k].indexCount, 0, 0);
    }

    m_swapchain->Present(1, 0);
}

void Renderer::Shutdown()
{
    m_meshes.clear();
    m_cbuf.Reset();
    m_inputLayout.Reset();
    m_vs.Reset();
    m_ps.Reset();
    destroyBackbufferAndDepth();
    m_context.Reset();
    m_device.Reset();
    m_swapchain.Reset();
}
