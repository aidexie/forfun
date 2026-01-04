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
│  Step 5: Dilation (GPU compute shader)                          │
│     Input:  Raw lightmap with holes                             │
│     Output: Dilated lightmap, no seam bleeding                  │
│                                                                 │
│  Step 6: OIDN Denoise (CPU, optional)                           │
│     Input:  Noisy lightmap                                      │
│     Output: Clean, denoised lightmap                            │
│                                                                 │
│  Step 7: Runtime Sampling                                       │
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
├── Lightmap2DManager.h/cpp    # Runtime lightmap data manager
└── LightmapDenoiser.h/cpp     # Intel OIDN wrapper

Shader/
├── DXR/Lightmap2DBake.hlsl    # DXR ray tracing shader
├── Lightmap2DFinalize.cs.hlsl # Compute shader for finalization
├── Lightmap2DDilate.cs.hlsl   # GPU dilation compute shader
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
| Denoising | Intel OIDN | AI-based, production quality |

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

┌─────────────────────────────────────────────────────────────────┐
│ Pass 3: Dilation (Lightmap2DDilate.cs.hlsl)                     │
├─────────────────────────────────────────────────────────────────┤
│ Multiple passes with ping-pong textures                         │
│                                                                 │
│ CSMain:                                                         │
│   1. Check if current texel is empty (alpha == 0)               │
│   2. Sample 8 neighbors, find valid pixels                      │
│   3. Average valid neighbors to fill gap                        │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Pass 4: OIDN Denoise (CPU, optional)                            │
├─────────────────────────────────────────────────────────────────┤
│ GPU → CPU readback → OIDN → CPU → GPU upload                    │
│                                                                 │
│ Steps:                                                          │
│   1. Copy GPU texture to staging buffer                         │
│   2. Convert R16G16B16A16_FLOAT to float3 RGB                   │
│   3. Call OIDN RTLightmap filter                                │
│   4. Convert back to R16G16B16A16_FLOAT                         │
│   5. Upload to GPU texture                                      │
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
config.samplesPerTexel = 64;       // Monte Carlo samples
config.maxBounces = 3;             // GI bounces
config.skyIntensity = 1.0f;
config.enableDenoiser = true;      // Intel OIDN denoising
config.debugExportImages = false;  // Export debug KTX2 images
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

## Intel OIDN Denoising

### Overview

Intel Open Image Denoise (OIDN) is integrated as an optional post-processing step after GPU baking. It uses AI-based denoising to remove Monte Carlo noise from path-traced lightmaps.

### CLightmapDenoiser API

```cpp
class CLightmapDenoiser {
public:
    bool Initialize();
    void Shutdown();
    bool IsReady() const;

    // Denoise in-place (RGB float buffer)
    bool Denoise(
        float* colorBuffer,     // width * height * 3 floats (RGB)
        int width,
        int height,
        float* normalBuffer = nullptr,  // Optional auxiliary
        float* albedoBuffer = nullptr   // Optional auxiliary
    );

    const char* GetLastError() const;
};
```

### Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| enableDenoiser | bool | true | Enable Intel OIDN denoising |
| debugExportImages | bool | false | Export before/after KTX2 debug images |

### Performance

- OIDN 2.x achieves ~98% noise reduction on typical lightmaps
- CPU-based processing (~100-500ms for 1024x1024 atlas)
- No GPU vendor lock-in (works on any x64 CPU with SSE4.1)

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

### Save/Load API

```cpp
// Load lightmap from disk
scene.GetLightmap2D().LoadLightmap("scenes/MyScene.lightmap");

// Direct data transfer from baker (no file I/O)
scene.GetLightmap2D().SetBakedData(atlasTexture, lightmapInfos);

// Query state
bool isLoaded = scene.GetLightmap2D().IsLoaded();
RHI::ITexture* atlas = scene.GetLightmap2D().GetAtlasTexture();

// Hot-reload
scene.GetLightmap2D().ReloadLightmap();
```

---

## Configuration Reference

### SLightmap2DBakeConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| samplesPerTexel | int | 64 | Monte Carlo samples per texel |
| maxBounces | int | 3 | Maximum GI ray bounces |
| skyIntensity | float | 1.0 | Sky light intensity multiplier |
| useGPU | bool | true | Use DXR GPU baking if available |
| enableDenoiser | bool | true | Enable Intel OIDN denoising |
| debugExportImages | bool | false | Export debug KTX2 images |

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

*Times with 64 samples/texel, 3 bounces, OIDN enabled*

### Runtime

- Lightmap sampling: < 0.1ms overhead
- Memory: 8 bytes/texel (R16G16B16A16_FLOAT)

---

## Implementation Status

- [x] UV2 generation (xatlas integration)
- [x] Atlas packing (row-based algorithm)
- [x] Barycentric rasterization
- [x] GPU DXR baker (CLightmap2DGPUBaker)
- [x] Finalize compute shader
- [x] GPU dilation pass
- [x] Intel OIDN denoising
- [x] Lightmap persistence (CLightmap2DManager)
- [x] Runtime binding (SceneRenderer)
- [x] Auto-load on mode switch
- [x] Hot-reload support
- [x] Debug image export (KTX2)

---

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| xatlas | - | UV2 generation and atlas packing |
| Intel OIDN | 2.x | AI-based denoising |
| KTX-Software | - | KTX2 texture export |

---

## References

- [xatlas GitHub](https://github.com/jpcy/xatlas)
- [Intel Open Image Denoise](https://www.openimagedenoise.org/)
- [GPU Gems 2: High-Quality Global Illumination](https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows)
- [UE4 Lightmass Documentation](https://docs.unrealengine.com/4.27/en-US/RenderingAndGraphics/Lightmass/)

---

**Last Updated**: 2026-01-04
