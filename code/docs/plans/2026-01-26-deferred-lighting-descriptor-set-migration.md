# DeferredLightingPass Descriptor Set Migration

**Date**: 2026-01-26
**Status**: Plan
**Related**: `descriptor-set-architecture-issues.md`, `dx12-descriptor-set-implementation.md`

---

## Overview

Migrate `CDeferredLightingPass` from legacy per-slot binding (`SetShaderResource`, `SetConstantBufferData`) to the 4-set descriptor model. This is a clean migration with no legacy fallback.

**Sets Used:**
- **Set 0 (PerFrame, space0)**: Received from RenderPipeline - global resources
- **Set 1 (PerPass, space1)**: Owned by DeferredLightingPass - G-Buffer + SSAO

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Legacy support | None | Clean migration, no dual code paths |
| CB split | CB_PerFrame + CB_PerPass | Camera/shadow data shared, pass-specific data isolated |
| PerFrame population | RenderPipeline | Orchestrator has access to all data sources |
| G-Buffer order | Reorder to match PerPassSlots.h | Conventional order (Albedo, Normal, WorldPos...) |
| Shader strategy | New file (DeferredLighting_DS.ps.hlsl) | Safe migration, keep old until complete |
| PerPass set lifetime | Persistent, update bindings on resize | No allocation churn |

---

## Constant Buffer Split

### CB_PerFrame (b0, space0) - Shared

```cpp
struct alignas(16) CB_PerFrame {
    // Camera
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX viewProj;
    XMMATRIX invViewProj;
    XMFLOAT3 camPosWS;
    float _pad0;

    // CSM
    int cascadeCount;
    int enableSoftShadows;
    float cascadeBlendRange;
    float shadowBias;
    XMFLOAT4 cascadeSplits;
    XMMATRIX lightSpaceVPs[4];

    // Primary directional light
    XMFLOAT3 lightDirWS;
    float _pad1;
    XMFLOAT3 lightColor;
    float _pad2;

    // Global settings
    float time;
    uint32_t useReversedZ;
    float _pad3[2];
};
```

### CB_DeferredLightingPerPass (b0, space1) - Pass-specific

```cpp
struct alignas(16) CB_DeferredLightingPerPass {
    float iblIntensity;
    int diffuseGIMode;
    int probeIndex;
    int _pad0;
};
```

---

## G-Buffer Slot Reorder

**Old order (legacy):**
| Slot | Texture |
|------|---------|
| t0 | WorldPosMetallic |
| t1 | NormalRoughness |
| t2 | AlbedoAO |
| t3 | EmissiveMaterialID |
| t4 | Velocity |
| t5 | Depth |

**New order (matches PerPassSlots.h):**
| Slot | Texture | Format |
|------|---------|--------|
| t0 | AlbedoAO | RGBA8 |
| t1 | NormalRoughness | RGBA16F |
| t2 | WorldPosMetallic | RGBA16F |
| t3 | EmissiveMaterialID | RGBA16F |
| t4 | Velocity | RG16F |
| t5 | Depth | D32F (SRV) |

---

## PerPass Layout Definition

```cpp
m_perPassLayout = ctx->CreateDescriptorSetLayout(
    BindingLayoutDesc("DeferredLighting_PerPass")
        .AddItem(BindingLayoutItem::Texture_SRV(0))   // GBuffer_Albedo
        .AddItem(BindingLayoutItem::Texture_SRV(1))   // GBuffer_Normal
        .AddItem(BindingLayoutItem::Texture_SRV(2))   // GBuffer_WorldPos
        .AddItem(BindingLayoutItem::Texture_SRV(3))   // GBuffer_Emissive
        .AddItem(BindingLayoutItem::Texture_SRV(4))   // GBuffer_Velocity
        .AddItem(BindingLayoutItem::Texture_SRV(5))   // GBuffer_Depth
        .AddItem(BindingLayoutItem::Texture_SRV(7))   // SSAO
        .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_DeferredLightingPerPass)))
        .AddItem(BindingLayoutItem::Sampler(0))       // PointClamp
        .AddItem(BindingLayoutItem::Sampler(1))       // LinearClamp
);
```

---

## File Changes

### New Files

| File | Purpose |
|------|---------|
| `Shader/DeferredLighting_DS.ps.hlsl` | SM 5.1 pixel shader with register spaces |
| `RHI/CB_PerFrame.h` | Shared CB_PerFrame struct definition |

### Modified Files

| File | Changes |
|------|---------|
| `Engine/Rendering/Deferred/DeferredLightingPass.h` | Add layout/set members, update Render() signature |
| `Engine/Rendering/Deferred/DeferredLightingPass.cpp` | Replace legacy binding with descriptor sets |
| `Engine/Rendering/Deferred/GBuffer.h` | Reorder getter methods to match new slot order |
| `Engine/Rendering/Deferred/GBuffer.cpp` | Update binding order |
| `Engine/Rendering/RenderPipeline.cpp` | Create PerFrame set, populate CB_PerFrame, pass to DeferredLightingPass |
| `Engine/Rendering/VolumetricLightmap.h` | Implement IPerFrameContributor |
| `Engine/Rendering/VolumetricLightmap.cpp` | Add PopulatePerFrameSet() |
| `Engine/Rendering/ReflectionProbeManager.h` | Implement IPerFrameContributor |
| `Engine/Rendering/ReflectionProbeManager.cpp` | Add PopulatePerFrameSet() |
| `Shader/DeferredLighting.ps.hlsl` | Update G-Buffer slot order (legacy, until removed) |
| `Shader/GBuffer.ps.hlsl` | Update output order to match new slots |

---

## Implementation Steps

### Phase 1: Prerequisites

1. **VolumetricLightmap implements IPerFrameContributor**
   - Add `PopulatePerFrameSet(IDescriptorSet*)` method
   - Bind SH textures to PerFrameSlots::Tex::Volumetric_* slots
   - Bind CB to PerFrameSlots::CB::Volumetric slot

2. **ReflectionProbeManager implements IPerFrameContributor**
   - Add `PopulatePerFrameSet(IDescriptorSet*)` method
   - Bind probe textures to PerFrameSlots::Tex::ReflectionProbe_* slots

3. **Create CB_PerFrame.h**
   - Define shared CB_PerFrame struct
   - Include in Common.hlsli equivalent

### Phase 2: G-Buffer Reorder

4. **Update CGBuffer class**
   - Reorder internal texture array to match PerPassSlots
   - Update getter method order for clarity
   - Update GBuffer.ps.hlsl output order

5. **Update GBuffer consumers**
   - Any pass reading G-Buffer must use new slot order
   - SSAOPass, SSRPass, etc.

### Phase 3: RenderPipeline PerFrame Management

6. **Create PerFrame layout in RenderPipeline**
   - Define layout matching PerFrameSlots.h
   - All SRVs, samplers, CBs declared

7. **Allocate and populate PerFrame set**
   - Allocate persistent set
   - Each frame: populate CB_PerFrame, call contributors

### Phase 4: DeferredLightingPass Migration

8. **Create DeferredLighting_DS.ps.hlsl**
   - SM 5.1 with register spaces
   - Include Common.hlsli for PerFrame bindings
   - Declare PerPass bindings (space1)

9. **Update DeferredLightingPass.h**
   - Add PerPass layout/set members
   - Update Render() signature to receive perFrameSet
   - Add OnResize() for G-Buffer rebinding

10. **Update DeferredLightingPass.cpp**
    - Initialize(): create PerPass layout, allocate set, compile SM 5.1 shader
    - OnResize(): rebind G-Buffer textures
    - Render(): bind sets, update VolatileCBV, draw
    - Remove all legacy SetShaderResource/SetSampler/SetConstantBufferData calls

### Phase 5: Integration & Cleanup

11. **Update RenderPipeline call site**
    - Pass perFrameSet to DeferredLightingPass::Render()
    - Remove legacy BindToMainPass() calls

12. **Remove legacy code**
    - Delete ClusteredLightingPass::BindToMainPass()
    - Delete VolumetricLightmap::Bind()
    - Delete old DeferredLighting.ps.hlsl (after validation)

---

## Render() Method After Migration

```cpp
void CDeferredLightingPass::Render(
    const CCamera& camera,
    CScene& scene,
    CGBuffer& gbuffer,
    ITexture* hdrOutput,
    uint32_t width, uint32_t height,
    IDescriptorSet* perFrameSet,
    ITexture* ssaoTexture)
{
    auto* cmdList = ctx->GetCommandList();
    CScopedDebugEvent evt(cmdList, L"Deferred Lighting Pass");

    // Set render target
    cmdList->SetRenderTargets(1, &hdrOutput, nullptr);
    cmdList->SetViewport(0, 0, (float)width, (float)height);
    cmdList->SetScissorRect(0, 0, width, height);
    cmdList->ClearRenderTarget(hdrOutput, clearColor);

    // Set PSO
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Update per-pass bindings
    CB_DeferredLightingPerPass cbPerPass = {};
    cbPerPass.iblIntensity = dirLight ? dirLight->ibl_intensity : 1.0f;
    cbPerPass.diffuseGIMode = static_cast<int>(scene.GetLightSettings().diffuseGIMode);
    cbPerPass.probeIndex = 0;

    m_perPassSet->Bind({
        BindingSetItem::Texture_SRV(7, ssaoTexture),
        BindingSetItem::VolatileCBV(0, &cbPerPass, sizeof(cbPerPass))
    });

    // Bind descriptor sets
    cmdList->BindDescriptorSet(0, perFrameSet);
    cmdList->BindDescriptorSet(1, m_perPassSet);

    // Draw full-screen triangle
    cmdList->Draw(3, 0);
}
```

---

## Testing

1. **Build verification**: Compile with new shader and code paths
2. **Visual test**: Run TestDeferredLighting, compare screenshot with baseline
3. **Stress test**: Run TestDeferredStress with many lights
4. **Regression**: Ensure other passes still work (forward, post-process)

**Test command:**
```bash
cmake --build build && timeout 30 build/Debug/forfun.exe --test TestDeferredLighting
```

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| G-Buffer reorder breaks other passes | Update all G-Buffer consumers in same commit |
| Shader compilation fails | Keep old shader until new one validated |
| PerFrame set missing bindings | Assert on incomplete set before bind |
| Performance regression | Profile before/after, descriptor sets should be faster |

---

## Success Criteria

- [ ] DeferredLightingPass uses only BindDescriptorSet(), no legacy binding calls
- [ ] CB_PerFrame populated by RenderPipeline
- [ ] VolumetricLightmap, ReflectionProbeManager implement IPerFrameContributor
- [ ] G-Buffer order matches PerPassSlots.h
- [ ] SM 5.1 shader compiles and renders correctly
- [ ] TestDeferredLighting passes
- [ ] No visual regression in editor

---

**Last Updated**: 2026-01-26
