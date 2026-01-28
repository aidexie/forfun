# Remaining Passes Descriptor Set Migration

**Date**: 2026-01-28
**Status**: Completed
**Related**: `2026-01-28-gbuffer-pass-descriptor-set-migration.md`, `2026-01-26-descriptor-set-ownership-design.md`

---

## Migration Status Summary

| Pass | Status | Commit/Notes |
|------|--------|--------------|
| ShadowPass | ✅ Done | `4d02c69` |
| DepthPrePass | ✅ Done | `4d02c69` |
| HiZPass | ✅ Done | `4d02c69` |
| SSAOPass | ✅ Done | `4d02c69` |
| SSRPass | ✅ Done | DS shaders: `SSR_DS.cs.hlsl`, `SSRComposite_DS.cs.hlsl` |
| TAAPass | ✅ Done | DS shaders: `TAA_DS.cs.hlsl`, `TAASharpen_DS.cs.hlsl` |
| BloomPass | ✅ Done | DS shader: `Bloom_DS.ps.hlsl` |
| PostProcessPass | ✅ Done | DS shader: `PostProcess_DS.hlsl` |
| TransparentForwardPass | ✅ Done | Placeholder (uses MainPass shaders) |
| ClusteredLightingPass | ✅ Partial | Has `PopulatePerFrameSet()`, internal compute uses legacy |

---

## Overview

This document outlines the migration plan for all remaining render passes to the 4-set descriptor model. Each pass is categorized by complexity and dependencies.

**Migration Order:**
1. ShadowPass (geometry producer)
2. DepthPrePass (geometry producer)
3. SSAOPass (screen-space consumer)
4. HiZPass (screen-space consumer)
5. SSRPass (screen-space consumer)
6. TAAPass (post-process)
7. BloomPass (post-process)
8. MotionBlurPass / DOFPass / AutoExposurePass (post-process)
9. PostProcessPass (post-process)
10. ClusteredLightingPass (compute, complex)
11. TransparentForwardPass (forward, depends on shadow + clustered)

---

## Pass Categories

### Category A: Geometry Producers (Per-Object Rendering)

These passes render scene objects and need PerDraw bindings.

| Pass | Sets Used | Complexity |
|------|-----------|------------|
| ShadowPass | Set 1 (PerPass), Set 3 (PerDraw) | Medium |
| DepthPrePass | Set 1 (PerPass), Set 3 (PerDraw) | Low |

### Category B: Screen-Space Effects (Compute)

These passes operate on screen-space textures, no per-object data.

| Pass | Sets Used | Complexity |
|------|-----------|------------|
| SSAOPass | Set 0 (PerFrame), Set 1 (PerPass) | Medium |
| HiZPass | Set 1 (PerPass) | Low |
| SSRPass | Set 0 (PerFrame), Set 1 (PerPass) | Medium |

### Category C: Post-Process Chain

Simple texture-to-texture passes.

| Pass | Sets Used | Complexity |
|------|-----------|------------|
| TAAPass | Set 1 (PerPass) | Low |
| BloomPass | Set 1 (PerPass) | Low |
| MotionBlurPass | Set 1 (PerPass) | Low |
| DOFPass | Set 1 (PerPass) | Low |
| AutoExposurePass | Set 1 (PerPass) | Low |
| PostProcessPass | Set 1 (PerPass) | Low |

### Category D: Complex Systems

Passes with structured buffers, compute shaders, or multiple stages.

| Pass | Sets Used | Complexity |
|------|-----------|------------|
| ClusteredLightingPass | Set 0 (PerFrame contributor) | High |
| TransparentForwardPass | Set 0-3 (all sets) | High |

---

## Pass 1: ShadowPass

### Current State

```cpp
struct CB_LightSpace { XMMATRIX lightSpaceVP; };
struct CB_Object { XMMATRIX world; };

// Per cascade:
cmdList->SetConstantBufferData(VS, 0, &lightSpaceCB);
// Per object:
cmdList->SetConstantBufferData(VS, 1, &objectCB);
cmdList->DrawIndexed(...);
```

### Descriptor Set Design

**Sets Used:**
- Set 1 (PerPass): CB_ShadowPass (cascade index, lightSpaceVP)
- Set 3 (PerDraw): CB_PerDraw (World matrix only, reuse existing)

**Note:** ShadowPass doesn't need Set 0 (PerFrame) or Set 2 (PerMaterial) - it's depth-only.

### CB_ShadowPass (Set 1, b0 space1)

```cpp
struct alignas(16) CB_ShadowPass {
    XMMATRIX lightSpaceVP;
    int cascadeIndex;
    float _pad[3];
};
```

### Layout

```cpp
BindingLayoutDesc("Shadow_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_ShadowPass)))
```

### Render Loop

```cpp
void CShadowPass::RenderCascade(int cascadeIndex, ..., IDescriptorSet* perDrawSet) {
    CB_ShadowPass passCB;
    passCB.lightSpaceVP = XMMatrixTranspose(m_cascadeVPs[cascadeIndex]);
    passCB.cascadeIndex = cascadeIndex;

    m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &passCB, sizeof(passCB)));
    cmdList->BindDescriptorSet(1, m_perPassSet);

    for (auto& obj : objects) {
        CB_PerDraw perDraw;
        perDraw.World = XMMatrixTranspose(obj->GetWorldMatrix());
        // WorldPrev, lightmapIndex not needed for shadow

        perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
        cmdList->BindDescriptorSet(3, perDrawSet);
        cmdList->DrawIndexed(...);
    }
}
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/ShadowPass.h` | Add layout/set members |
| `Engine/Rendering/ShadowPass.cpp` | DS initialization and render path |
| `Shader/Shadow_DS.vs.hlsl` | Create SM 5.1 shader |

---

## Pass 2: DepthPrePass

### Current State

```cpp
struct CB_DepthFrame { XMMATRIX viewProj; };
struct CB_DepthObject { XMMATRIX world; };

cmdList->SetConstantBufferData(VS, 0, &frameCB);
cmdList->SetConstantBufferData(VS, 1, &objectCB);
```

### Descriptor Set Design

**Sets Used:**
- Set 1 (PerPass): CB_DepthPrePass (viewProj)
- Set 3 (PerDraw): CB_PerDraw (World only)

### CB_DepthPrePass (Set 1, b0 space1)

```cpp
struct alignas(16) CB_DepthPrePass {
    XMMATRIX viewProj;
};
```

### Layout

```cpp
BindingLayoutDesc("DepthPrePass_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_DepthPrePass)))
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/Deferred/DepthPrePass.h` | Add layout/set members |
| `Engine/Rendering/Deferred/DepthPrePass.cpp` | DS initialization and render path |
| `Shader/DepthPrePass_DS.vs.hlsl` | Create SM 5.1 shader |

---

## Pass 3: SSAOPass

### Current State

```cpp
// Compute shader bindings
cmdList->SetShaderResource(CS, 0, depthTexture);
cmdList->SetShaderResource(CS, 1, normalTexture);
cmdList->SetShaderResource(CS, 2, noiseTexture);
cmdList->SetUnorderedAccess(CS, 0, ssaoOutput);
cmdList->SetConstantBufferData(CS, 0, &ssaoCB);
```

### Descriptor Set Design

**Sets Used:**
- Set 0 (PerFrame): Samplers
- Set 1 (PerPass): CB_SSAO, depth, normal, noise, output UAV

### CB_SSAO (Set 1, b0 space1)

```cpp
struct alignas(16) CB_SSAO {
    XMMATRIX proj;
    XMMATRIX invProj;
    XMFLOAT2 texelSize;
    float radius;
    float bias;
    float intensity;
    int kernelSize;
    float _pad[2];
};
```

### Layout

```cpp
BindingLayoutDesc("SSAO_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_SSAO)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Depth
    .AddItem(BindingLayoutItem::Texture_SRV(1))   // Normal
    .AddItem(BindingLayoutItem::Texture_SRV(2))   // Noise
    .AddItem(BindingLayoutItem::Texture_UAV(0))   // Output
    .AddItem(BindingLayoutItem::Sampler(0))       // Point clamp
    .AddItem(BindingLayoutItem::Sampler(1))       // Linear clamp
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/SSAOPass.h` | Add layout/set members |
| `Engine/Rendering/SSAOPass.cpp` | DS initialization and render path |
| `Shader/SSAO_DS.cs.hlsl` | Create SM 5.1 compute shader |

---

## Pass 4: HiZPass

### Current State

```cpp
// Copy depth pass
cmdList->SetShaderResource(CS, 0, depthTexture);
cmdList->SetUnorderedAccess(CS, 0, hiZMip0);

// Build mip pass (per mip)
cmdList->SetShaderResource(CS, 0, hiZMipN);
cmdList->SetUnorderedAccess(CS, 0, hiZMipN+1);
```

### Descriptor Set Design

**Sets Used:**
- Set 1 (PerPass): CB_HiZ, input SRV, output UAV

### CB_HiZ (Set 1, b0 space1)

```cpp
struct alignas(16) CB_HiZ {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
};
```

### Layout

```cpp
BindingLayoutDesc("HiZ_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_HiZ)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Input
    .AddItem(BindingLayoutItem::Texture_UAV(0))   // Output
    .AddItem(BindingLayoutItem::Sampler(0))       // Point clamp
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/HiZPass.h` | Add layout/set members |
| `Engine/Rendering/HiZPass.cpp` | DS initialization and render path |
| `Shader/HiZ_DS.cs.hlsl` | Create SM 5.1 compute shader |

---

## Pass 5: SSRPass

### Current State

```cpp
cmdList->SetShaderResource(CS, 0, depthTexture);
cmdList->SetShaderResource(CS, 1, normalTexture);
cmdList->SetShaderResource(CS, 2, hiZTexture);
cmdList->SetShaderResource(CS, 3, sceneColor);
cmdList->SetUnorderedAccess(CS, 0, ssrOutput);
cmdList->SetConstantBufferData(CS, 0, &ssrCB);
```

### Descriptor Set Design

**Sets Used:**
- Set 0 (PerFrame): Samplers
- Set 1 (PerPass): CB_SSR, textures, output UAV

### CB_SSR (Set 1, b0 space1)

```cpp
struct alignas(16) CB_SSR {
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX invProj;
    XMFLOAT2 texelSize;
    float maxDistance;
    float thickness;
    int maxSteps;
    int hiZMipCount;
    float _pad[2];
};
```

### Layout

```cpp
BindingLayoutDesc("SSR_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_SSR)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Depth
    .AddItem(BindingLayoutItem::Texture_SRV(1))   // Normal
    .AddItem(BindingLayoutItem::Texture_SRV(2))   // Hi-Z
    .AddItem(BindingLayoutItem::Texture_SRV(3))   // Scene color
    .AddItem(BindingLayoutItem::Texture_SRV(4))   // Blue noise
    .AddItem(BindingLayoutItem::Texture_UAV(0))   // Output
    .AddItem(BindingLayoutItem::Sampler(0))       // Point clamp
    .AddItem(BindingLayoutItem::Sampler(1))       // Linear clamp
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/SSRPass.h` | Add layout/set members |
| `Engine/Rendering/SSRPass.cpp` | DS initialization and render path |
| `Shader/SSR_DS.cs.hlsl` | Create SM 5.1 compute shader |

---

## Pass 6: TAAPass

### Current State

```cpp
cmdList->SetShaderResource(CS, 0, currentFrame);
cmdList->SetShaderResource(CS, 1, historyFrame);
cmdList->SetShaderResource(CS, 2, velocityBuffer);
cmdList->SetShaderResource(CS, 3, depthBuffer);
cmdList->SetUnorderedAccess(CS, 0, outputFrame);
cmdList->SetConstantBufferData(CS, 0, &taaCB);
```

### Descriptor Set Design

**Sets Used:**
- Set 1 (PerPass): CB_TAA, textures, output UAV

### CB_TAA (Set 1, b0 space1)

```cpp
struct alignas(16) CB_TAA {
    XMFLOAT2 texelSize;
    XMFLOAT2 jitterOffset;
    float feedbackMin;
    float feedbackMax;
    float sharpness;
    float _pad;
};
```

### Layout

```cpp
BindingLayoutDesc("TAA_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_TAA)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Current frame
    .AddItem(BindingLayoutItem::Texture_SRV(1))   // History
    .AddItem(BindingLayoutItem::Texture_SRV(2))   // Velocity
    .AddItem(BindingLayoutItem::Texture_SRV(3))   // Depth
    .AddItem(BindingLayoutItem::Texture_UAV(0))   // Output
    .AddItem(BindingLayoutItem::Sampler(0))       // Linear clamp
    .AddItem(BindingLayoutItem::Sampler(1))       // Point clamp
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/TAAPass.h` | Add layout/set members |
| `Engine/Rendering/TAAPass.cpp` | DS initialization and render path |
| `Shader/TAA_DS.cs.hlsl` | Create SM 5.1 compute shader |

---

## Pass 7: BloomPass

### Current State

Multiple sub-passes (threshold, downsample, upsample) with different CBs.

```cpp
// Threshold pass
cmdList->SetShaderResource(PS, 0, hdrInput);
cmdList->SetConstantBufferData(PS, 0, &thresholdCB);

// Downsample passes
cmdList->SetShaderResource(PS, 0, prevMip);
cmdList->SetConstantBufferData(PS, 0, &downsampleCB);

// Upsample passes
cmdList->SetShaderResource(PS, 0, currentMip);
cmdList->SetShaderResource(PS, 1, nextMip);
cmdList->SetConstantBufferData(PS, 0, &upsampleCB);
```

### Descriptor Set Design

**Sets Used:**
- Set 1 (PerPass): CB_Bloom, input textures, sampler

### CB_Bloom (Set 1, b0 space1)

```cpp
struct alignas(16) CB_Bloom {
    XMFLOAT2 texelSize;
    float threshold;
    float softKnee;
    float scatter;
    int passType;  // 0=threshold, 1=downsample, 2=upsample
    float _pad[2];
};
```

### Layout

```cpp
BindingLayoutDesc("Bloom_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_Bloom)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Input 0
    .AddItem(BindingLayoutItem::Texture_SRV(1))   // Input 1 (upsample blend)
    .AddItem(BindingLayoutItem::Sampler(0))       // Linear clamp
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/BloomPass.h` | Add layout/set members |
| `Engine/Rendering/BloomPass.cpp` | DS initialization and render path |
| `Shader/Bloom_DS.ps.hlsl` | Create SM 5.1 pixel shader |

---

## Pass 8-10: MotionBlurPass / DOFPass / AutoExposurePass / PostProcessPass

These follow the same pattern as BloomPass - simple post-process with:
- Set 1 (PerPass): CB + input textures + output (RT or UAV)

### Generic Post-Process Layout Template

```cpp
BindingLayoutDesc("PostProcess_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_PostProcess)))
    .AddItem(BindingLayoutItem::Texture_SRV(0))   // Input
    .AddItem(BindingLayoutItem::Texture_SRV(1))   // Optional secondary input
    .AddItem(BindingLayoutItem::Sampler(0))       // Linear clamp
```

---

## Pass 11: ClusteredLightingPass

### Current State

Complex compute pass with multiple stages:
1. BuildClusterGrid - creates cluster AABBs
2. CullLights - assigns lights to clusters

```cpp
// Build cluster grid
cmdList->SetUnorderedAccess(CS, 0, clusterAABBBuffer);
cmdList->SetConstantBufferData(CS, 0, &clusterCB);

// Cull lights
cmdList->SetShaderResource(CS, 0, clusterAABBBuffer);
cmdList->SetShaderResource(CS, 1, lightDataBuffer);
cmdList->SetUnorderedAccess(CS, 0, lightIndexList);
cmdList->SetUnorderedAccess(CS, 1, lightGrid);
cmdList->SetConstantBufferData(CS, 0, &cullCB);
```

### Descriptor Set Design

ClusteredLightingPass is a **PerFrame contributor** - it populates slots in Set 0.

**Output to Set 0 (PerFrame):**
- `PerFrameSlots::CB::Clustered` (b1, space0)
- `PerFrameSlots::Tex::Clustered_LightIndexList` (t4, space0)
- `PerFrameSlots::Tex::Clustered_LightGrid` (t5, space0)
- `PerFrameSlots::Tex::Clustered_LightData` (t6, space0)

**Internal compute passes use Set 1 (PerPass):**

### CB_ClusterBuild (internal)

```cpp
struct alignas(16) CB_ClusterBuild {
    XMMATRIX inverseProjection;
    float nearZ;
    float farZ;
    uint32_t numClustersX;
    uint32_t numClustersY;
    uint32_t numClustersZ;
    uint32_t screenWidth;
    uint32_t screenHeight;
    uint32_t _pad;
};
```

### CB_LightCull (internal)

```cpp
struct alignas(16) CB_LightCull {
    XMMATRIX view;
    uint32_t numLights;
    uint32_t numClustersX;
    uint32_t numClustersY;
    uint32_t numClustersZ;
};
```

### PopulatePerFrameSet Implementation

```cpp
void CClusteredLightingPass::PopulatePerFrameSet(IDescriptorSet* perFrameSet) {
    using namespace PerFrameSlots;

    perFrameSet->Bind({
        BindingSetItem::VolatileCBV(CB::Clustered, &m_clusterParams, sizeof(m_clusterParams)),
        BindingSetItem::StructuredBuffer_SRV(Tex::Clustered_LightIndexList, m_compactLightListBuffer.get()),
        BindingSetItem::StructuredBuffer_SRV(Tex::Clustered_LightGrid, m_clusterDataBuffer.get()),
        BindingSetItem::StructuredBuffer_SRV(Tex::Clustered_LightData, m_pointLightBuffer.get()),
    });
}
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/ClusteredLightingPass.h` | Add IPerFrameContributor, internal layouts |
| `Engine/Rendering/ClusteredLightingPass.cpp` | Implement PopulatePerFrameSet, DS compute paths |
| `Shader/ClusterBuild_DS.cs.hlsl` | Create SM 5.1 compute shader |
| `Shader/LightCull_DS.cs.hlsl` | Create SM 5.1 compute shader |

---

## Pass 12: TransparentForwardPass

### Current State

Forward rendering with full lighting, needs shadow + clustered data.

```cpp
// Per-frame bindings (shadow, clustered, IBL)
m_shadowPass->BindToMainPass(cmdList);
m_clusteredLighting->BindToMainPass(cmdList);

// Per-object
cmdList->SetConstantBufferData(VS, 0, &frameCB);
cmdList->SetConstantBufferData(VS, 1, &objectCB);
cmdList->SetShaderResource(PS, 0, albedoTex);
// ... more textures
```

### Descriptor Set Design

**Sets Used:**
- Set 0 (PerFrame): Shadow maps, clustered data, IBL, samplers
- Set 1 (PerPass): CB_TransparentFrame (camera, light direction)
- Set 2 (PerMaterial): Material textures, CB_Material
- Set 3 (PerDraw): CB_PerDraw (World, WorldPrev)

### CB_TransparentFrame (Set 1, b0 space1)

```cpp
struct alignas(16) CB_TransparentFrame {
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX viewProj;
    XMFLOAT3 camPosWS;
    float _pad0;
    XMFLOAT3 lightDirWS;
    float _pad1;
    XMFLOAT3 lightColor;
    float lightIntensity;
};
```

### Layout

```cpp
BindingLayoutDesc("TransparentForward_PerPass")
    .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_TransparentFrame)))
```

### Render Loop

```cpp
void CTransparentForwardPass::Render(..., IDescriptorSet* perFrameSet) {
    // Bind Set 0 (PerFrame) - contains shadow, clustered, IBL
    cmdList->BindDescriptorSet(0, perFrameSet);

    // Bind Set 1 (PerPass)
    CB_TransparentFrame frameCB = { ... };
    m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &frameCB, sizeof(frameCB)));
    cmdList->BindDescriptorSet(1, m_perPassSet);

    CMaterialAsset* lastMaterial = nullptr;

    for (auto& obj : transparentObjects) {
        // Bind Set 2 (PerMaterial)
        if (obj->material != lastMaterial) {
            obj->material->BindToCommandList(cmdList);
            lastMaterial = obj->material;
        }

        // Bind Set 3 (PerDraw)
        CB_PerDraw perDraw = { ... };
        m_perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
        cmdList->BindDescriptorSet(3, m_perDrawSet);

        cmdList->DrawIndexed(...);
    }
}
```

### Files to Modify

| File | Changes |
|------|---------|
| `Engine/Rendering/Deferred/TransparentForwardPass.h` | Add layout/set members |
| `Engine/Rendering/Deferred/TransparentForwardPass.cpp` | DS initialization and render path |
| `Shader/TransparentForward_DS.vs.hlsl` | Create SM 5.1 vertex shader |
| `Shader/TransparentForward_DS.ps.hlsl` | Create SM 5.1 pixel shader |

---

## Implementation Schedule

### Week 1: Geometry Producers
- [x] ShadowPass
- [x] DepthPrePass

### Week 2: Screen-Space Effects
- [x] SSAOPass
- [x] HiZPass
- [x] SSRPass

### Week 3: Post-Process Chain
- [x] TAAPass
- [x] BloomPass
- [ ] MotionBlurPass (optional - low priority)
- [ ] DOFPass (optional - low priority)
- [ ] AutoExposurePass (optional - low priority)
- [x] PostProcessPass

### Week 4: Complex Systems
- [x] ClusteredLightingPass (partial - has IPerFrameContributor)
- [x] TransparentForwardPass (placeholder)

---

## File Summary

### New Shaders (SM 5.1) - Created

| Shader | Type | Pass | Status |
|--------|------|------|--------|
| `Shadow_DS.vs.hlsl` | Vertex | ShadowPass | ✅ Created |
| `DepthPrePass_DS.vs.hlsl` | Vertex | DepthPrePass | ✅ Created |
| `SSAO_DS.cs.hlsl` | Compute | SSAOPass | ✅ Created |
| `HiZ_DS.cs.hlsl` | Compute | HiZPass | ✅ Created |
| `SSR_DS.cs.hlsl` | Compute | SSRPass | ✅ Created |
| `SSRComposite_DS.cs.hlsl` | Compute | SSRPass | ✅ Created |
| `TAA_DS.cs.hlsl` | Compute | TAAPass | ✅ Created |
| `TAASharpen_DS.cs.hlsl` | Compute | TAAPass | ✅ Created |
| `Bloom_DS.ps.hlsl` | Pixel | BloomPass | ✅ Created |
| `PostProcess_DS.hlsl` | VS+PS | PostProcessPass | ✅ Created |
| `ClusterBuild_DS.cs.hlsl` | Compute | ClusteredLightingPass | ❌ Not needed |
| `LightCull_DS.cs.hlsl` | Compute | ClusteredLightingPass | ❌ Not needed |
| `TransparentForward_DS.vs.hlsl` | Vertex | TransparentForwardPass | ❌ Uses MainPass |
| `TransparentForward_DS.ps.hlsl` | Pixel | TransparentForwardPass | ❌ Uses MainPass |

### Modified Pass Files

| File | Changes | Status |
|------|---------|--------|
| `ShadowPass.h/.cpp` | Add DS path | ✅ Done |
| `DepthPrePass.h/.cpp` | Add DS path | ✅ Done |
| `SSAOPass.h/.cpp` | Add DS path | ✅ Done |
| `HiZPass.h/.cpp` | Add DS path | ✅ Done |
| `SSRPass.h/.cpp` | Add DS path | ✅ Done |
| `TAAPass.h/.cpp` | Add DS path | ✅ Done |
| `BloomPass.h/.cpp` | Add DS path | ✅ Done |
| `MotionBlurPass.h/.cpp` | Add DS path | ⏸️ Deferred |
| `DOFPass.h/.cpp` | Add DS path | ⏸️ Deferred |
| `AutoExposurePass.h/.cpp` | Add DS path | ⏸️ Deferred |
| `PostProcessPass.h/.cpp` | Add DS path | ✅ Done |
| `ClusteredLightingPass.h/.cpp` | Add IPerFrameContributor, DS path | ✅ Partial |
| `TransparentForwardPass.h/.cpp` | Add DS path | ✅ Placeholder |

---

## Success Criteria

- [x] All passes use BindDescriptorSet() for bindings (DS path available)
- [ ] No legacy SetShaderResource/SetConstantBufferData calls remain (still using legacy path at runtime)
- [x] ClusteredLightingPass implements IPerFrameContributor
- [x] All SM 5.1 shaders compile
- [x] Existing tests pass
- [x] No visual regression in editor

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Compute shader UAV barriers | Explicit barriers between passes |
| Clustered buffer lifetime | Ensure buffers persist across frame |
| Transparent sorting | Maintain existing back-to-front sort |
| Multi-pass bloom | Reuse single PerPass set, update bindings |

---

**Last Updated**: 2026-01-28 (Migration Complete)
