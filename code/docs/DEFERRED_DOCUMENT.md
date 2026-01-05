# Deferred Rendering Pipeline

**Status**: Complete
**Related**: `RENDERING.md`, `ROADMAP.md`

---

## Overview

The engine supports two rendering pipelines, selectable via configuration:

| Pipeline | Best For | Implementation |
|----------|----------|----------------|
| **Forward+** | Scenes with <50 lights | `ForwardRenderPipeline.h/cpp` |
| **Deferred** | Complex scenes, screen-space effects | `Deferred/DeferredRenderPipeline.h/cpp` |

### Pipeline Selection

Configure in `assets/config/render.json`:

```json
{
    "backend": "DX12",
    "pipeline": "Deferred",
    "window": { "width": 1600, "height": 900 }
}
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      Deferred Rendering Pipeline                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Pass 0: Depth Pre-Pass                                                     │
│     • Depth-only rendering (no pixel shader)                                │
│     • Populates depth buffer to eliminate G-Buffer overdraw                 │
│                                                                              │
│  Pass 1: G-Buffer Pass                                                      │
│     • Renders opaque geometry to 5 render targets                           │
│     • Depth Test: LessEqual (with bias), no depth write                     │
│     • Applies 2D Lightmap (pre-baked to Emissive)                           │
│                                                                              │
│  Pass 2: Shadow Pass                                                         │
│     • CSM for directional light (1-4 cascades)                              │
│     • Shared with Forward+ pipeline                                         │
│                                                                              │
│  Pass 3: Deferred Lighting Pass                                             │
│     • Full-screen quad evaluating all lights per pixel                      │
│     • PBR lighting (Cook-Torrance BRDF)                                     │
│     • IBL, Volumetric Lightmap, Reflection Probes                           │
│     • Material branching via MaterialID                                     │
│                                                                              │
│  Pass 4: Transparent Forward Pass                                           │
│     • Forward rendering for alpha-blended objects                           │
│     • Back-to-front sorting                                                 │
│                                                                              │
│  Pass 5: Post-Processing                                                    │
│     • Tonemapping (HDR → LDR)                                               │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## G-Buffer Layout

### 5 Render Targets + Depth

| RT | Format | Content | Size @1080p |
|----|--------|---------|-------------|
| RT0 | R16G16B16A16_FLOAT | WorldPosition.xyz + Metallic | 16 MB |
| RT1 | R16G16B16A16_FLOAT | Normal.xyz + Roughness | 16 MB |
| RT2 | R8G8B8A8_UNORM_SRGB | Albedo.rgb + AO | 8 MB |
| RT3 | R16G16B16A16_FLOAT | Emissive.rgb + MaterialID | 16 MB |
| RT4 | R16G16_FLOAT | Velocity.xy | 8 MB |
| Depth | D32_FLOAT | Scene Depth | 8 MB |
| **Total** | | | **~72 MB** |

### Design Decisions

| Aspect | Decision | Rationale |
|--------|----------|-----------|
| World Position | Store directly | Avoids reconstruction artifacts |
| Normal Format | R16G16B16A16_FLOAT | Highest quality, no quantization |
| Velocity Buffer | Included | Ready for TAA/Motion Blur |

---

## Material System

### EMaterialType Enum

Defined in `Core/MaterialAsset.h`:

```cpp
enum class EMaterialType : uint8_t {
    Standard = 0,     // Default PBR (Cook-Torrance BRDF)
    Subsurface = 1,   // Skin, wax, leaves (future)
    Cloth = 2,        // Fabric, velvet (future)
    Hair = 3,         // Anisotropic hair (future)
    ClearCoat = 4,    // Car paint, lacquer (future)
    Unlit = 5         // Emissive only, no lighting
};
```

### Shader Implementation

MaterialID is encoded in RT3.a and decoded in `DeferredLighting.ps.hlsl`:

```hlsl
int materialID = (int)(rt3.a * 255.0 + 0.5);

// MATERIAL_UNLIT: Skip all lighting
if (materialID == MATERIAL_UNLIT) {
    return float4(emissive + albedo, 1.0);
}

// MATERIAL_STANDARD: Full PBR lighting
// ... standard lighting code ...
```

---

## Depth Pre-Pass

### Purpose

G-Buffer pixel shader is expensive (5-7 texture samples). Without pre-pass, overlapping geometry causes overdraw. With pre-pass, each pixel executes PS exactly once.

### Implementation

| Pass | Depth Test | Depth Write | Notes |
|------|------------|-------------|-------|
| Depth Pre-Pass | Less | ON | Vertex shader only |
| G-Buffer Pass | LessEqual | OFF | With negative depth bias |

### Z-Fighting Fix

The pre-pass uses `pos * ViewProj` (single multiply) while G-Buffer uses `(pos * View) * Proj` (two multiplies). Floating-point non-associativity causes precision differences.

**Solution**: Negative depth bias (`-1`) in GBufferPass compensates for FP error.

---

## GI Integration

### Diffuse GI Modes

```hlsl
#define DIFFUSE_GI_VOLUMETRIC_LIGHTMAP 0  // Per-pixel SH9 lookup
#define DIFFUSE_GI_GLOBAL_IBL 1           // Skybox irradiance
#define DIFFUSE_GI_NONE 2                 // Disabled
#define DIFFUSE_GI_LIGHTMAP_2D 3          // Baked to emissive in G-Buffer
```

### 2D Lightmap Integration

Pre-baked to Emissive channel during G-Buffer pass:

```hlsl
// GBuffer.ps.hlsl
if (gLightmapIndex >= 0) {
    float3 lightmapGI = SampleLightmap2D(i.uv2, gLightmapIndex, gSamp);
    emissive += lightmapGI * albedo;
}
```

---

## File Structure

```
Engine/Rendering/
├── RenderPipeline.h              # Base class interface
├── ForwardRenderPipeline.h/cpp   # Forward+ implementation
└── Deferred/
    ├── DeferredRenderPipeline.h/cpp   # Main orchestration
    ├── GBuffer.h/cpp                  # G-Buffer management
    ├── DepthPrePass.h/cpp             # Depth-only pre-pass
    ├── GBufferPass.h/cpp              # G-Buffer rendering
    ├── DeferredLightingPass.h/cpp     # Full-screen lighting
    └── TransparentForwardPass.h/cpp   # Transparent objects

Shader/
├── GBuffer.vs.hlsl               # G-Buffer vertex shader
├── GBuffer.ps.hlsl               # G-Buffer pixel shader
└── DeferredLighting.ps.hlsl      # Deferred lighting
```

---

## Performance

### Benchmark Results

Test scene: 26 objects, 16 point lights, DX12, 1600×900

| Pipeline | Avg FPS | Frame Time |
|----------|---------|------------|
| Forward+ | 38.7 | 25.86 ms |
| Deferred | 27.9 | 35.90 ms |

### Analysis

- **Forward+ faster for small scenes** (~39% in this test)
- **Deferred overhead**: G-Buffer fill + full-screen lighting pass
- **Deferred benefits**: Scales better with 100+ lights, enables screen-space effects

### Theoretical Performance @ 1080p

```
G-Buffer read: 72 MB
Shadow maps: ~16 MB (4 cascades × 2K)
Total bandwidth: ~92 MB/frame @ 60 FPS = 5.5 GB/s
RTX 2060 bandwidth: 336 GB/s → 1.6% utilization
```

---

## Known Issues

| Issue | Status | Description |
|-------|--------|-------------|
| DX12 texture first-load | Open | `GenerateMips` triggers during first `SetShaderResource`. Resource state issue. |
| Mesh loading errors | Open | Occasional garbage paths during scene iteration. |

---

## Testing

| Test | Purpose |
|------|---------|
| `TestGBuffer` | Verify G-Buffer infrastructure |
| `TestMaterialTypes` | Verify MaterialID system |
| `TestDeferredPerf` | Performance benchmarking |

Run tests:
```bash
forfun.exe --test TestGBuffer
forfun.exe --test TestMaterialTypes
forfun.exe --test TestDeferredPerf
```

---

## Future Extensions

These features are designed for but not yet implemented:

- **Screen-Space Effects**: SSAO, SSR, TAA (velocity buffer ready)
- **Deferred Decals**: G-Buffer modification (MaterialID reserves space)
- **Advanced Materials**: Subsurface, Cloth, Hair, ClearCoat shading models
- **Clustered Deferred**: Compute shader lighting (if needed for 100+ lights)

---

**Last Updated**: 2026-01-05
