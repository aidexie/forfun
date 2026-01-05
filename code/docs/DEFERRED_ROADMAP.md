# Deferred Rendering Pipeline - Design & Roadmap

**Status**: Phase 3.2.4 Complete - Material System Done
**Target**: True Deferred Pipeline for best rendering quality
**Related**: `ROADMAP.md`, `docs/RENDERING.md`

---

## Overview

This document outlines the design and implementation roadmap for migrating from Forward+ to a **True Deferred Rendering Pipeline**.

### Why True Deferred?

| Aspect | Forward+ (Current) | True Deferred (Target) |
|--------|-------------------|------------------------|
| Lighting computation | Per-object, per-light | Per-pixel, all lights once |
| Overdraw cost | High (re-evaluates lights) | Low (single evaluation) |
| Screen-space effects | Extra passes needed | Natural fit (G-Buffer) |
| Material variety | Limited | Full support via MaterialID |
| Decals | Complex | Deferred decals (simple) |

---

## Design Decisions

### Finalized Choices

| Aspect | Decision | Rationale |
|--------|----------|-----------|
| **Depth Pre-Pass** | Yes | Eliminates G-Buffer overdraw, expensive PS (5-7 texture samples) |
| **Lighting Pass** | Full-Screen Quad (Phase 1) | Simpler implementation, migrate to clustered later |
| **World Position** | Store in G-Buffer | Avoids reconstruction artifacts |
| **Normal Quality** | R16G16B16A16_FLOAT | Highest quality, no quantization |
| **MSAA** | Not supported | Will use TAA for anti-aliasing |
| **Decals** | Future support designed | MaterialID reserves space |
| **Material Types** | Multiple types | MaterialID for shader branching |
| **2D Lightmap** | Pre-bake to Emissive | Apply in G-Buffer pass, efficient |
| **Target** | 100+ lights @ 1080p | Full-screen quad acceptable |

---

## G-Buffer Layout

### 5 Render Targets + Depth

```
┌──────┬──────────────────────────┬───────────────────────────────────────────┐
│  RT  │  Format                  │  Content                                  │
├──────┼──────────────────────────┼───────────────────────────────────────────┤
│ RT0  │ R16G16B16A16_FLOAT       │ WorldPosition.xyz + Metallic.a            │
│ RT1  │ R16G16B16A16_FLOAT       │ Normal.xyz + Roughness.a                  │
│ RT2  │ R8G8B8A8_UNORM_SRGB      │ Albedo.rgb + AO.a                         │
│ RT3  │ R16G16B16A16_FLOAT       │ Emissive.rgb + MaterialID.a               │
│ RT4  │ R16G16_FLOAT             │ Velocity.xy (TAA/MotionBlur)              │
├──────┼──────────────────────────┼───────────────────────────────────────────┤
│Depth │ D32_FLOAT                │ Scene Depth                               │
└──────┴──────────────────────────┴───────────────────────────────────────────┘
```

### Memory Budget @ 1080p

| RT | Bytes/Pixel | Total |
|----|-------------|-------|
| RT0 | 8 | 16 MB |
| RT1 | 8 | 16 MB |
| RT2 | 4 | 8 MB |
| RT3 | 8 | 16 MB |
| RT4 | 4 | 8 MB |
| Depth | 4 | 8 MB |
| **Total** | **36** | **~72 MB** |

---

## Material ID System

Encoded in RT3.a (0-255 range as float 0.0-1.0):

```hlsl
// Material IDs
#define MATERIAL_STANDARD       0    // Default PBR
#define MATERIAL_SUBSURFACE     1    // Skin, wax, leaves
#define MATERIAL_CLOTH          2    // Fabric, velvet
#define MATERIAL_HAIR           3    // Anisotropic hair
#define MATERIAL_CLEAR_COAT     4    // Car paint, lacquer
#define MATERIAL_UNLIT          5    // Emissive only

// Reserved: 6-15 for future materials
// Reserved: 16-31 for decal blend modes
```

### Shader Usage

```hlsl
int materialID = (int)(gbuffer3.a * 255.0);
switch (materialID) {
    case MATERIAL_STANDARD:
        color = EvaluateStandardPBR(gbufferData, lights);
        break;
    case MATERIAL_SUBSURFACE:
        color = EvaluateSubsurfacePBR(gbufferData, lights);
        break;
    // ...
}
```

---

## Pipeline Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      True Deferred Rendering Pipeline                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Pass 0: Depth Pre-Pass                                                     │
│     • Render all opaque objects (depth only, no pixel shader)               │
│     • Depth Test: LESS, Depth Write: ON                                     │
│     • Purpose: Populate depth buffer to eliminate G-Buffer overdraw         │
│     • Vertex shader only, minimal GPU cost                                  │
│                                                                              │
│  Pass 1: G-Buffer Pass (Opaque Geometry)                                    │
│     • Render all opaque objects                                             │
│     • Depth Test: EQUAL, Depth Write: OFF                                   │
│     • Only visible pixels execute pixel shader (no overdraw)                │
│     • Output: WorldPos, Normal, Albedo, Emissive+Lightmap, Velocity         │
│     • 2D Lightmap applied here (pre-baked to Emissive)                      │
│                                                                              │
│  Pass 1.5: Deferred Decal Pass (Future)                                     │
│     • Render decal volumes                                                  │
│     • Modify Albedo, Normal, Roughness in G-Buffer                          │
│     • Depth test, no depth write                                            │
│                                                                              │
│  Pass 2: Shadow Pass                                                         │
│     • CSM for directional light                                             │
│     • Point/Spot shadow maps                                                │
│                                                                              │
│  Pass 3: Deferred Lighting Pass                                             │
│     • Full-screen quad (Phase 1) or Compute (Phase 2)                       │
│     • Evaluate ALL lights per pixel                                         │
│     • PBR (Cook-Torrance BRDF)                                              │
│     • Sample IBL, Volumetric Lightmap, Reflection Probes                    │
│     • Output: HDR Lit Color                                                 │
│                                                                              │
│  Pass 4: Screen-Space Effects                                               │
│     • SSAO (G-Buffer depth + normal)                                        │
│     • SSR (G-Buffer + HDR scene)                                            │
│     • SSGI (G-Buffer + HDR scene)                                           │
│                                                                              │
│  Pass 5: Transparent Forward Pass                                           │
│     • Render transparent objects with full lighting                         │
│     • Back-to-front sorting                                                 │
│                                                                              │
│  Pass 6: Post-Processing                                                    │
│     • Bloom, Tonemapping, TAA                                               │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Depth Pre-Pass Details

### Why Pre-Pass?

G-Buffer pixel shader is expensive (5-7 texture samples per pixel):
```hlsl
// Each sample costs bandwidth + ALU
albedo    = AlbedoMap.Sample(...)      // 1
normal    = NormalMap.Sample(...)       // 2
metallic  = MetallicMap.Sample(...)     // 3 (often packed with roughness)
emissive  = EmissiveMap.Sample(...)     // 4
ao        = AOMap.Sample(...)           // 5
lightmap  = Lightmap2D.Sample(...)      // 6-7 (for static objects)
```

Without pre-pass, overlapping geometry executes PS multiple times per pixel (overdraw).
With pre-pass, each pixel executes PS exactly once.

### Pre-Pass Implementation

```hlsl
// DepthPrePass.vs.hlsl
struct VSInput {
    float3 position : POSITION;
};

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), g_WorldViewProj);
    return output;
}

// No pixel shader needed - depth-only pass
```

### Render States

| Pass | Depth Test | Depth Write | Pixel Shader |
|------|------------|-------------|--------------|
| Depth Pre-Pass | LESS | ON | None (null PS) |
| G-Buffer Pass | EQUAL | OFF | Full G-Buffer PS |

### Performance Trade-off

| Factor | Cost | Benefit |
|--------|------|---------|
| Vertex processing | 2× (double transform) | - |
| Draw calls | 2× (same objects twice) | - |
| Pixel shader | - | 0% overdraw |
| Bandwidth | Depth buffer written twice | G-Buffer written once per visible pixel |

**Net benefit**: Positive for scenes with any overdraw (typical case).

---

## 2D Lightmap Integration

**Strategy**: Pre-bake to Emissive channel during G-Buffer pass

```hlsl
// GBuffer.ps.hlsl
struct PSOutput {
    float4 worldPosMetallic : SV_Target0;
    float4 normalRoughness  : SV_Target1;
    float4 albedoAO         : SV_Target2;
    float4 emissiveMaterialID : SV_Target3;
    float2 velocity         : SV_Target4;
};

PSOutput main(PSInput input) {
    PSOutput output;

    // ... other G-Buffer outputs ...

    // Apply 2D Lightmap to Emissive
    float3 lightmapGI = SampleLightmap2D(input.uv2, lightmapIndex);
    output.emissiveMaterialID.rgb = material.emissive + lightmapGI * albedo.rgb;
    output.emissiveMaterialID.a = materialID / 255.0;

    return output;
}
```

**Benefits**:
- No extra G-Buffer RT for UV2
- Lightmap contribution baked once per pixel
- DeferredLighting simply adds Emissive to final color

---

## GI Sampling in Deferred Lighting

```hlsl
// DeferredLighting.ps.hlsl

float3 ComputeGI(float3 worldPos, float3 normal, float3 albedo, float3 emissive) {
    float3 gi = float3(0, 0, 0);

    // 1. Pre-baked 2D Lightmap (already in Emissive from G-Buffer pass)
    gi += emissive;

    // 2. Volumetric Lightmap (dynamic objects, per-pixel)
    if (usesVolumetricLightmap) {
        gi += SampleVolumetricLightmap(worldPos, normal) * albedo;
    }

    // 3. Global IBL fallback
    else if (diffuseGIMode == GlobalIBL) {
        gi += SampleIrradianceMap(normal) * albedo;
    }

    return gi;
}
```

---

## File Structure

```
Engine/Rendering/
├── ForwardRenderPipeline.h/cpp       # Forward+ pipeline (clustered lighting)
├── RenderPipeline.h                  # Base class with virtual interface
└── Deferred/                         # ✅ Deferred pipeline module
    ├── DeferredRenderPipeline.h/cpp  # ✅ Main pipeline orchestration
    ├── GBuffer.h/cpp                 # ✅ G-Buffer management (5 RTs + depth)
    ├── DepthPrePass.h/cpp            # ✅ Depth-only pre-pass
    ├── GBufferPass.h/cpp             # ✅ G-Buffer rendering
    ├── DeferredLightingPass.h/cpp    # ✅ Screen-space lighting (full-screen quad)
    └── TransparentForwardPass.h/cpp  # ✅ Forward pass for alpha-blended objects

Core/
├── RenderConfig.h/cpp                # ✅ Pipeline selection (Forward/Deferred)
└── Testing/
    └── Screenshot.h/cpp              # ✅ Updated for CRenderPipeline*

Shader/Deferred/
├── DepthPrePass.vs.hlsl              # ✅ Depth-only vertex shader
├── GBuffer.vs.hlsl                   # ✅ G-Buffer vertex shader
├── GBuffer.ps.hlsl                   # ✅ G-Buffer pixel shader (5 RT output)
├── DeferredLighting.vs.hlsl          # ✅ Full-screen quad vertex shader
└── DeferredLighting.ps.hlsl          # ✅ Deferred lighting pixel shader

Future:
└── ScreenSpaceEffects/               # Planned
    ├── SSAOPass.h/cpp
    ├── SSRPass.h/cpp
    └── SSGIPass.h/cpp
```

---

## Implementation Roadmap

### Phase 3.2.1: Depth Pre-Pass & G-Buffer Infrastructure ✅ COMPLETE
- [x] Create `DepthPrePass` class (depth-only rendering)
- [x] Create `DepthPrePass.vs.hlsl` (vertex shader only, no PS)
- [x] Create `GBuffer` class (manage 5 RTs + depth)
- [x] Create `GBuffer.vs.hlsl` / `GBuffer.ps.hlsl`
- [x] Implement Depth Pre-Pass (LESS test, write ON)
- [x] Implement G-Buffer pass (EQUAL test, write OFF)
- [x] Debug visualization (view each G-Buffer channel)
- [x] Test: `TestGBuffer` - verify G-Buffer infrastructure

### Phase 3.2.2: Deferred Lighting (Standard PBR) ✅ COMPLETE
- [x] Create `DeferredLightingPass` class
- [x] Create `DeferredLighting.ps.hlsl` (full-screen quad)
- [x] Implement directional light with CSM shadows
- [x] Implement point lights (clustered lighting integration)
- [x] Implement spot lights (clustered lighting integration)
- [x] Integrate IBL (diffuse + specular via Reflection Probes)

### Phase 3.2.3: Complete Integration ✅ COMPLETE
- [x] Create `DeferredRenderPipeline` class
- [x] Integrate 2D Lightmap (pre-bake to Emissive in G-Buffer pass)
- [x] Integrate Volumetric Lightmap (SH9 sampling in lighting pass)
- [x] Integrate Reflection Probes (per-pixel IBL)
- [x] Implement transparent forward pass (`TransparentForwardPass`)
- [x] Add velocity buffer output (RT4: R16G16_FLOAT)
- [x] Runtime pipeline switching via `render.json` config
- [x] Polymorphic `CRenderPipeline*` in main.cpp
- [x] Test: `TestGBuffer` - G-Buffer infrastructure validation

### Phase 3.2.4: Material System ✅ COMPLETE
- [x] Implement MaterialID encoding/decoding
- [x] Add `EMaterialType` enum to `MaterialAsset.h`
- [x] Add MATERIAL_STANDARD (default PBR) - always supported
- [x] Add MATERIAL_UNLIT (emissive only) - early-out in lighting shader
- [x] Pass materialType from CMaterialAsset → GBufferPass → G-Buffer RT3.a
- [x] Shader branching in DeferredLighting.ps.hlsl based on MaterialID
- [x] Test: `TestMaterialTypes` - verify material switching
- [ ] Design subsurface/cloth/hair shaders (future phases)

### Phase 3.2.5: Performance Optimization
- [ ] Profile G-Buffer bandwidth
- [ ] Migrate to clustered deferred (compute shader)
- [ ] Implement light culling per tile
- [ ] Compare performance with Forward+

### Phase 3.3: Screen-Space Effects (Future)
- [ ] SSAO implementation
- [ ] SSR implementation
- [ ] TAA implementation
- [ ] Motion blur (using velocity buffer)

### Phase 3.4: Deferred Decals (Future)
- [ ] Decal volume rendering
- [ ] G-Buffer modification shaders
- [ ] Blend mode support via MaterialID

---

## Performance Analysis

### Target: 100+ Lights @ 1080p @ 60 FPS

**Full-Screen Quad Analysis:**
```
Pixels: 1920 × 1080 = 2,073,600
Lights: 100
BRDF cost: ~100 FLOPs/light

Total FLOPs/frame = 2M × 100 × 100 = 20.7 GFLOPS
At 60 FPS = 1.24 TFLOPS

RTX 2060 capacity: ~6.5 TFLOPS
Usage: ~19% (acceptable)
```

**Bandwidth Analysis:**
```
G-Buffer read: 72 MB
Shadow maps: ~16 MB (4 cascades × 2K)
IBL/Probes: ~4 MB
Total read: ~92 MB/frame

At 60 FPS = 5.5 GB/s
RTX 2060 bandwidth: 336 GB/s
Usage: ~1.6% (excellent)
```

**Verdict**: Full-screen quad approach is viable for Phase 1.

---

## Migration Strategy

### Coexistence Period

Both pipelines will coexist during development:

```cpp
// RenderPipelineManager.h
enum class ERenderPipeline {
    Forward,    // Current ForwardRenderPipeline
    Deferred    // New DeferredRenderPipeline
};

class RenderPipelineManager {
    void SetPipeline(ERenderPipeline type);
    void Render(Scene* scene, Camera* camera);
};
```

### Configuration ✅ IMPLEMENTED

Pipeline selection via `assets/config/render.json`:

```json
{
    "backend": "DX11",           // "DX11" | "DX12"
    "pipeline": "Deferred",      // "Forward" | "Deferred"
    "window": {
        "width": 1600,
        "height": 900,
        "fullscreen": false,
        "vsync": true
    },
    "graphics": {
        "msaaSamples": 1,
        "enableValidation": false
    }
}
```

**Implementation**:
- `Core/RenderConfig.h`: `ERenderPipeline` enum + `SRenderConfig::pipeline` field
- `main.cpp`: Polymorphic `CRenderPipeline*` created based on config
- Runtime switching: Change config and restart application

---

## Testing Strategy

Each phase includes automated tests:

| Test | Purpose |
|------|---------|
| `TestDepthPrePass` | Verify depth buffer population |
| `TestGBuffer` | Verify G-Buffer output (visual + data) |
| `TestDeferredLighting` | Compare lighting with Forward+ |
| `TestDeferredFull` | Full scene rendering |
| `TestMaterialTypes` | Verify MaterialID system |
| `TestDeferredPerf` | Performance benchmarking (measure overdraw reduction) |

---

## Known Issues (Phase 3.2.3)

| Issue | Severity | Description |
|-------|----------|-------------|
| Mesh loading errors | Medium | "Failed to load mesh" errors with garbage paths during deferred rendering. Appears to be memory corruption in scene iteration. |
| CreateShaderResourceView E_INVALIDARG | Low | Occasional SRV creation failure, likely related to G-Buffer resize timing. |
| DX12 texture first-load trigger | Medium | `TextureLoader.cpp:112` (`GenerateMips`) triggers during `cmdList->SetShaderResource` on first texture load. Likely DX12 resource state issue - texture not transitioned properly before binding. |
| Depth Pre-Pass Z-fighting | Fixed | Black stripes in G-Buffer due to FP precision difference between pre-pass (`pos*ViewProj`) and G-Buffer pass (`(pos*View)*Proj`). Fixed with negative depth bias in GBufferPass. |

These issues don't prevent the pipeline from functioning but should be investigated in Phase 3.2.4.

---

## References

- UE4/UE5 Deferred Shading
- Frostbite Rendering Architecture (GDC 2014)
- DOOM (2016) - Clustered Forward vs Deferred analysis

---

**Last Updated**: 2026-01-05
