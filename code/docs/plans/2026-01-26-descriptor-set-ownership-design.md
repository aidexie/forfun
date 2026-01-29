# Descriptor Set Ownership Design

**Date**: 2026-01-26
**Status**: Approved
**Related**: `descriptor-set-abstraction.md`, `descriptor-set-architecture-issues.md`

---

## Summary

This document defines the **4-set ownership model** for descriptor set organization, solving the architecture issues identified in `descriptor-set-architecture-issues.md`.

### Key Decisions

| Decision | Choice |
|----------|--------|
| Set count | **4 sets** (PerFrame, PerPass, PerMaterial, PerDraw) |
| PerFrame ownership | **RenderContext** owns, subsystems populate via `PopulatePerFrameSet()` |
| Static + Dynamic | **Mixed in same set**, VolatileCBV handles dynamic updates |
| Slot assignment | **Reserved slot ranges** defined in header files |
| PerPass layout | **Per-pass layouts** (each pass has tailored layout) |
| PerMaterial layout | **One shared layout** (PBR material) |
| PerDraw binding | **Push constants** (128 bytes, transient sets) |

---

## Set Overview

| Set | Name | Layout Strategy | Allocation | Update Frequency |
|-----|------|-----------------|------------|------------------|
| **0** | PerFrame | One shared layout | Persistent | Once per frame (VolatileCBV) |
| **1** | PerPass | Per-pass layouts | Persistent | Once per pass |
| **2** | PerMaterial | One shared layout | Persistent | On material change |
| **3** | PerDraw | One shared layout | Transient | Every draw call |

---

## Set 0: PerFrame (space0)

### Slot Layout

```cpp
// RHI/PerFrameSlots.h
#pragma once

namespace PerFrameSlots {

//==============================================
// Constant Buffers (b-registers, space0)
// Range: b0-b7 (8 slots)
//==============================================
namespace CB {
    constexpr uint32_t PerFrame        = 0;  // Camera, Time, global settings
    constexpr uint32_t Clustered       = 1;  // Cluster grid params
    constexpr uint32_t Volumetric      = 2;  // Lightmap params
    constexpr uint32_t ReflectionProbe = 3;  // Probe selection data
    // b4-b7: Reserved for future
}

//==============================================
// Textures (t-registers, space0)
// Range: t0-t31 (32 slots)
//==============================================
namespace Tex {
    // Global resources (t0-t3)
    constexpr uint32_t ShadowMapArray   = 0;
    constexpr uint32_t BrdfLUT          = 1;
    constexpr uint32_t IrradianceArray  = 2;
    constexpr uint32_t PrefilteredArray = 3;

    // ClusteredLighting (t4-t7)
    constexpr uint32_t Clustered_LightIndexList = 4;
    constexpr uint32_t Clustered_LightGrid      = 5;
    constexpr uint32_t Clustered_LightData      = 6;
    constexpr uint32_t Clustered_Reserved       = 7;

    // VolumetricLightmap (t8-t12)
    constexpr uint32_t Volumetric_SH_R     = 8;
    constexpr uint32_t Volumetric_SH_G     = 9;
    constexpr uint32_t Volumetric_SH_B     = 10;
    constexpr uint32_t Volumetric_Octree   = 11;
    constexpr uint32_t Volumetric_Reserved = 12;

    // ReflectionProbes (t13-t15)
    constexpr uint32_t ReflectionProbe_Array    = 13;
    constexpr uint32_t ReflectionProbe_Indices  = 14;
    constexpr uint32_t ReflectionProbe_Reserved = 15;

    // t16-t31: Reserved for future (DDGI, RTGI, etc.)
}

//==============================================
// Samplers (s-registers, space0)
// Range: s0-s7 (8 slots)
//==============================================
namespace Samp {
    constexpr uint32_t LinearClamp = 0;
    constexpr uint32_t LinearWrap  = 1;
    constexpr uint32_t PointClamp  = 2;
    constexpr uint32_t ShadowCmp   = 3;
    constexpr uint32_t Aniso       = 4;
    // s5-s7: Reserved for future
}

} // namespace PerFrameSlots
```

### Subsystem Integration

Subsystems populate the PerFrame set directly via `PopulatePerFrameSet()`:

```cpp
// Interface for subsystems
class IPerFrameContributor {
public:
    virtual void PopulatePerFrameSet(IDescriptorSet* perFrameSet) = 0;
};

// Example: ClusteredLightingPass
class CClusteredLightingPass : public IPerFrameContributor {
public:
    void PopulatePerFrameSet(IDescriptorSet* perFrameSet) override {
        using namespace PerFrameSlots;
        perFrameSet->Bind({
            BindingSetItem::ConstantBuffer(CB::Clustered, m_clusterCB),
            BindingSetItem::Buffer_SRV(Tex::Clustered_LightIndexList, m_lightIndexList),
            BindingSetItem::Buffer_SRV(Tex::Clustered_LightGrid, m_lightGrid),
            BindingSetItem::Buffer_SRV(Tex::Clustered_LightData, m_lightDataBuffer),
        });
    }
};

// RenderContext aggregates all subsystems
void CRenderContext::BuildPerFrameSet() {
    // Core bindings
    m_perFrameSet->Bind({
        BindingSetItem::Texture_SRV(PerFrameSlots::Tex::ShadowMapArray, m_shadowMaps),
        BindingSetItem::Texture_SRV(PerFrameSlots::Tex::BrdfLUT, m_brdfLUT),
        BindingSetItem::Texture_SRV(PerFrameSlots::Tex::IrradianceArray, m_irradiance),
        BindingSetItem::Texture_SRV(PerFrameSlots::Tex::PrefilteredArray, m_prefiltered),
        BindingSetItem::Sampler(PerFrameSlots::Samp::LinearClamp, m_linearClampSampler),
        BindingSetItem::Sampler(PerFrameSlots::Samp::ShadowCmp, m_shadowCmpSampler),
    });

    // Subsystems populate their bindings
    m_clusteredLighting->PopulatePerFrameSet(m_perFrameSet);
    m_volumetricLightmap->PopulatePerFrameSet(m_perFrameSet);
    m_reflectionProbes->PopulatePerFrameSet(m_perFrameSet);

    // Dynamic CB updated each frame
    m_perFrameSet->Bind(BindingSetItem::VolatileCBV(
        PerFrameSlots::CB::PerFrame, &m_cbPerFrame, sizeof(CB_PerFrame)));
}
```

---

## Set 1: PerPass (space1)

### Slot Layout

```cpp
// RHI/PerPassSlots.h
#pragma once

namespace PerPassSlots {

//==============================================
// Constant Buffers (b-registers, space1)
//==============================================
namespace CB {
    constexpr uint32_t PerPass = 0;
}

//==============================================
// Textures (t-registers, space1)
// Range: t0-t15 (16 slots)
//==============================================
namespace Tex {
    // G-Buffer (t0-t5)
    constexpr uint32_t GBuffer_Albedo   = 0;
    constexpr uint32_t GBuffer_Normal   = 1;
    constexpr uint32_t GBuffer_WorldPos = 2;
    constexpr uint32_t GBuffer_Emissive = 3;
    constexpr uint32_t GBuffer_Velocity = 4;
    constexpr uint32_t GBuffer_Depth    = 5;

    // Post-process inputs (t6-t11)
    constexpr uint32_t SceneColor = 6;
    constexpr uint32_t SSAO       = 7;
    constexpr uint32_t SSR        = 8;
    constexpr uint32_t Bloom      = 9;
    constexpr uint32_t PrevFrame  = 10;
    constexpr uint32_t DepthHiZ   = 11;

    // t12-t15: Reserved
}

//==============================================
// Samplers (s-registers, space1)
//==============================================
namespace Samp {
    constexpr uint32_t PointClamp  = 0;
    constexpr uint32_t LinearClamp = 1;
}

//==============================================
// UAVs (u-registers, space1)
//==============================================
namespace UAV {
    constexpr uint32_t Output0 = 0;
    constexpr uint32_t Output1 = 1;
}

} // namespace PerPassSlots
```

### Per-Pass Layout Strategy

Each render pass defines its own layout with only the slots it needs:

```cpp
// Engine/Rendering/PassLayouts.h
#pragma once

namespace PassLayouts {

inline IDescriptorSetLayout* CreateDeferredLightingLayout(IDescriptorSetAllocator* alloc) {
    using namespace PerPassSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("DeferredLighting")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, sizeof(CB_DeferredLighting)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Albedo))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Normal))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_WorldPos))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Emissive))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Depth))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SSAO))
            .AddItem(BindingLayoutItem::Sampler(Samp::PointClamp))
    );
}

inline IDescriptorSetLayout* CreateSSAOLayout(IDescriptorSetAllocator* alloc) {
    using namespace PerPassSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("SSAO")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, sizeof(CB_SSAO)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Normal))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Depth))
            .AddItem(BindingLayoutItem::Texture_UAV(UAV::Output0))
            .AddItem(BindingLayoutItem::Sampler(Samp::PointClamp))
    );
}

inline IDescriptorSetLayout* CreateSSRLayout(IDescriptorSetAllocator* alloc) {
    using namespace PerPassSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("SSR")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, sizeof(CB_SSR)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Normal))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Depth))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SceneColor))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::DepthHiZ))
            .AddItem(BindingLayoutItem::Texture_UAV(UAV::Output0))
            .AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp))
    );
}

inline IDescriptorSetLayout* CreateBloomLayout(IDescriptorSetAllocator* alloc) {
    using namespace PerPassSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("Bloom")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, sizeof(CB_Bloom)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SceneColor))
            .AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp))
    );
}

inline IDescriptorSetLayout* CreateTAALayout(IDescriptorSetAllocator* alloc) {
    using namespace PerPassSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("TAA")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, sizeof(CB_TAA)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::GBuffer_Velocity))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SceneColor))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::PrevFrame))
            .AddItem(BindingLayoutItem::Texture_UAV(UAV::Output0))
            .AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp))
    );
}

inline IDescriptorSetLayout* CreateFXAALayout(IDescriptorSetAllocator* alloc) {
    using namespace PerPassSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("FXAA")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, sizeof(CB_FXAA)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SceneColor))
            .AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp))
    );
}

} // namespace PassLayouts
```

### Pass Ownership Pattern

Each render pass owns its layout and set:

```cpp
class CSSAOPass {
private:
    IDescriptorSetLayout* m_layout = nullptr;
    IDescriptorSet* m_set = nullptr;
    IPipelineState* m_pso = nullptr;

public:
    void Initialize(IRenderContext* ctx) {
        auto* alloc = ctx->GetDescriptorSetAllocator();
        m_layout = PassLayouts::CreateSSAOLayout(alloc);
        m_set = alloc->AllocatePersistentSet(m_layout);

        SPipelineStateDesc psoDesc;
        psoDesc.setLayouts[0] = ctx->GetPerFrameLayout();
        psoDesc.setLayouts[1] = m_layout;
        psoDesc.computeShader = LoadShader("SSAO.cs.hlsl");
        m_pso = ctx->CreatePipelineState(psoDesc);
    }

    void Render(ICommandList* cmdList, ITexture* normalTex,
                ITexture* depthTex, ITexture* outputTex) {
        using namespace PerPassSlots;
        m_set->Bind({
            BindingSetItem::Texture_SRV(Tex::GBuffer_Normal, normalTex),
            BindingSetItem::Texture_SRV(Tex::GBuffer_Depth, depthTex),
            BindingSetItem::Texture_UAV(UAV::Output0, outputTex),
        });
        m_set->Bind(BindingSetItem::VolatileCBV(CB::PerPass, &m_cb, sizeof(CB_SSAO)));

        cmdList->SetPipelineState(m_pso);
        cmdList->BindDescriptorSet(1, m_set);
        cmdList->Dispatch(dispatchX, dispatchY, 1);
    }
};
```

---

## Set 2: PerMaterial (space2)

### Slot Layout

```cpp
// RHI/PerMaterialSlots.h
#pragma once

namespace PerMaterialSlots {

namespace CB {
    constexpr uint32_t Material = 0;
}

namespace Tex {
    constexpr uint32_t Albedo            = 0;
    constexpr uint32_t Normal            = 1;
    constexpr uint32_t MetallicRoughness = 2;
    constexpr uint32_t Emissive          = 3;
    constexpr uint32_t AO                = 4;
    constexpr uint32_t Height            = 5;  // Future: parallax
    constexpr uint32_t DetailNormal      = 6;  // Future: detail
    constexpr uint32_t Mask              = 7;  // Future: custom mask
}

namespace Samp {
    constexpr uint32_t Material = 0;
}

} // namespace PerMaterialSlots
```

### Shared PBR Material Layout

```cpp
// Engine/Rendering/MaterialLayout.h
namespace MaterialLayout {

inline IDescriptorSetLayout* CreatePBRMaterialLayout(IDescriptorSetAllocator* alloc) {
    using namespace PerMaterialSlots;
    return alloc->CreateLayout(
        BindingLayoutDesc("PBRMaterial")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::Material, sizeof(CB_Material)))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::Albedo))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::Normal))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::MetallicRoughness))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::Emissive))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::AO))
            .AddItem(BindingLayoutItem::Sampler(Samp::Material))
    );
}

} // namespace MaterialLayout
```

### Material Class

```cpp
class CMaterial {
private:
    IDescriptorSet* m_descriptorSet = nullptr;
    ITexture* m_albedoTex = nullptr;
    ITexture* m_normalTex = nullptr;
    ITexture* m_metallicRoughnessTex = nullptr;
    ITexture* m_emissiveTex = nullptr;
    ITexture* m_aoTex = nullptr;
    ISampler* m_sampler = nullptr;
    CB_Material m_properties;
    bool m_dirty = true;

public:
    void Initialize(IDescriptorSetAllocator* alloc, IDescriptorSetLayout* layout) {
        m_descriptorSet = alloc->AllocatePersistentSet(layout);
    }

    void Bind(ICommandList* cmdList) {
        using namespace PerMaterialSlots;

        if (m_dirty) {
            m_descriptorSet->Bind({
                BindingSetItem::Texture_SRV(Tex::Albedo, m_albedoTex),
                BindingSetItem::Texture_SRV(Tex::Normal, m_normalTex),
                BindingSetItem::Texture_SRV(Tex::MetallicRoughness, m_metallicRoughnessTex),
                BindingSetItem::Texture_SRV(Tex::Emissive, m_emissiveTex),
                BindingSetItem::Texture_SRV(Tex::AO, m_aoTex),
                BindingSetItem::Sampler(Samp::Material, m_sampler),
            });
            m_dirty = false;
        }

        m_descriptorSet->Bind(BindingSetItem::VolatileCBV(
            CB::Material, &m_properties, sizeof(CB_Material)));
        cmdList->BindDescriptorSet(2, m_descriptorSet);
    }
};
```

---

## Set 3: PerDraw (space3)

### Slot Layout

```cpp
// RHI/PerDrawSlots.h
#pragma once

namespace PerDrawSlots {

namespace Push {
    constexpr uint32_t PerDraw = 0;
}

// Must fit in 128 bytes (DX12 root constants limit)
struct CB_PerDraw {
    DirectX::XMFLOAT4X4 World;             // 64 bytes
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 64 bytes
    // Total: 128 bytes
};

} // namespace PerDrawSlots
```

### PerDraw Layout and Usage

```cpp
namespace PerDrawLayout {

inline IDescriptorSetLayout* CreatePerDrawLayout(IDescriptorSetAllocator* alloc) {
    return alloc->CreateLayout(
        BindingLayoutDesc("PerDraw")
            .AddItem(BindingLayoutItem::PushConstants(
                PerDrawSlots::Push::PerDraw, sizeof(PerDrawSlots::CB_PerDraw)))
    );
}

} // namespace PerDrawLayout

// Usage - transient sets
void SceneRenderer::RenderObject(ICommandList* cmdList, const CTransform& transform) {
    PerDrawSlots::CB_PerDraw perDraw;
    perDraw.World = transform.GetWorldMatrix();
    perDraw.WorldInvTranspose = transform.GetWorldInverseTranspose();

    auto* drawSet = m_allocator->AllocateTransientSet(m_perDrawLayout);
    drawSet->Bind(BindingSetItem::PushConstants(
        PerDrawSlots::Push::PerDraw, &perDraw, sizeof(perDraw)));

    cmdList->BindDescriptorSet(3, drawSet);
    cmdList->DrawIndexed(mesh->GetIndexCount());
}
```

---

## HLSL Shader Template

```hlsl
// Shader/Common.hlsli

//==============================================
// Set 0: PerFrame (space0)
//==============================================
cbuffer CB_PerFrame : register(b0, space0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float3 gCameraPos;
    float gTime;
};

cbuffer CB_Clustered : register(b1, space0) {
    // Cluster grid params
};

cbuffer CB_Volumetric : register(b2, space0) {
    // Volumetric lightmap params
};

// Global textures
Texture2DArray gShadowMaps       : register(t0, space0);
Texture2D gBrdfLUT               : register(t1, space0);
TextureCubeArray gIrradiance     : register(t2, space0);
TextureCubeArray gPrefiltered    : register(t3, space0);

// Clustered lighting (t4-t7)
StructuredBuffer<uint> gLightIndexList : register(t4, space0);
StructuredBuffer<uint2> gLightGrid     : register(t5, space0);
StructuredBuffer<LightData> gLightData : register(t6, space0);

// Volumetric lightmap (t8-t12)
Texture3D gVolumetric_SH_R       : register(t8, space0);
Texture3D gVolumetric_SH_G       : register(t9, space0);
Texture3D gVolumetric_SH_B       : register(t10, space0);
StructuredBuffer<OctreeNode> gVolumetric_Octree : register(t11, space0);

// Reflection probes (t13-t15)
TextureCubeArray gReflectionProbes : register(t13, space0);

// Samplers
SamplerState gLinearClamp        : register(s0, space0);
SamplerState gLinearWrap         : register(s1, space0);
SamplerState gPointClamp         : register(s2, space0);
SamplerComparisonState gShadowSamp : register(s3, space0);
SamplerState gAniso              : register(s4, space0);

//==============================================
// Set 1: PerPass (space1) - Defined per-shader
//==============================================
// Example for DeferredLighting:
// Texture2D gGBuffer_Albedo   : register(t0, space1);
// Texture2D gGBuffer_Normal   : register(t1, space1);
// ...

//==============================================
// Set 2: PerMaterial (space2)
//==============================================
cbuffer CB_Material : register(b0, space2) {
    float4 gAlbedoColor;
    float gRoughness;
    float gMetallic;
    float2 gMaterialPadding;
};

Texture2D gAlbedoTex             : register(t0, space2);
Texture2D gNormalTex             : register(t1, space2);
Texture2D gMetallicRoughnessTex  : register(t2, space2);
Texture2D gEmissiveTex           : register(t3, space2);
Texture2D gAOTex                 : register(t4, space2);
SamplerState gMaterialSampler    : register(s0, space2);

//==============================================
// Set 3: PerDraw (space3)
//==============================================
cbuffer CB_PerDraw : register(b0, space3) {
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};
```

---

## Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `RHI/PerFrameSlots.h` | Create | PerFrame slot constants |
| `RHI/PerPassSlots.h` | Create | PerPass slot constants |
| `RHI/PerMaterialSlots.h` | Create | PerMaterial slot constants |
| `RHI/PerDrawSlots.h` | Create | PerDraw slot constants |
| `Engine/Rendering/PassLayouts.h` | Create | Per-pass layout factory functions |
| `Engine/Rendering/MaterialLayout.h` | Create | Material layout factory |
| `Engine/Rendering/PerDrawLayout.h` | Create | PerDraw layout factory |
| `Shader/Common.hlsli` | Create | Shared HLSL declarations |
| `Engine/Rendering/ClusteredLightingPass.h` | Modify | Add `PopulatePerFrameSet()` |
| `Engine/Rendering/VolumetricLightmap.h` | Modify | Add `PopulatePerFrameSet()` |
| `Engine/Rendering/ReflectionProbeManager.h` | Modify | Add `PopulatePerFrameSet()` |

---

## Migration Order

### Phase 1: Infrastructure
1. Create slot header files
2. Create `Common.hlsli` with set declarations
3. Add `PopulatePerFrameSet()` interface to subsystems

### Phase 2: Simple Passes
1. FXAA (already done)
2. SMAA
3. Bloom
4. Tonemap

### Phase 3: Complex Passes
1. SSAO
2. SSR
3. TAA
4. DeferredLighting

### Phase 4: Main Rendering
1. GBuffer pass
2. MainPass (forward)
3. TransparentForward

---

## Verification

- [ ] TestDescriptorSet passes
- [ ] TestFXAA passes with new layout
- [ ] All post-process passes migrated
- [ ] DeferredLighting migrated
- [ ] No regression in existing tests

---

**Last Updated**: 2026-01-26
