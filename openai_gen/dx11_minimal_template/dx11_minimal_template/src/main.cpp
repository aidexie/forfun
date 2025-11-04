
// Minimal DX11 Triangle (Win32 + D3D11). No external assets; shaders are inlined.
// Build with CMake on Windows. Links: d3d11, dxgi, d3dcompiler.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string>
#include <array>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// Globals for simplicity (a tiny template)
struct DX11State {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    D3D_FEATURE_LEVEL              featureLevel{};

    // Pipeline objects
    ComPtr<ID3D11VertexShader>     vs;
    ComPtr<ID3D11PixelShader>      ps;
    ComPtr<ID3D11InputLayout>      inputLayout;
    ComPtr<ID3D11Buffer>           vbo;

    UINT width  = 1280;
    UINT height = 720;
} g;

void CreateDeviceAndSwapchain(HWND hwnd);
void CreateRTV();
void DestroyRTV();
void CreateTrianglePipeline();
void Resize(UINT w, UINT h);
void Render();

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    // Register window class
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DX11TriangleClass";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // Create window
    RECT rc{0, 0, (LONG)g.width, (LONG)g.height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"DX11 Minimal Triangle",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);

    CreateDeviceAndSwapchain(hwnd);
    CreateRTV();
    CreateTrianglePipeline();

    // Main loop
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
    sd.BufferCount = 2; // double buffering
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // widely supported for DX11

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

void CreateRTV()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = g.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backBuffer.GetAddressOf());
    if (FAILED(hr)) {
        MessageBoxA(nullptr, "GetBuffer failed", "Error", MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }
    hr = g.device->CreateRenderTargetView(backBuffer.Get(), nullptr, g.rtv.GetAddressOf());
    if (FAILED(hr)) {
        MessageBoxA(nullptr, "CreateRenderTargetView failed", "Error", MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }
}

void DestroyRTV()
{
    g.rtv.Reset();
}

void CreateTrianglePipeline()
{
    // Simple colored triangle shaders (HLSL 5.0) as inlined strings
    static const char* kVS = R"(
        struct VSIn {
            float3 pos : POSITION;
            float3 col : COLOR;
        };
        struct VSOut {
            float4 pos : SV_Position;
            float3 col : COLOR;
        };
        VSOut main(VSIn i) {
            VSOut o;
            o.pos = float4(i.pos, 1.0);
            o.col = i.col;
            return o;
        }
    )";

    static const char* kPS = R"(
        struct PSIn {
            float4 pos : SV_Position;
            float3 col : COLOR;
        };
        float4 main(PSIn i) : SV_Target {
            return float4(i.col, 1.0);
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob))) {
        MessageBoxA(nullptr, errBlob ? (char*)errBlob->GetBufferPointer() : "VS compile error", "HLSL Error", MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &errBlob))) {
        MessageBoxA(nullptr, errBlob ? (char*)errBlob->GetBufferPointer() : "PS compile error", "HLSL Error", MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }

    // Create shaders
    g.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, g.vs.GetAddressOf());
    g.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, g.ps.GetAddressOf());

    // Input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                           D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float)*3,             D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g.device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), g.inputLayout.GetAddressOf());

    // Triangle vertex buffer (clip-space positions)
    struct Vertex { float x,y,z; float r,g,b; };
    std::array<Vertex, 3> verts{{
        {  0.0f,  0.5f, 0.0f, 1.f, 0.f, 0.f },
        {  0.5f, -0.5f, 0.0f, 0.f, 1.f, 0.f },
        { -0.5f, -0.5f, 0.0f, 0.f, 0.f, 1.f },
    }};

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(sizeof(verts));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = verts.data();

    g.device->CreateBuffer(&bd, &init, g.vbo.GetAddressOf());
}

void Resize(UINT w, UINT h)
{
    if (!g.swapchain) return;
    g.width = w; g.height = h;
    DestroyRTV();
    g.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    CreateRTV();
}

void Render()
{
    FLOAT clear[4] = { 0.07f, 0.07f, 0.10f, 1.0f };
    g.context->OMSetRenderTargets(1, g.rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = static_cast<FLOAT>(g.width);
    vp.Height = static_cast<FLOAT>(g.height);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    g.context->RSSetViewports(1, &vp);

    g.context->ClearRenderTargetView(g.rtv.Get(), clear);

    UINT stride = sizeof(float)*6;
    UINT offset = 0;
    g.context->IASetInputLayout(g.inputLayout.Get());
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->IASetVertexBuffers(0, 1, g.vbo.GetAddressOf(), &stride, &offset);
    g.context->VSSetShader(g.vs.Get(), nullptr, 0);
    g.context->PSSetShader(g.ps.Get(), nullptr, 0);
    g.context->Draw(3, 0);

    g.swapchain->Present(1, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (g.swapchain) {
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            if (w == 0 || h == 0) break; // minimized
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
