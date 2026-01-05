#pragma once
#include "GBuffer.h"
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <DirectXMath.h>

// Forward declarations
class CScene;
class CCamera;
class CShadowPass;
class CClusteredLightingPass;

namespace RHI {
    class ITexture;
    class ISampler;
}

// ============================================
// CDeferredLightingPass - Full-Screen Deferred Lighting
// ============================================
// Evaluates lighting for all visible pixels using G-Buffer data.
// Runs as a full-screen pass after G-Buffer is populated.
//
// Features:
// - Directional light with CSM shadows
// - Point lights (via clustered light grid)
// - Spot lights (via clustered light grid)
// - IBL: Diffuse irradiance + specular pre-filtered environment
// - Volumetric Lightmap support
// - 2D Lightmap support
//
// Input:
//   - G-Buffer (5 RTs + Depth)
//   - Shadow maps (CSM Texture2DArray)
//   - IBL textures (Irradiance, Pre-filtered, BRDF LUT)
//   - Clustered light data (from ClusteredLightingPass)
//
// Output:
//   - HDR color buffer (R16G16B16A16_FLOAT)
// ============================================
class CDeferredLightingPass
{
public:
    CDeferredLightingPass() = default;
    ~CDeferredLightingPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Perform deferred lighting and output to HDR target
    void Render(
        const CCamera& camera,
        CScene& scene,
        CGBuffer& gbuffer,
        RHI::ITexture* hdrOutput,
        uint32_t width,
        uint32_t height,
        const CShadowPass* shadowPass,
        CClusteredLightingPass* clusteredLighting,
        RHI::ITexture* ssaoTexture = nullptr  // Optional SSAO texture (t18)
    );

private:
    // Shaders
    RHI::ShaderPtr m_vs;  // Full-screen triangle VS
    RHI::ShaderPtr m_ps;  // Deferred lighting PS

    // Pipeline state
    RHI::PipelineStatePtr m_pso;

    // Samplers
    RHI::SamplerPtr m_linearSampler;
    RHI::SamplerPtr m_shadowSampler;  // Comparison sampler for PCF
    RHI::SamplerPtr m_pointSampler;   // Point sampler for G-Buffer
};
