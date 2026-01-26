#pragma once
#include "GBuffer.h"
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include "RHI/CB_PerFrame.h"
#include <DirectXMath.h>

// Forward declarations
class CScene;
class CCamera;
class CShadowPass;
class CClusteredLightingPass;

namespace RHI {
    class ITexture;
    class ISampler;
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// ============================================
// CDeferredLightingPass - Full-Screen Deferred Lighting
// ============================================
// Evaluates lighting for all visible pixels using G-Buffer data.
// Runs as a full-screen pass after G-Buffer is populated.
//
// Descriptor Set Model:
// - Set 0 (PerFrame, space0): Received from RenderPipeline - global resources
// - Set 1 (PerPass, space1): Owned by this pass - G-Buffer + SSAO
//
// Features:
// - Directional light with CSM shadows
// - Point lights (via clustered light grid)
// - Spot lights (via clustered light grid)
// - IBL: Diffuse irradiance + specular pre-filtered environment
// - Volumetric Lightmap support
//
// Input:
//   - G-Buffer (5 RTs + Depth)
//   - PerFrame descriptor set (shadow maps, IBL, clustered data)
//
// Output:
//   - HDR color buffer (R16G16B16A16_FLOAT)
// ============================================
class CDeferredLightingPass
{
public:
    CDeferredLightingPass() = default;
    ~CDeferredLightingPass();

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Resource Management
    // ============================================
    // Call when G-Buffer is resized to rebind textures
    void OnResize(CGBuffer& gbuffer);

    // ============================================
    // Rendering (Descriptor Set API)
    // ============================================
    // Perform deferred lighting using descriptor sets
    void Render(
        const CCamera& camera,
        CScene& scene,
        CGBuffer& gbuffer,
        RHI::ITexture* hdrOutput,
        uint32_t width,
        uint32_t height,
        const CShadowPass* shadowPass,
        RHI::IDescriptorSet* perFrameSet,
        RHI::ITexture* ssaoTexture = nullptr
    );

    // ============================================
    // Legacy Rendering (for backwards compatibility during migration)
    // ============================================
    void Render(
        const CCamera& camera,
        CScene& scene,
        CGBuffer& gbuffer,
        RHI::ITexture* hdrOutput,
        uint32_t width,
        uint32_t height,
        const CShadowPass* shadowPass,
        CClusteredLightingPass* clusteredLighting,
        RHI::ITexture* ssaoTexture = nullptr
    );

    // Check if descriptor set mode is available (DX12 only)
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_pso_ds != nullptr; }

    // Get PerPass layout for pipeline creation
    RHI::IDescriptorSetLayout* GetPerPassLayout() const { return m_perPassLayout; }

    // Create PSO with descriptor set layouts (called after PerFrame layout is available)
    void CreatePSOWithLayouts(RHI::IDescriptorSetLayout* perFrameLayout);

private:
    void initDescriptorSets();
    void initLegacy();

    // Shaders
    RHI::ShaderPtr m_vs;       // Full-screen triangle VS
    RHI::ShaderPtr m_ps;       // Deferred lighting PS (legacy SM 5.0)
    RHI::ShaderPtr m_ps_ds;    // Deferred lighting PS (descriptor set, SM 5.1)

    // Pipeline states
    RHI::PipelineStatePtr m_pso;       // Legacy PSO
    RHI::PipelineStatePtr m_pso_ds;    // Descriptor set PSO

    // Samplers (used in both modes)
    RHI::SamplerPtr m_linearSampler;
    RHI::SamplerPtr m_shadowSampler;
    RHI::SamplerPtr m_pointSampler;

    // ============================================
    // Descriptor Set Resources (DX12 only)
    // ============================================
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    // Cached G-Buffer textures for rebinding on resize
    RHI::ITexture* m_cachedGBuffer[6] = {};  // AlbedoAO, NormalRoughness, WorldPosMetallic, Emissive, Velocity, Depth
};
