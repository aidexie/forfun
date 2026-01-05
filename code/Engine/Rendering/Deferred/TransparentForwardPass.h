#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include "Engine/Rendering/ShadowPass.h"
#include <DirectXMath.h>

class CCamera;
class CScene;
class CClusteredLightingPass;

// ============================================
// CTransparentForwardPass - Forward rendering for transparent objects
// ============================================
// Used in deferred pipeline to render alpha-blended objects.
// Transparent objects cannot be deferred (no fixed blending order),
// so they must use forward rendering with back-to-front sorting.
//
// Pipeline Integration:
// - Runs AFTER deferred lighting pass (HDR buffer contains lit opaques)
// - Uses depth buffer from G-Buffer pass (read-only, no write)
// - Blends transparent objects on top of lit scene
// ============================================
class CTransparentForwardPass
{
public:
    CTransparentForwardPass() = default;
    ~CTransparentForwardPass() = default;

    bool Initialize();
    void Shutdown();

    // Render transparent objects to HDR buffer
    // - hdrRT: HDR render target (already contains lit opaque scene)
    // - depthRT: Depth buffer from G-Buffer pass (read-only)
    void Render(
        const CCamera& camera,
        CScene& scene,
        RHI::ITexture* hdrRT,
        RHI::ITexture* depthRT,
        uint32_t width,
        uint32_t height,
        const CShadowPass::Output* shadowData,
        CClusteredLightingPass* clusteredLighting
    );

private:
    void createPipeline();

    // Shaders (reuse MainPass shaders)
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;

    // Pipeline state (alpha blending, depth read-only)
    RHI::PipelineStatePtr m_pso;

    // Constant buffers
    RHI::BufferPtr m_cbFrame;
    RHI::BufferPtr m_cbObject;

    // Samplers
    RHI::SamplerPtr m_linearSampler;
    RHI::SamplerPtr m_shadowSampler;
};
