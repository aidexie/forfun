#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <DirectXMath.h>

// Forward declarations
class CScene;
class CCamera;

namespace RHI {
    class ITexture;
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// ============================================
// CDepthPrePass - Depth-Only Pre-Pass
// ============================================
// Renders all opaque geometry with depth-only output (no pixel shader).
// This eliminates G-Buffer overdraw by pre-populating the depth buffer.
//
// Descriptor Set Model (DX12):
// - Set 1 (PerPass, space1): CB_DepthPrePass (viewProj)
// - Set 3 (PerDraw, space3): CB_PerDraw (World matrix only)
// Note: DepthPrePass doesn't need Set 0 (PerFrame) or Set 2 (PerMaterial) - depth-only
//
// Depth Test: LESS (standard depth test)
// Depth Write: ON
// Pixel Shader: None (null PS)
//
// The subsequent G-Buffer pass uses EQUAL depth test with depth write OFF,
// ensuring each pixel executes the expensive G-Buffer PS exactly once.
// ============================================
class CDepthPrePass
{
public:
    CDepthPrePass() = default;
    ~CDepthPrePass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Render depth-only pass for all opaque objects
    // depthTarget: The depth buffer to render to (typically GBuffer's depth)
    // width, height: Viewport dimensions
    void Render(
        const CCamera& camera,
        CScene& scene,
        RHI::ITexture* depthTarget,
        uint32_t width,
        uint32_t height
    );

    // Check if descriptor set mode is available (DX12 only)
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_pso_ds != nullptr; }

    // Create PSO with descriptor set layouts (called after PerFrame layout is available)
    void CreatePSOWithLayouts(RHI::IDescriptorSetLayout* perFrameLayout);

private:
    void initDescriptorSets();
    // Depth-only vertex shader (no PS)
    RHI::ShaderPtr m_depthVS;

    // Pipeline state (depth-only, no color output)
    RHI::PipelineStatePtr m_pso;

    // Constant buffers
    RHI::BufferPtr m_cbFrame;   // View/Projection matrices
    RHI::BufferPtr m_cbObject;  // World matrix per object

    // ============================================
    // Descriptor Set Resources (SM 5.1, DX12 only)
    // ============================================
    RHI::ShaderPtr m_depthVS_ds;
    RHI::PipelineStatePtr m_pso_ds;

    // Descriptor set layouts
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSetLayout* m_perDrawLayout = nullptr;

    // Descriptor sets
    RHI::IDescriptorSet* m_perPassSet = nullptr;
    RHI::IDescriptorSet* m_perDrawSet = nullptr;
};
