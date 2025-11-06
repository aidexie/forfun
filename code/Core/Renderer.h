#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include "Mesh.h"

// 仅负责渲染：不创建窗口/设备/交换链，不 Present
class Renderer
{
public:
    struct OffscreenTarget {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>         color;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>         depth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>  dsv;
        UINT w = 0, h = 0;

        void Reset() {
            color.Reset(); rtv.Reset(); srv.Reset();
            depth.Reset(); dsv.Reset();
            w = h = 0;
        }
    };

    Renderer() = default;
    ~Renderer() = default;

    // 由外部提供设备与上下文；这里不拥有生命周期
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height);
    void Shutdown();

    // 视口尺寸变换（不涉及 ResizeBuffers；仅记录并用于投影矩阵/视口）
    void SetSize(UINT width, UINT height);

    // 可选的简单相机交互（保留你原来的右键观察/鼠标位移）
    void OnRButton(bool down);
    void OnMouseDelta(int dx, int dy);

    // 每帧渲染：外部传入已绑定的 RTV/DSV（或传入后由本函数绑定均可）
    // 这里选择“由本函数绑定”，以确保管线完整性
    void Render(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, float dt);
    ID3D11ShaderResourceView* GetOffscreenSRV() const { return m_off.srv.Get(); }
    UINT GetOffscreenWidth()  const { return m_off.w; }
    UINT GetOffscreenHeight() const { return m_off.h; }
    void RenderToOffscreen(UINT w, UINT h, float dt);
    // --- Extended API for editor/World integration ---
    std::size_t AddMesh(const MeshCPU_PNT& cpu, DirectX::XMMATRIX world);
    std::size_t AddMesh(const struct GltfMeshCPU& gltfMesh, DirectX::XMMATRIX world);
    void SetMeshWorld(std::size_t index, DirectX::XMMATRIX world);
private:
    void ensureOffscreen(UINT w, UINT h);
private:
    OffscreenTarget m_off;
    // === 设备引用（不拥有） ===
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    UINT m_width = 0, m_height = 0;

    // === 资源与管线 ===
    //struct VertexPNT { DirectX::XMFLOAT3 p, n; DirectX::XMFLOAT2 uv; DirectX::XMFLOAT4 t; };
    //struct MeshCPU_PNT { std::vector<VertexPNT> vertices; std::vector<uint32_t> indices; };

    struct GpuMesh {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vbo;
        Microsoft::WRL::ComPtr<ID3D11Buffer> ibo;
        UINT indexCount = 0;
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> albedoSRV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSRV;
    };
    std::vector<GpuMesh> m_meshes;

    // shaders / input layout / CB / sampler / raster states
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbObj;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsWire;

    // 默认贴图（兜底）
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_albedoSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_normalSRV;

    // camera
    DirectX::XMFLOAT3 m_camPos{ -6.0f, 0.8f, 0.0f };
    float m_yaw = 0.0f, m_pitch = 0.0f;
    bool  m_rmbLook = false;

private:
    // 内部工具
    void createPipeline();
    void createRasterStates();
    void ResetCameraLookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);
    void updateInput(float dt);

    // 资源上传/加载（与你原工程一致的最小保留）
    GpuMesh upload(const MeshCPU_PNT& m);
    bool    tryLoadOBJ(const std::string& path, bool flipZ, bool flipWinding, float targetDiag, DirectX::XMMATRIX world);

    // 外部加载函数（仍沿用你工程里的实现）
    friend bool LoadGLTF_PNT(const char* path, std::vector<struct GltfMeshCPU>& out,
                             bool flipZ_to_LH, bool flipWinding);
    friend void RecenterAndScale(MeshCPU_PNT& m, float targetDiag);

    // 与 glTF 贴图加载配套的 WIC 加载器（你工程里已有）
    friend bool LoadTextureWIC(ID3D11Device* dev, const std::wstring& path,
                               Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV,
                               bool srgb);
};
