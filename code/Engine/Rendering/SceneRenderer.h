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
class CReflectionProbeManager;

namespace RHI {
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

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
    // Descriptor Set PSO Creation
    // ============================================
    // Create PSO with descriptor set layouts (call after PerFrame layout is available)
    void CreatePSOWithLayouts(RHI::IDescriptorSetLayout* perFrameLayout);

    // ============================================
    // 核心渲染接口 (Descriptor Set Path)
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
    // - perFrameSet: PerFrame descriptor set (IBL, shadows, clustered lighting)
    // - probeManager: Reflection probe manager (for per-object probe selection)
    void Render(
        const CCamera& camera,
        CScene& scene,
        RHI::ITexture* hdrRT,
        RHI::ITexture* depthRT,
        uint32_t w, uint32_t h,
        float dt,
        const CShadowPass::Output* shadowData,
        CClusteredLightingPass* clusteredLighting,
        RHI::IDescriptorSet* perFrameSet,
        const CReflectionProbeManager* probeManager
    );

private:
    // Pipeline creation
    void createPipeline();

    // Descriptor set initialization (DX12 only)
    void initDescriptorSets();

    // ============================================
    // Rendering Resources (RHI)
    // ============================================
    // Shaders (SM 5.0 legacy)
    std::unique_ptr<RHI::IShader> m_vs;
    std::unique_ptr<RHI::IShader> m_ps;

    // Shaders (SM 5.1 descriptor set path)
    std::unique_ptr<RHI::IShader> m_vs_ds;
    std::unique_ptr<RHI::IShader> m_ps_ds;

    // Pipeline states (legacy)
    std::unique_ptr<RHI::IPipelineState> m_psoOpaque;
    std::unique_ptr<RHI::IPipelineState> m_psoTransparent;

    // Pipeline states (descriptor set path)
    std::unique_ptr<RHI::IPipelineState> m_psoOpaque_ds;
    std::unique_ptr<RHI::IPipelineState> m_psoTransparent_ds;

    // Constant buffers (legacy)
    std::unique_ptr<RHI::IBuffer> m_cbFrame;
    std::unique_ptr<RHI::IBuffer> m_cbObj;

    // Samplers
    std::unique_ptr<RHI::ISampler> m_sampler;
    std::unique_ptr<RHI::ISampler> m_materialSampler;

    // ============================================
    // Descriptor Set Resources (DX12 only)
    // ============================================
    // Layouts
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSetLayout* m_perMaterialLayout = nullptr;
    RHI::IDescriptorSetLayout* m_perDrawLayout = nullptr;

    // Sets
    RHI::IDescriptorSet* m_perPassSet = nullptr;
    RHI::IDescriptorSet* m_perMaterialSet = nullptr;
    RHI::IDescriptorSet* m_perDrawSet = nullptr;
};
