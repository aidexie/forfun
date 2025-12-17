#pragma once
#include <DirectXMath.h>
#include <memory>
#include <cstdint>
#include "ShadowPass.h"
#include "RHI/RHIResources.h"

// Forward declarations
class CScene;
class CCamera;
class CClusteredLightingPass;

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
    // - clusteredLighting: 聚类光照 Pass（用于绑定光照数据）
    void Render(
        const CCamera& camera,
        CScene& scene,
        RHI::ITexture* hdrRT,
        RHI::ITexture* depthRT,
        uint32_t w, uint32_t h,
        float dt,
        const CShadowPass::Output* shadowData,
        CClusteredLightingPass* clusteredLighting
    );

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

    // ============================================
    // Rendering Resources (RHI)
    // ============================================
    // Shaders
    std::unique_ptr<RHI::IShader> m_vs;
    std::unique_ptr<RHI::IShader> m_ps;

    // Pipeline states
    std::unique_ptr<RHI::IPipelineState> m_psoOpaque;
    std::unique_ptr<RHI::IPipelineState> m_psoTransparent;

    // Constant buffers
    std::unique_ptr<RHI::IBuffer> m_cbFrame;
    std::unique_ptr<RHI::IBuffer> m_cbObj;

    // Samplers
    std::unique_ptr<RHI::ISampler> m_sampler;
};
