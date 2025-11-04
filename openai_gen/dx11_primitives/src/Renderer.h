
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include "Mesh.h"

class Renderer {
public:
    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Render();
    void Shutdown();
    bool IsInitialized() const { return m_swapchain != nullptr; }

private:
    struct GpuMesh {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vbo;
        Microsoft::WRL::ComPtr<ID3D11Buffer> ibo;
        UINT stride = sizeof(VertexPC);
        UINT indexCount = 0;
    };
    void createBackbufferAndDepth(UINT w, UINT h);
    void destroyBackbufferAndDepth();
    void createPipeline();
    GpuMesh upload(const MeshCPU&);

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
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cbuf;

    std::vector<GpuMesh> m_meshes;

    D3D_FEATURE_LEVEL m_featureLevel{};
    UINT m_width = 0;
    UINT m_height = 0;
};
