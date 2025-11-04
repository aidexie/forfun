
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

struct DX11State {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>        depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11DepthStencilState> dss;
    D3D_FEATURE_LEVEL              featureLevel{};

    ComPtr<ID3D11VertexShader>     vs;
    ComPtr<ID3D11PixelShader>      ps;
    ComPtr<ID3D11InputLayout>      inputLayout;
    ComPtr<ID3D11Buffer>           vbo;
    ComPtr<ID3D11Buffer>           ibo;
    ComPtr<ID3D11Buffer>           cbuf;

    UINT width  = 1280;
    UINT height = 720;
} g;

struct alignas(16) CB_MVP { DirectX::XMMATRIX mvp; };

void CreateDeviceAndSwapchain(HWND hwnd);
void CreateBackbufferAndDepth(UINT w, UINT h);
void DestroyBackbufferAndDepth();
void CreatePipeline();
void Resize(UINT w, UINT h);
void Render();

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DX11RotCubeClass";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    RECT rc{0, 0, (LONG)g.width, (LONG)g.height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"DX11 Rotating Cube (Minimal)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);

    CreateDeviceAndSwapchain(hwnd);
    CreateBackbufferAndDepth(g.width, g.height);
    CreatePipeline();

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;
        Render();
    }
    return 0;
}

void CreateDeviceAndSwapchain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width  = g.width;
    sd.BufferDesc.Height = g.height;
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

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, _countof(levels), D3D11_SDK_VERSION,
        &sd, g.swapchain.GetAddressOf(),
        g.device.GetAddressOf(), &g.featureLevel,
        g.context.GetAddressOf()
    );
    if (FAILED(hr)) {
        MessageBoxA(nullptr, "D3D11CreateDeviceAndSwapChain failed", "Error", MB_ICONERROR);
        PostQuitMessage(1);
    }
}

void CreateBackbufferAndDepth(UINT w, UINT h)
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = g.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backBuffer.GetAddressOf());
    if (FAILED(hr)) { MessageBoxA(nullptr, "GetBuffer failed", "Error", MB_ICONERROR); PostQuitMessage(1); return; }
    hr = g.device->CreateRenderTargetView(backBuffer.Get(), nullptr, g.rtv.GetAddressOf());
    if (FAILED(hr)) { MessageBoxA(nullptr, "CreateRenderTargetView failed", "Error", MB_ICONERROR); PostQuitMessage(1); return; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = g.device->CreateTexture2D(&td, nullptr, g.depthTex.GetAddressOf());
    if (FAILED(hr)) { MessageBoxA(nullptr, "CreateTexture2D (depth) failed", "Error", MB_ICONERROR); PostQuitMessage(1); return; }
    hr = g.device->CreateDepthStencilView(g.depthTex.Get(), nullptr, g.dsv.GetAddressOf());
    if (FAILED(hr)) { MessageBoxA(nullptr, "CreateDepthStencilView failed", "Error", MB_ICONERROR); PostQuitMessage(1); return; }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    ds.StencilEnable = FALSE;
    g.device->CreateDepthStencilState(&ds, g.dss.GetAddressOf());
}

void DestroyBackbufferAndDepth()
{
    g.dsv.Reset();
    g.depthTex.Reset();
    g.rtv.Reset();
}

void CreatePipeline()
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
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob))) {
        MessageBoxA(nullptr, errBlob ? (char*)errBlob->GetBufferPointer() : "VS compile error", "HLSL Error", MB_ICONERROR);
        PostQuitMessage(1); return;
    }
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &errBlob))) {
        MessageBoxA(nullptr, errBlob ? (char*)errBlob->GetBufferPointer() : "PS compile error", "HLSL Error", MB_ICONERROR);
        PostQuitMessage(1); return;
    }

    g.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, g.vs.GetAddressOf());
    g.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, g.ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                           D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float)*3,             D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g.device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), g.inputLayout.GetAddressOf());

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
    g.device->CreateBuffer(&bd, &initVB, g.vbo.GetAddressOf());

    D3D11_BUFFER_DESC ib{};
    ib.ByteWidth = UINT(sizeof(idx));
    ib.Usage = D3D11_USAGE_DEFAULT;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initIB{ idx.data(), 0, 0 };
    g.device->CreateBuffer(&ib, &initIB, g.ibo.GetAddressOf());

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(CB_MVP);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.device->CreateBuffer(&cb, nullptr, g.cbuf.GetAddressOf());
}

static auto gStart = std::chrono::steady_clock::now();

void Render()
{
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - gStart).count();
    using namespace DirectX;
    XMMATRIX world = XMMatrixRotationY(t*0.9f) * XMMatrixRotationX(t*0.5f);
    XMVECTOR eye    = XMVectorSet(0.0f, 0.0f, -5.0f, 1.0f);
    XMVECTOR at     = XMVectorSet(0.0f, 0.0f,  0.0f, 1.0f);
    XMVECTOR up     = XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f);
    XMMATRIX view   = XMMatrixLookAtLH(eye, at, up);
    float aspect    = (float)g.width / (float)g.height;
    XMMATRIX proj   = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 100.0f);
    XMMATRIX mvpT = XMMatrixTranspose(world * view * proj);
    CB_MVP cbData{ mvpT };
    g.context->UpdateSubresource(g.cbuf.Get(), 0, nullptr, &cbData, 0, 0);

    D3D11_VIEWPORT vp{};
    vp.Width  = (FLOAT)g.width;
    vp.Height = (FLOAT)g.height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    g.context->RSSetViewports(1, &vp);

    FLOAT clear[4] = { 0.07f, 0.07f, 0.1f, 1.0f };
    g.context->OMSetRenderTargets(1, g.rtv.GetAddressOf(), g.dsv.Get());
    g.context->ClearRenderTargetView(g.rtv.Get(), clear);
    g.context->ClearDepthStencilView(g.dsv.Get(), D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL, 1.0f, 0);
    g.context->OMSetDepthStencilState(g.dss.Get(), 0);

    UINT stride = sizeof(float)*6;
    UINT offset = 0;
    g.context->IASetInputLayout(g.inputLayout.Get());
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->IASetVertexBuffers(0, 1, g.vbo.GetAddressOf(), &stride, &offset);
    g.context->IASetIndexBuffer(g.ibo.Get(), DXGI_FORMAT_R16_UINT, 0);
    g.context->VSSetShader(g.vs.Get(), nullptr, 0);
    g.context->PSSetShader(g.ps.Get(), nullptr, 0);
    g.context->VSSetConstantBuffers(0, 1, g.cbuf.GetAddressOf());
    g.context->DrawIndexed(36, 0, 0);

    g.swapchain->Present(1, 0);
}

void Resize(UINT w, UINT h)
{
    if (!g.swapchain) return;
    g.width = w; g.height = h;
    g.context->OMSetRenderTargets(0, nullptr, nullptr);
    DestroyBackbufferAndDepth();
    g.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    CreateBackbufferAndDepth(w, h);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (g.swapchain) {
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            if (w == 0 || h == 0) break;
            Resize(w, h);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
