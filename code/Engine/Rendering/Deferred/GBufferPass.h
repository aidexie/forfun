#pragma once
#include "GBuffer.h"
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <DirectXMath.h>

// Forward declarations
class CScene;
class CCamera;

namespace RHI {
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// ============================================
// CGBufferPass - G-Buffer Geometry Pass
// ============================================
// Renders all opaque geometry to the G-Buffer.
// Uses EQUAL depth test (depth pre-pass must be run first).
//
// Descriptor Set Model (DX12):
// - Set 0 (PerFrame, space0): Received from RenderPipeline - global resources
// - Set 1 (PerPass, space1): Owned by this pass - CB_GBufferFrame, Lightmap
// - Set 2 (PerMaterial, space2): Owned by this pass - material textures, CB_Material
// - Set 3 (PerDraw, space3): Owned by this pass - CB_PerDraw (per-object data)
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
    ~CGBufferPass();

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering (Descriptor Set API - DX12)
    // ============================================
    void Render(
        const CCamera& camera,
        CScene& scene,
        CGBuffer& gbuffer,
        const DirectX::XMMATRIX& viewProjPrev,
        uint32_t width,
        uint32_t height,
        RHI::IDescriptorSet* perFrameSet
    );

    // Check if descriptor set mode is available (DX12 only)
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_pso_ds != nullptr; }

    // Get PerPass layout for pipeline creation
    RHI::IDescriptorSetLayout* GetPerPassLayout() const { return m_perPassLayout; }

    // Create PSO with descriptor set layouts (called after PerFrame layout is available)
    void CreatePSOWithLayouts(RHI::IDescriptorSetLayout* perFrameLayout);

private:
    void initDescriptorSets();

    // ============================================
    // Legacy Resources (SM 5.0)
    // ============================================
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;
    RHI::PipelineStatePtr m_pso;
    RHI::SamplerPtr m_sampler;

    // ============================================
    // Descriptor Set Resources (SM 5.1, DX12 only)
    // ============================================
    RHI::ShaderPtr m_vs_ds;
    RHI::ShaderPtr m_ps_ds;
    RHI::PipelineStatePtr m_pso_ds;

    // Descriptor set layouts
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSetLayout* m_perMaterialLayout = nullptr;
    RHI::IDescriptorSetLayout* m_perDrawLayout = nullptr;

    // Descriptor sets
    RHI::IDescriptorSet* m_perPassSet = nullptr;
    RHI::IDescriptorSet* m_perMaterialSet = nullptr;
    RHI::IDescriptorSet* m_perDrawSet = nullptr;

    // Samplers for descriptor set path
    RHI::SamplerPtr m_lightmapSampler;
    RHI::SamplerPtr m_materialSampler;
};
