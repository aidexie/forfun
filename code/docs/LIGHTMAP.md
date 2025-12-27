# 2D Lightmap System

Complete documentation for the 2D Lightmap baking system.

---

## Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Lightmap Pipeline                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Step 1: UV2 Generation (xatlas)                                │
│     Input:  Mesh vertices/indices                               │
│     Output: UV2 coordinates per vertex                          │
│                                                                 │
│  Step 2: Atlas Packing                                          │
│     Input:  Multiple meshes with UV2                            │
│     Output: Scale/offset per mesh, atlas layout                 │
│                                                                 │
│  Step 3: Rasterization                                          │
│     Input:  Triangles in UV2 space                              │
│     Output: Per-texel worldPos + normal                         │
│                                                                 │
│  Step 4: Baking (DXR GPU Path Tracing)                          │
│     Input:  Texel positions/normals                             │
│     Output: Irradiance (RGB HDR) per texel                      │
│                                                                 │
│  Step 5: Post-processing                                        │
│     Input:  Raw lightmap with holes                             │
│     Output: Dilated lightmap, no seam bleeding                  │
│                                                                 │
│  Step 6: Runtime Sampling                                       │
│     Shader: texture(lightmap, uv2) * albedo                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Architecture

### File Structure

```
Engine/Rendering/Lightmap/
├── LightmapTypes.h            # Data structures (STexelData, SLightmapInfo, etc.)
├── LightmapUV2.h/cpp          # xatlas UV2 generation
├── LightmapAtlas.h/cpp        # Atlas packing (row-based algorithm)
├── LightmapRasterizer.h/cpp   # Barycentric triangle rasterization
├── LightmapBaker.h/cpp        # Main orchestration (UV2→Atlas→Raster→Bake)
├── Lightmap2DGPUBaker.h/cpp   # GPU DXR baking backend
└── Lightmap2DManager.h/cpp    # Runtime lightmap data manager (singleton)

Shader/
├── DXR/Lightmap2DBake.hlsl    # DXR ray tracing shader
├── Lightmap2DFinalize.cs.hlsl # Compute shader for finalization
└── Lightmap2D.hlsl            # Runtime sampling include
```

---

## GPU Baker (CLightmap2DGPUBaker)

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Sampling | Cosine-weighted hemisphere | Optimal for Lambertian diffuse |
| Rays/texel | 64 (configurable) | Balance quality vs. speed |
| Bounces | 3 (configurable) | 95%+ energy captured |
| Output | R16G16B16A16_FLOAT | HDR, no banding |
| Batching | 1024 texels/dispatch | GPU utilization |

### Key Differences from Volumetric Lightmap

| Aspect | Volumetric Lightmap | 2D Lightmap |
|--------|---------------------|-------------|
| Data structure | 3D voxel grid (octree) | 2D texture atlas |
| Sample location | 3D world position | Surface texel (UV2) |
| Output | SH9 coefficients | RGB irradiance |
| Ray distribution | Full sphere (cubemap) | Hemisphere above normal |
| Use case | Dynamic objects | Static geometry |

### Shader Pipeline

```
┌─────────────────────────────────────────────────────────────────┐
│ Pass 1: DXR Ray Tracing (Lightmap2DBake.hlsl)                   │
├─────────────────────────────────────────────────────────────────┤
│ Dispatch(batchSize, samplesPerTexel, 1)                         │
│                                                                 │
│ RayGen:                                                         │
│   1. Load texel worldPos/normal from g_TexelData                │
│   2. Generate cosine-weighted hemisphere ray                    │
│   3. Path trace (max 3 bounces)                                 │
│   4. Atomic add to g_Accumulation[atlasIdx]                     │
│                                                                 │
│ Resources:                                                      │
│   t0: TLAS                t5: Vertices                          │
│   t1: Skybox              t6: Indices                           │
│   t2: Materials           t7: TexelData                         │
│   t3: Lights              u0: Accumulation                      │
│   t4: Instances           s0: LinearSampler                     │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Pass 2: Finalize (Lightmap2DFinalize.cs.hlsl)                   │
├─────────────────────────────────────────────────────────────────┤
│ Dispatch(atlasWidth/8, atlasHeight/8, 1)                        │
│                                                                 │
│ CSMain:                                                         │
│   1. Read accumulated radiance + sample count                   │
│   2. Normalize (radiance / sampleCount)                         │
│   3. Write to output texture (R16G16B16A16_FLOAT)               │
└─────────────────────────────────────────────────────────────────┘
```

### Usage Example

```cpp
#include "Engine/Rendering/Lightmap/Lightmap2DGPUBaker.h"

// Initialize baker (once)
CLightmap2DGPUBaker baker;
if (!baker.Initialize()) {
    CFFLog::Error("GPU baker not available");
    return;
}

// Configure
SLightmap2DGPUBakeConfig config;
config.samplesPerTexel = 64;   // Monte Carlo samples
config.maxBounces = 3;         // GI bounces
config.skyIntensity = 1.0f;
config.progressCallback = [](float p, const char* s) {
    CFFLog::Info("Bake: %.0f%% - %s", p * 100, s);
};

// Bake from scene and rasterizer
RHI::TexturePtr lightmap = baker.BakeLightmap(
    scene,
    rasterizer,  // CLightmapRasterizer with texel data
    config
);

// Use lightmap texture...
```

---

## CPU Components

### UV2 Generation (xatlas)

```cpp
SUV2GenerationResult result = GenerateUV2(
    positions,          // std::vector<XMFLOAT3>
    normals,
    uv1,                // Original texture UVs
    indices,
    texelsPerUnit       // Target density (default: 16)
);

if (result.success) {
    // xatlas may split vertices at UV seams
    // result.positions, normals, uv2, indices are the new mesh data
    int atlasWidth = result.atlasWidth;
    int atlasHeight = result.atlasHeight;
}
```

### Atlas Packing

```cpp
CLightmapAtlasBuilder builder;

// Add meshes with their world bounds
for (auto& mesh : meshes) {
    SLightmapMeshInfo info;
    info.meshRendererIndex = i;
    info.boundsMin = worldMin;
    info.boundsMax = worldMax;
    builder.AddMesh(info);
}

// Build atlas
SLightmapAtlasConfig config;
config.resolution = 1024;
config.texelsPerUnit = 16;
builder.Build(config);

// Get per-mesh lightmap info
const auto& infos = builder.GetLightmapInfos();
// infos[i].scaleOffset = {scaleU, scaleV, offsetU, offsetV}
```

### Rasterization

```cpp
CLightmapRasterizer rasterizer;
rasterizer.Initialize(atlasWidth, atlasHeight);

// Rasterize each mesh
for (const auto& entry : atlas.GetEntries()) {
    rasterizer.RasterizeMesh(
        positions, normals, uv2, indices,
        worldMatrix,
        entry.atlasX, entry.atlasY,
        entry.width, entry.height
    );
}

// Get texel data
const auto& texels = rasterizer.GetTexels();
int validCount = rasterizer.GetValidTexelCount();
```

---

## Runtime Sampling

### Shader Include (Lightmap2D.hlsl)

```hlsl
// Resources
Texture2D<float4> g_Lightmap2D : register(t16);
StructuredBuffer<float4> g_LightmapScaleOffsets : register(t17);

cbuffer CB_Lightmap2D : register(b7) {
    int lm_enabled;
    float lm_intensity;
    float2 _lm_pad;
};

// Sample lightmap in pixel shader
float3 SampleLightmap2D(float2 uv2, int lightmapIndex, SamplerState samp) {
    if (lm_enabled == 0 || lightmapIndex < 0) {
        return float3(0, 0, 0);
    }

    // Fetch per-object scale/offset from structured buffer
    float4 scaleOffset = g_LightmapScaleOffsets[lightmapIndex];

    // Transform UV2 with per-object scale/offset
    float2 atlasUV = uv2 * scaleOffset.xy + scaleOffset.zw;

    // Sample lightmap (HDR, linear space)
    float3 irradiance = g_Lightmap2D.Sample(samp, atlasUV).rgb;

    return irradiance * lm_intensity;
}
```

### Per-Object Lightmap Index

```cpp
// SMeshRenderer component
struct SMeshRenderer : public CComponent {
    std::string path;
    std::string materialPath;
    int lightmapInfosIndex = -1;  // Index into CLightmap2DManager buffer
    // ...
};

// CB_Object in shader
cbuffer CB_Object : register(b1) {
    // ...
    int gLightmapIndex;  // Per-object lightmap index (-1 = no lightmap)
};
```

### Vertex Format (UV2)

```cpp
// SVertexPNT extended with UV2
struct SVertexPNT {
    float px, py, pz;    // Position
    float nx, ny, nz;    // Normal
    float u, v;          // UV1 (texture)
    float u2, v2;        // UV2 (lightmap)
    // ...
};

// Input layout includes TEXCOORD1
D3D11_INPUT_ELEMENT_DESC layout[] = {
    // ...
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SVertexPNT, u2), ...}
};
```

---

## Lightmap Persistence

### File Format

```
SceneName.lightmap/
├── data.bin      # Binary: SLightmapDataHeader + SLightmapInfo[]
└── atlas.ktx2    # Atlas texture (R16G16B16A16_FLOAT)
```

**data.bin structure:**
```cpp
struct SLightmapDataHeader {
    uint32_t magic = 0x4C4D3244;  // "LM2D"
    uint32_t version = 1;
    uint32_t infoCount = 0;
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    uint32_t reserved[3] = {0, 0, 0};
};
// Followed by: SLightmapInfo[infoCount]
```

### Save/Load System

**CLightmap2DManager** (singleton) manages runtime lightmap data:

```cpp
// Save after baking
CLightmap2DManager::Instance().SaveLightmap(
    "scenes/MyScene.lightmap",
    lightmapInfos,
    atlasTexture
);

// Auto-load when enabling Lightmap2D mode
CLightmap2DManager::Instance().LoadLightmap("scenes/MyScene.lightmap");

// Query
bool isLoaded = CLightmap2DManager::Instance().IsLoaded();
RHI::ITexture* atlas = CLightmap2DManager::Instance().GetAtlasTexture();
RHI::IBuffer* buffer = CLightmap2DManager::Instance().GetScaleOffsetBuffer();
```

**Binding in SceneRenderer:**
```cpp
// t16: Atlas texture
cmdList->SetShaderResource(EShaderStage::Pixel, 16, lightmap2D.GetAtlasTexture());

// t17: ScaleOffset structured buffer
cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 17, lightmap2D.GetScaleOffsetBuffer());

// b7: CB_Lightmap2D (enabled flag + intensity)
CB_Lightmap2D cb{};
cb.enabled = 1;
cb.intensity = 1.0f;
cmdList->SetConstantBufferData(EShaderStage::Pixel, 7, &cb, sizeof(CB_Lightmap2D));
```

---

## Known Issues

### Baking
1. **Atomic float accumulation**: Current shader uses simple store, may have race conditions with multiple samples. Consider using `InterlockedCompareExchange` for proper atomic float add.
2. **Dilation not GPU-accelerated**: Currently placeholder, needs compute shader implementation.

### Runtime
None - persistence and binding fully implemented.

---

## Configuration Reference

### SLightmap2DGPUBakeConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| samplesPerTexel | uint32_t | 64 | Monte Carlo samples per texel |
| maxBounces | uint32_t | 3 | Maximum GI ray bounces |
| skyIntensity | float | 1.0 | Sky light intensity multiplier |
| progressCallback | function | nullptr | Progress reporting callback |

### SLightmapAtlasConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| resolution | int | 1024 | Atlas texture size (square) |
| padding | int | 2 | Pixels between charts |
| texelsPerUnit | int | 16 | Texel density (texels/world unit) |

---

## Performance Characteristics

### GPU Baking

| Scene | Texels | Time | GPU |
|-------|--------|------|-----|
| Simple (1K texels) | 1,024 | ~0.5s | RTX 2060 |
| Medium (64K texels) | 65,536 | ~5s | RTX 2060 |
| Large (256K texels) | 262,144 | ~20s | RTX 2060 |

*Times with 64 samples/texel, 3 bounces*

### Runtime

- Lightmap sampling: < 0.1ms overhead
- Memory: 4 bytes/texel (RGBA8) or 8 bytes/texel (RGBA16F)

---

## Implementation Status

- [x] UV2 generation (xatlas integration)
- [x] Atlas packing (row-based algorithm)
- [x] Barycentric rasterization
- [x] GPU DXR baker (CLightmap2DGPUBaker)
- [x] Finalize compute shader
- [x] **Lightmap persistence (CLightmap2DManager)**
- [x] **Runtime binding (SceneRenderer)**
- [x] **Auto-load on mode switch**
- [x] **Structured buffer for per-object scaleOffset**
- [ ] GPU dilation pass
- [ ] Hot-reload support

---

## References

- [xatlas GitHub](https://github.com/jpcy/xatlas)
- [GPU Gems 2: High-Quality Global Illumination](https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows)
- [UE4 Lightmass Documentation](https://docs.unrealengine.com/4.27/en-US/RenderingAndGraphics/Lightmass/)
- [Bakery GPU Lightmapper](https://geom.io/bakery/wiki/)

---

**Last Updated**: 2025-12-27
