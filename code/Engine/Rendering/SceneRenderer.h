#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "ClusteredLightingPass.h"
#include "ShadowPass.h"
#include "RHI/RHIResources.h"

// Forward declarations
class CScene;
class CCamera;

// ============================================
// CSceneRenderer - 核心场景渲染器
// ============================================
// 职责：渲染游戏场景内容（Opaque + Transparent + Skybox）
//
// 特点：
// - 纯粹的场景渲染，不包含编辑器工具（Grid, Debug Lines）
// - 不包含后处理（Tone Mapping, Gamma Correction）
// - 可被多处复用：编辑器视口、Reflection Probe、截图等
// - 输出 HDR 格式（R16G16B16A16_FLOAT）
//
// 使用场景：
// - 编辑器视口渲染（通过 CEditorRenderer）
// - Reflection Probe baking
// - 截图/录像
// ============================================
class CSceneRenderer {
public:
    CSceneRenderer() = default;
    ~CSceneRenderer() = default;

    // 初始化渲染器（创建 shader、pipeline state 等）
    bool Initialize();
    void Shutdown();

    // ============================================
    // 核心渲染接口
    // ============================================
    // 渲染场景到指定的 HDR RenderTarget
    //
    // 参数：
    // - camera: 渲染使用的相机
    // - scene: 要渲染的场景
    // - hdrRT: HDR render target texture (via RHI)
    // - depthRT: Depth stencil texture (via RHI)
    // - w, h: 渲染分辨率
    // - dt: Delta time
    // - shadowData: 阴影数据（可选，nullptr = 无阴影）
    void Render(
        const CCamera& camera,
        CScene& scene,
        RHI::ITexture* hdrRT,
        RHI::ITexture* depthRT,
        UINT w, UINT h,
        float dt,
        const CShadowPass::Output* shadowData
    );

    // 访问内部 Pass（用于编辑器调试）
    CClusteredLightingPass& GetClusteredLightingPass() { return m_clusteredLighting; }

private:
    // Internal rendering stages
    void renderOpaqueAndTransparent(
        const CCamera& camera,
        CScene& scene,
        float dt,
        const CShadowPass::Output* shadowData
    );

    // Pipeline creation
    void createPipeline();
    void createRasterStates();

    // ============================================
    // Rendering Resources
    // ============================================
    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;

    // Constant buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbObj;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbClusteredParams;

    // Samplers
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    // Rasterizer states
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsSolid;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rsWire;

    // Depth stencil states
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStateDefault;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStateTransparent;

    // Blend states
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendStateTransparent;

    // Clustered lighting
    CClusteredLightingPass m_clusteredLighting;
};
