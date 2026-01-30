# Descriptor Set Architecture Issues

**Date**: 2026-01-24
**Status**: Active
**Related**: `descriptor-set-abstraction.md`, `2026-01-26-descriptor-set-ownership-design.md`

---

## Overview

This document tracks architecture issues discovered during descriptor set abstraction implementation. These issues informed the 4-set ownership model defined in `2026-01-26-descriptor-set-ownership-design.md`.

---

## Issue 1: Pass Ownership Ambiguity

**Problem**: Unclear which component owns descriptor bindings for shared resources.

**Example**:
- G-Buffer textures: Created by GBufferPass, consumed by DeferredLightingPass, SSAOPass, SSRPass
- Shadow maps: Created by ShadowPass, consumed by MainPass, DeferredLightingPass
- IBL textures: Created at startup, consumed by multiple passes

**Questions**:
- Who creates the descriptor set layout?
- Who allocates the descriptor set?
- Who binds the resources?

**Resolution**: Define clear ownership per update frequency (PerFrame, PerPass, PerMaterial, PerDraw).

---

## Issue 2: Subsystem Binding Fragmentation

**Problem**: Subsystems (ClusteredLighting, VolumetricLightmap, ReflectionProbes) each bind their own resources independently, leading to:
- Scattered binding code across multiple files
- No central view of what's bound at any given time
- Risk of slot conflicts

**Current Pattern**:
```cpp
// In DeferredLightingPass::Render()
m_clusteredLighting->BindToMainPass(cmdList);  // binds t4-t7, b1
m_volumetricLightmap->Bind(cmdList);           // binds t8-t12, b2
m_reflectionProbes->Bind(cmdList);             // binds t13-t15, b3
```

**Resolution**: `IPerFrameContributor` interface - subsystems populate a shared PerFrame set.

---

## Issue 3: Root Signature Explosion

**Problem**: Each pass potentially needs different resource combinations, leading to many root signatures.

**Options Considered**:
1. **Monolithic root signature** - One giant signature for all passes (wasteful, hits limits)
2. **Per-pass root signatures** - Maximum flexibility but no sharing
3. **Cached per-layout-combination** - Share signatures when layout combinations match

**Resolution**: Root signature caching keyed by layout pointer combination.

---

## Issue 4: Dynamic vs Static Resource Binding

**Problem**: Some resources change every frame (camera CB), others rarely change (IBL textures).

**Binding Strategies**:
| Resource Type | Update Frequency | Binding Strategy |
|---------------|------------------|------------------|
| Camera matrices | Every frame | VolatileCBV (ring buffer) |
| Shadow maps | Every frame | Persistent set, update bindings |
| IBL textures | Rarely | Persistent set, bind once |
| Material textures | On material change | Persistent set per material |
| World matrix | Every draw | Push constants / transient set |

**Resolution**: Mixed static + dynamic in same set, VolatileCBV for per-frame updates.

---

## Issue 5: Shader Register Space Mapping

**Problem**: How to map logical descriptor sets to HLSL register spaces?

**DX12 SM 5.1 Register Spaces**:
```hlsl
// space0 = PerFrame (Set 0)
Texture2DArray gShadowMaps : register(t0, space0);

// space1 = PerPass (Set 1)
Texture2D gGBuffer_Albedo : register(t0, space1);

// space2 = PerMaterial (Set 2)
Texture2D gAlbedoTex : register(t0, space2);

// space3 = PerDraw (Set 3)
cbuffer CB_PerDraw : register(b0, space3) { ... };
```

**Resolution**: 1:1 mapping of set index to register space. Create `Common.hlsli` with shared declarations.

---

## Issue 6: Slot Assignment Conflicts

**Problem**: Without centralized slot definitions, different systems may claim the same slots.

**Example Conflict**:
```cpp
// ClusteredLightingPass.cpp
constexpr uint32_t LIGHT_DATA_SLOT = 4;  // t4

// VolumetricLightmap.cpp
constexpr uint32_t SH_RED_SLOT = 4;      // Also t4!
```

**Resolution**: Centralized slot constants in header files:
- `RHI/PerFrameSlots.h` - space0 slots
- `RHI/PerPassSlots.h` - space1 slots
- `RHI/PerMaterialSlots.h` - space2 slots
- `RHI/PerDrawSlots.h` - space3 slots

---

## Implementation Status

| Task | Status |
|------|--------|
| Create slot header files | Done |
| Create IPerFrameContributor interface | Done |
| ClusteredLightingPass implements IPerFrameContributor | Done |
| Create Common.hlsli | Done |
| Create PassLayouts factory | Done |
| Migrate FXAA to descriptor sets | Done |
| VolumetricLightmap implements IPerFrameContributor | Pending |
| ReflectionProbeManager implements IPerFrameContributor | Pending |
| Migrate DeferredLightingPass | Pending |

---

**Last Updated**: 2026-01-26
