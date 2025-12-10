#include "DX11Context.h"
#include <cassert>

CDX11Context& CDX11Context::Instance()
{
    static CDX11Context s;
    return s;
}

bool CDX11Context::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_hwnd  = hwnd;
    m_width = width;
    m_height = height;

    if (!createDeviceAndSwapchain(hwnd))
        return false;

    createBackbufferViews();
    return true;
}

void CDX11Context::Shutdown()
{
    // 解绑
    if (m_context) {
        ID3D11RenderTargetView* nullRTV = nullptr;
        m_context->OMSetRenderTargets(1, &nullRTV, nullptr);
    }

    destroyBackbufferViews();
    m_swapchain.Reset();
    m_context.Reset();
    m_device.Reset();

    m_hwnd = nullptr;
    m_width = m_height = 0;
}

bool CDX11Context::createDeviceAndSwapchain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT deviceFlags = 0;
#if defined(_DEBUG)
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
        levels, _countof(levels),
        D3D11_SDK_VERSION, sd.BufferCount ? &sd : nullptr,
        m_swapchain.GetAddressOf(),
        m_device.GetAddressOf(), &created,
        m_context.GetAddressOf());

    return SUCCEEDED(hr);
}

void CDX11Context::createBackbufferViews()
{
    // Backbuffer RTV
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuf;
    HRESULT hr = m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backbuf.GetAddressOf());
    assert(SUCCEEDED(hr));

    hr = m_device->CreateRenderTargetView(backbuf.Get(), nullptr, m_backbufferRTV.GetAddressOf());
    assert(SUCCEEDED(hr));

    // 深度/模板
    D3D11_TEXTURE2D_DESC dsd{};
    dsd.Width = m_width;
    dsd.Height = m_height;
    dsd.MipLevels = 1;
    dsd.ArraySize = 1;
    dsd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsd.SampleDesc.Count = 1;
    dsd.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = m_device->CreateTexture2D(&dsd, nullptr, m_depthTex.GetAddressOf());
    assert(SUCCEEDED(hr));

    hr = m_device->CreateDepthStencilView(m_depthTex.Get(), nullptr, m_dsv.GetAddressOf());
    assert(SUCCEEDED(hr));
}

void CDX11Context::destroyBackbufferViews()
{
    m_dsv.Reset();
    m_depthTex.Reset();
    m_backbufferRTV.Reset();
}

void CDX11Context::OnResize(UINT width, UINT height)
{
    if (!m_swapchain) return;

    m_width  = width;
    m_height = height;

    if (m_context) {
        ID3D11RenderTargetView* nullRTV = nullptr;
        m_context->OMSetRenderTargets(1, &nullRTV, nullptr);
    }

    destroyBackbufferViews();

    DXGI_SWAP_CHAIN_DESC desc{};
    m_swapchain->GetDesc(&desc);
    m_swapchain->ResizeBuffers(desc.BufferCount, width, height, desc.BufferDesc.Format, desc.Flags);

    createBackbufferViews();
}

void CDX11Context::BindRenderTargets(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv)
{
    m_context->OMSetRenderTargets(1, &rtv, dsv);
}

void CDX11Context::SetViewport(float x, float y, float w, float h)
{
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = x; vp.TopLeftY = y;
    vp.Width = w; vp.Height = h;
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    m_context->RSSetViewports(1, &vp);
}

void CDX11Context::ClearRTV(ID3D11RenderTargetView* rtv, const float color[4])
{
    m_context->ClearRenderTargetView(rtv, color);
}

void CDX11Context::ClearDSV(ID3D11DepthStencilView* dsv, float depth, UINT8 stencil)
{
    m_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, stencil);
}

void CDX11Context::Present(UINT sync, UINT flags)
{
    m_swapchain->Present(sync, flags);
}
