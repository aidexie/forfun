#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "ShadowPass.h"
#include "PostProcessPass.h"
#include "DebugLinePass.h"
#include "GridPass.h"
#include "ClusteredLightingPass.h"
// Forward declarations
class CScene;
class CShadowPass;

// CMainPass: 主渲染流程
// 在 Engine 层，可以直接访问 Scene/GameObject/Components
// 使用 DX11Context::Instance() 访问 D3D11 设备和上下文
class CMainPass
{
public:
    struct OffscreenTarget {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>         color;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>         depth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>  dsv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV;  // For reading depth in shaders
        UINT w = 0, h = 0;

        void Reset() {
            color.Reset(); rtv.Reset(); srv.Reset();
            depth.Reset(); dsv.Reset(); depthSRV.Reset();
            w = h = 0;
        }
    };

    CMainPass() = default;
    ~CMainPass() = default;

    // 初始化渲染管线资源
    bool Initialize();
    void Shutdown();

    // 相机交互
    void OnRButton(bool down);
    void OnMouseDelta(int dx, int dy);

    // 更新摄像机矩阵（在 ShadowPass 之前调用）
    void UpdateCamera(UINT viewportWidth, UINT viewportHeight, float dt);

    // 渲染场景到离屏目标
    // shadowData: shadow resources from CShadowPass (nullptr = no shadows)
    void Render(CScene& scene, UINT w, UINT h, float dt,
                const CShadowPass::Output* shadowData);

    // 访问离屏目标（返回最终的 LDR sRGB 纹理用于显示）
    ID3D11ShaderResourceView* GetOffscreenSRV() const { return m_offLDR.srv.Get(); }
    ID3D11Texture2D* GetOffscreenTexture() const { return m_offLDR.color.Get(); }
    UINT GetOffscreenWidth()  const { return m_offLDR.w; }
    UINT GetOffscreenHeight() const { return m_offLDR.h; }

    // 获取摄像机矩阵（用于 ShadowPass 视锥拟合）
    DirectX::XMMATRIX GetCameraViewMatrix() const { return m_cameraView; }
    DirectX::XMMATRIX GetCameraProjMatrix() const { return m_cameraProj; }

    // 获取 DebugLinePass（用于外部添加调试线条）
    CDebugLinePass& GetDebugLinePass() { return m_debugLinePass; }

    // 获取 ClusteredLightingPass（用于debug控制）
    CClusteredLightingPass& GetClusteredLightingPass() { return m_clusteredLighting; }

private:
    void ensureOffscreen(UINT w, UINT h);
    void renderScene(CScene& scene, float dt, const CShadowPass::Output* shadowData);

private:
    OffscreenTarget m_off;     // HDR linear space (R16G16B16A16_FLOAT)
    OffscreenTarget m_offLDR;  // LDR sRGB space (R8G8B8A8_UNORM_SRGB) for display

    // === 渲染管线资源 ===
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbObj;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbClusteredParams;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsWire;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStateDefault;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStateTransparent;  // Depth read-only for transparent
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendStateTransparent;          // Alpha blending

    // 相机状态
    DirectX::XMFLOAT3 m_camPos{ -6.0f, 0.8f, 0.0f };
    float m_yaw = 0.0f, m_pitch = 0.0f;
    bool  m_rmbLook = false;

    // 摄像机矩阵缓存（用于 ShadowPass）
    DirectX::XMMATRIX m_cameraView = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_cameraProj = DirectX::XMMatrixIdentity();

    // Skybox is now managed by Scene singleton
    // Post-process (tone mapping + gamma correction)
    CPostProcessPass m_postProcess;

    // Debug line rendering
    CClusteredLightingPass m_clusteredLighting;
    CDebugLinePass m_debugLinePass;

private:
    // 内部工具
    void createPipeline();
    void createRasterStates();
    void ResetCameraLookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);
    void updateInput(float dt);
};
