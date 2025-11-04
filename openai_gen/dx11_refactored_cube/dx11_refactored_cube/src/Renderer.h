
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>

class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Render();
    void Shutdown();

    bool IsInitialized() const { return m_swapchain != nullptr; }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

private:
    void createBackbufferAndDepth(UINT w, UINT h);
    void destroyBackbufferAndDepth();
    void createPipeline();

private:
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         m_swapchain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dss;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>     m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>      m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_vbo;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_ibo;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cbuf;

    D3D_FEATURE_LEVEL m_featureLevel{};
    UINT m_width = 0;
    UINT m_height = 0;
};
