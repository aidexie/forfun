#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

class CDX11Context
{
public:
    // ---- Singleton ----
    static CDX11Context& Instance();

    // 不负责创建窗口；由外部传入 hwnd 和初始宽高
    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Shutdown();
    void OnResize(UINT width, UINT height);

    // ---- Frame helpers (纯DX) ----
    void BindRenderTargets(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv);
    void SetViewport(float x, float y, float w, float h);
    void ClearRTV(ID3D11RenderTargetView* rtv, const float color[4]);
    void ClearDSV(ID3D11DepthStencilView* dsv, float depth, UINT8 stencil);
    void Present(UINT sync, UINT flags);

    // ---- Getters ----
    ID3D11Device*           GetDevice()         const { return m_device.Get(); }
    ID3D11DeviceContext*    GetContext()        const { return m_context.Get(); }
    IDXGISwapChain*         GetSwapChain()      const { return m_swapchain.Get(); }
    ID3D11RenderTargetView* GetBackbufferRTV()  const { return m_backbufferRTV.Get(); }
    ID3D11DepthStencilView* GetDSV()            const { return m_dsv.Get(); }
    UINT GetWidth()  const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    CDX11Context() = default;
    ~CDX11Context() = default;
    CDX11Context(const CDX11Context&) = delete;
    CDX11Context& operator=(const CDX11Context&) = delete;

    bool createDeviceAndSwapchain(HWND hwnd);
    void createBackbufferViews();
    void destroyBackbufferViews();

private:
    // 外部窗口句柄（不管理窗口生命周期）
    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // D3D11
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         m_swapchain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backbufferRTV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
};
