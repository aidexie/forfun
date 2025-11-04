
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include "Mesh.h"

class Renderer {
public:
    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Render();
    void Shutdown();
    bool IsInitialized() const { return m_swapchain != nullptr; }

    void OnMouseDelta(int dx, int dy);
    void OnRButton(bool down);

    void ResetCameraLookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);

private:
    struct GpuMesh {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vbo;
        Microsoft::WRL::ComPtr<ID3D11Buffer> ibo;
        UINT stride = sizeof(VertexPNT);
        UINT indexCount = 0;
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
    };
    void createBackbufferAndDepth(UINT w, UINT h);
    void destroyBackbufferAndDepth();
    void createPipeline();
    void createRasterStates();
    GpuMesh upload(const MeshCPU_PNT&);
    void updateInput(float dt);
    bool tryLoadOBJ(const std::string& path, bool flipZ, bool flipWinding, float targetDiag, DirectX::XMMATRIX world);

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
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cbFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cbObj;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>     m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_rsSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_rsWire;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_albedoSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_normalSRV;

    std::vector<GpuMesh> m_meshes;

    DirectX::XMFLOAT3 m_camPos{ -6.0f, 0.8f, 0.0f };
    float m_yaw = 0.0f;
    float m_pitch = -0.1f;
    bool  m_rmbLook = false;

    D3D_FEATURE_LEVEL m_featureLevel{};
    UINT m_width = 0;
    UINT m_height = 0;
};
