#pragma once
#include "GBuffer.h"
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <DirectXMath.h>

// Forward declarations
class CScene;
class CCamera;

// ============================================
// CGBufferPass - G-Buffer Geometry Pass
// ============================================
// Renders all opaque geometry to the G-Buffer.
// Uses EQUAL depth test (depth pre-pass must be run first).
//
// Input:
//   - Pre-populated depth buffer from DepthPrePass
//   - Scene geometry with materials
//
// Output:
//   - RT0: WorldPosition.xyz + Metallic
//   - RT1: Normal.xyz + Roughness
//   - RT2: Albedo.rgb + AO
//   - RT3: Emissive.rgb + MaterialID
//   - RT4: Velocity.xy
//
// Depth Test: EQUAL (matches depth pre-pass values)
// Depth Write: OFF (depth already written by pre-pass)
// ============================================
class CGBufferPass
{
public:
    CGBufferPass() = default;
    ~CGBufferPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Render geometry to G-Buffer
    // Requires depth buffer to be pre-populated by DepthPrePass
    void Render(
        const CCamera& camera,
        CScene& scene,
        CGBuffer& gbuffer,
        const DirectX::XMMATRIX& viewProjPrev,  // Previous frame's VP matrix
        uint32_t width,
        uint32_t height
    );

private:
    // Shaders
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;

    // Pipeline state (EQUAL depth test, 5 MRT)
    RHI::PipelineStatePtr m_pso;

    // Sampler
    RHI::SamplerPtr m_sampler;
};
