#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <DirectXMath.h>

// Forward declarations
class CScene;
class CCamera;

namespace RHI {
    class ITexture;
}

// ============================================
// CDepthPrePass - Depth-Only Pre-Pass
// ============================================
// Renders all opaque geometry with depth-only output (no pixel shader).
// This eliminates G-Buffer overdraw by pre-populating the depth buffer.
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

private:
    // Depth-only vertex shader (no PS)
    RHI::ShaderPtr m_depthVS;

    // Pipeline state (depth-only, no color output)
    RHI::PipelineStatePtr m_pso;

    // Constant buffers
    RHI::BufferPtr m_cbFrame;   // View/Projection matrices
    RHI::BufferPtr m_cbObject;  // World matrix per object
};
