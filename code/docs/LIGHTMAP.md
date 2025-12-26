# Lightmap Implementation Roadmap

Step-by-step guide for implementing 2D Lightmap baking system (Phase 3.1).

**Goal**: Bake static lighting into UV2 texture space, reusing existing DXR infrastructure.

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
│  Step 4: Baking (DXR Path Tracing)                              │
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

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| UV2 Generation | Static (editor button) | Cache to mesh, avoid runtime cost |
| Storage | Atlas packing | Reduce texture count, single bind |
| Data Format | R16G16B16A16_FLOAT | HDR irradiance, no banding |
| Baking Backend | DXR (GPU) | Reuse existing CDXRCubemapBaker |

---

## Step 1: Integrate xatlas

**Goal**: Generate UV2 coordinates for meshes that don't have them.

### 1.1 Download xatlas

```
Repository: https://github.com/jpcy/xatlas
Location:   E:/forfun/thirdparty/xatlas/
```

xatlas is header-only, just need:
- `xatlas.h`
- `xatlas.cpp`

### 1.2 Add to CMakeLists.txt

```cmake
# In thirdparty section
add_library(xatlas STATIC
    ${THIRDPARTY_DIR}/xatlas/xatlas.cpp
)
target_include_directories(xatlas PUBLIC ${THIRDPARTY_DIR}/xatlas)

# Link to main target
target_link_libraries(forfun PRIVATE xatlas)
```

### 1.3 Create UV2 Generator

**File**: `Engine/Rendering/Lightmap/LightmapUV2.h`

```cpp
#pragma once
#include <vector>
#include <DirectXMath.h>

class CMesh;

// UV2 generation result
struct SUV2GenerationResult {
    bool success = false;
    std::vector<DirectX::XMFLOAT2> uv2;      // Per-vertex UV2
    std::vector<uint32_t> newIndices;         // Re-indexed (xatlas may split vertices)
    std::vector<DirectX::XMFLOAT3> newPositions;
    std::vector<DirectX::XMFLOAT3> newNormals;
    std::vector<DirectX::XMFLOAT2> newUV1;    // Original UV
    int atlasWidth = 0;
    int atlasHeight = 0;
};

// Generate UV2 for a single mesh
SUV2GenerationResult GenerateUV2(
    const std::vector<DirectX::XMFLOAT3>& positions,
    const std::vector<DirectX::XMFLOAT3>& normals,
    const std::vector<DirectX::XMFLOAT2>& uv1,
    const std::vector<uint32_t>& indices,
    int targetTexelDensity = 16  // texels per world unit
);

// Generate UV2 for CMesh (updates mesh in-place)
bool GenerateUV2ForMesh(CMesh& mesh, int targetTexelDensity = 16);
```

### 1.4 Implement UV2 Generator

**File**: `Engine/Rendering/Lightmap/LightmapUV2.cpp`

```cpp
#include "LightmapUV2.h"
#include <xatlas.h>

SUV2GenerationResult GenerateUV2(
    const std::vector<DirectX::XMFLOAT3>& positions,
    const std::vector<DirectX::XMFLOAT3>& normals,
    const std::vector<DirectX::XMFLOAT2>& uv1,
    const std::vector<uint32_t>& indices,
    int targetTexelDensity)
{
    SUV2GenerationResult result;

    // 1. Create xatlas context
    xatlas::Atlas* atlas = xatlas::Create();

    // 2. Add mesh
    xatlas::MeshDecl meshDecl;
    meshDecl.vertexCount = (uint32_t)positions.size();
    meshDecl.vertexPositionData = positions.data();
    meshDecl.vertexPositionStride = sizeof(DirectX::XMFLOAT3);
    meshDecl.vertexNormalData = normals.data();
    meshDecl.vertexNormalStride = sizeof(DirectX::XMFLOAT3);
    meshDecl.indexCount = (uint32_t)indices.size();
    meshDecl.indexData = indices.data();
    meshDecl.indexFormat = xatlas::IndexFormat::UInt32;

    xatlas::AddMeshError error = xatlas::AddMesh(atlas, meshDecl);
    if (error != xatlas::AddMeshError::Success) {
        xatlas::Destroy(atlas);
        return result;
    }

    // 3. Generate UV2
    xatlas::ChartOptions chartOptions;
    chartOptions.maxIterations = 4;

    xatlas::PackOptions packOptions;
    packOptions.texelsPerUnit = (float)targetTexelDensity;
    packOptions.padding = 2;
    packOptions.bilinear = true;

    xatlas::Generate(atlas, chartOptions, packOptions);

    // 4. Extract results
    const xatlas::Mesh& outMesh = atlas->meshes[0];

    result.atlasWidth = atlas->width;
    result.atlasHeight = atlas->height;
    result.newIndices.resize(outMesh.indexCount);
    result.uv2.resize(outMesh.vertexCount);
    result.newPositions.resize(outMesh.vertexCount);
    result.newNormals.resize(outMesh.vertexCount);
    result.newUV1.resize(outMesh.vertexCount);

    for (uint32_t i = 0; i < outMesh.vertexCount; i++) {
        const xatlas::Vertex& v = outMesh.vertexArray[i];
        uint32_t origIdx = v.xref;  // Original vertex index

        result.uv2[i] = {
            v.uv[0] / (float)atlas->width,
            v.uv[1] / (float)atlas->height
        };
        result.newPositions[i] = positions[origIdx];
        result.newNormals[i] = normals[origIdx];
        if (!uv1.empty()) {
            result.newUV1[i] = uv1[origIdx];
        }
    }

    for (uint32_t i = 0; i < outMesh.indexCount; i++) {
        result.newIndices[i] = outMesh.indexArray[i];
    }

    result.success = true;
    xatlas::Destroy(atlas);
    return result;
}
```

### 1.5 Extend CMesh to Store UV2

**File**: `Core/Mesh.h` - Add UV2 storage

```cpp
class CMesh {
    // ... existing members ...

    // UV2 for lightmapping (optional)
    std::vector<DirectX::XMFLOAT2> m_uv2;
    bool m_hasUV2 = false;

public:
    bool HasUV2() const { return m_hasUV2; }
    const std::vector<DirectX::XMFLOAT2>& GetUV2() const { return m_uv2; }
    void SetUV2(const std::vector<DirectX::XMFLOAT2>& uv2) {
        m_uv2 = uv2;
        m_hasUV2 = true;
    }
};
```

### 1.6 Test: Verify UV2 Generation

Create `TestLightmapUV2`:
1. Load a simple mesh (cube or plane)
2. Call `GenerateUV2()`
3. Verify UV2 coordinates are in [0,1] range
4. Verify no overlapping triangles in UV space

---

## Step 2: Atlas Packing

**Goal**: Pack multiple meshes into a single lightmap atlas.

### 2.1 Data Structures

**File**: `Engine/Rendering/Lightmap/LightmapAtlas.h`

```cpp
#pragma once
#include <vector>
#include <DirectXMath.h>

// Per-object lightmap info (stored in MeshRenderer)
struct SLightmapInfo {
    int lightmapIndex = -1;                    // Which atlas (-1 = none)
    DirectX::XMFLOAT4 scaleOffset = {1,1,0,0}; // xy: scale, zw: offset
};

// Atlas packing entry
struct SAtlasEntry {
    int meshRendererIndex;     // Which MeshRenderer
    int atlasX, atlasY;        // Position in atlas (pixels)
    int width, height;         // Size in atlas (pixels)
};

// Atlas configuration
struct SLightmapAtlasConfig {
    int resolution = 1024;     // Atlas texture size
    int padding = 2;           // Pixels between charts
};

class CLightmapAtlas {
public:
    // Pack meshes into atlas, returns false if doesn't fit
    bool Pack(
        const std::vector<std::pair<int, int>>& meshSizes,  // (width, height) per mesh
        const SLightmapAtlasConfig& config
    );

    // Get result
    const std::vector<SAtlasEntry>& GetEntries() const { return m_entries; }
    int GetAtlasCount() const { return m_atlasCount; }

private:
    std::vector<SAtlasEntry> m_entries;
    int m_atlasCount = 0;
};
```

### 2.2 Simple Row Packing Algorithm

```cpp
bool CLightmapAtlas::Pack(
    const std::vector<std::pair<int, int>>& meshSizes,
    const SLightmapAtlasConfig& config)
{
    m_entries.clear();
    m_entries.resize(meshSizes.size());

    int currentX = 0;
    int currentY = 0;
    int rowHeight = 0;
    int atlasIndex = 0;

    for (size_t i = 0; i < meshSizes.size(); i++) {
        int w = meshSizes[i].first + config.padding;
        int h = meshSizes[i].second + config.padding;

        // Check if fits in current row
        if (currentX + w > config.resolution) {
            // Move to next row
            currentX = 0;
            currentY += rowHeight;
            rowHeight = 0;
        }

        // Check if fits in current atlas
        if (currentY + h > config.resolution) {
            // Need new atlas
            atlasIndex++;
            currentX = 0;
            currentY = 0;
            rowHeight = 0;
        }

        m_entries[i].meshRendererIndex = (int)i;
        m_entries[i].atlasX = currentX;
        m_entries[i].atlasY = currentY;
        m_entries[i].width = meshSizes[i].first;
        m_entries[i].height = meshSizes[i].second;

        currentX += w;
        rowHeight = std::max(rowHeight, h);
    }

    m_atlasCount = atlasIndex + 1;
    return true;
}
```

### 2.3 Compute Scale/Offset

```cpp
SLightmapInfo ComputeLightmapInfo(
    const SAtlasEntry& entry,
    int atlasResolution)
{
    SLightmapInfo info;
    info.lightmapIndex = 0;  // Assuming single atlas for now

    float invRes = 1.0f / (float)atlasResolution;
    info.scaleOffset.x = entry.width * invRes;   // scale U
    info.scaleOffset.y = entry.height * invRes;  // scale V
    info.scaleOffset.z = entry.atlasX * invRes;  // offset U
    info.scaleOffset.w = entry.atlasY * invRes;  // offset V

    return info;
}
```

---

## Step 3: Rasterization

**Goal**: Convert triangles to texel data (worldPos + normal).

### 3.1 Data Structures

**File**: `Engine/Rendering/Lightmap/LightmapRasterizer.h`

```cpp
#pragma once
#include <vector>
#include <DirectXMath.h>

struct STexelData {
    DirectX::XMFLOAT3 worldPos = {0, 0, 0};
    DirectX::XMFLOAT3 normal = {0, 0, 0};
    bool valid = false;
};

class CLightmapRasterizer {
public:
    // Rasterize mesh triangles to lightmap space
    void Rasterize(
        const std::vector<DirectX::XMFLOAT3>& positions,
        const std::vector<DirectX::XMFLOAT3>& normals,
        const std::vector<DirectX::XMFLOAT2>& uv2,
        const std::vector<uint32_t>& indices,
        const DirectX::XMMATRIX& worldMatrix,
        int texWidth, int texHeight,
        int offsetX, int offsetY      // Atlas offset
    );

    // Get texel data
    const std::vector<STexelData>& GetTexels() const { return m_texels; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    void rasterizeTriangle(
        const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2,
        const DirectX::XMFLOAT3& n0, const DirectX::XMFLOAT3& n1, const DirectX::XMFLOAT3& n2,
        const DirectX::XMFLOAT2& uv0, const DirectX::XMFLOAT2& uv1, const DirectX::XMFLOAT2& uv2,
        int offsetX, int offsetY
    );

    std::vector<STexelData> m_texels;
    int m_width = 0;
    int m_height = 0;
};
```

### 3.2 Barycentric Rasterization

```cpp
void CLightmapRasterizer::rasterizeTriangle(
    const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2,
    const XMFLOAT3& n0, const XMFLOAT3& n1, const XMFLOAT3& n2,
    const XMFLOAT2& uv0, const XMFLOAT2& uv1, const XMFLOAT2& uv2,
    int offsetX, int offsetY)
{
    // Convert UV to pixel coordinates
    float x0 = uv0.x * m_width + offsetX;
    float y0 = uv0.y * m_height + offsetY;
    float x1 = uv1.x * m_width + offsetX;
    float y1 = uv1.y * m_height + offsetY;
    float x2 = uv2.x * m_width + offsetX;
    float y2 = uv2.y * m_height + offsetY;

    // Bounding box
    int minX = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
    int maxX = std::min(m_width - 1, (int)std::ceil(std::max({x0, x1, x2})));
    int minY = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
    int maxY = std::min(m_height - 1, (int)std::ceil(std::max({y0, y1, y2})));

    // Precompute barycentric denominator
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::abs(denom) < 1e-8f) return;  // Degenerate triangle
    float invDenom = 1.0f / denom;

    // Rasterize
    for (int py = minY; py <= maxY; py++) {
        for (int px = minX; px <= maxX; px++) {
            float cx = px + 0.5f;  // Texel center
            float cy = py + 0.5f;

            // Barycentric coordinates
            float lambda0 = ((y1 - y2) * (cx - x2) + (x2 - x1) * (cy - y2)) * invDenom;
            float lambda1 = ((y2 - y0) * (cx - x2) + (x0 - x2) * (cy - y2)) * invDenom;
            float lambda2 = 1.0f - lambda0 - lambda1;

            // Inside triangle?
            if (lambda0 >= 0 && lambda1 >= 0 && lambda2 >= 0) {
                int idx = py * m_width + px;

                // Interpolate world position
                m_texels[idx].worldPos.x = lambda0 * p0.x + lambda1 * p1.x + lambda2 * p2.x;
                m_texels[idx].worldPos.y = lambda0 * p0.y + lambda1 * p1.y + lambda2 * p2.y;
                m_texels[idx].worldPos.z = lambda0 * p0.z + lambda1 * p1.z + lambda2 * p2.z;

                // Interpolate and normalize normal
                float nx = lambda0 * n0.x + lambda1 * n1.x + lambda2 * n2.x;
                float ny = lambda0 * n0.y + lambda1 * n1.y + lambda2 * n2.y;
                float nz = lambda0 * n0.z + lambda1 * n1.z + lambda2 * n2.z;
                float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                m_texels[idx].normal = {nx/len, ny/len, nz/len};

                m_texels[idx].valid = true;
            }
        }
    }
}
```

---

## Step 4: Baking (DXR Path Tracing)

**Goal**: Compute irradiance for each valid texel.

### 4.1 Reuse Existing Infrastructure

Your existing `CPathTraceBaker` and `CDXRCubemapBaker` can be adapted:

```cpp
// For each valid texel, trace rays and accumulate irradiance
void BakeLightmap(
    const std::vector<STexelData>& texels,
    std::vector<DirectX::XMFLOAT4>& outIrradiance,
    CScene& scene,
    const SLightmapBakeConfig& config)
{
    outIrradiance.resize(texels.size());

    // Initialize ray tracer / DXR baker
    // ... (reuse existing code)

    for (size_t i = 0; i < texels.size(); i++) {
        if (!texels[i].valid) {
            outIrradiance[i] = {0, 0, 0, 0};
            continue;
        }

        DirectX::XMFLOAT3 irradiance = {0, 0, 0};

        // Monte Carlo integration
        for (int s = 0; s < config.samplesPerTexel; s++) {
            // Sample cosine-weighted hemisphere
            DirectX::XMFLOAT3 dir = SampleCosineHemisphere(texels[i].normal);

            // Path trace
            DirectX::XMFLOAT3 radiance = PathTrace(
                texels[i].worldPos,
                dir,
                scene,
                config.maxBounces
            );

            // Accumulate (cosine term cancels with PDF)
            irradiance.x += radiance.x;
            irradiance.y += radiance.y;
            irradiance.z += radiance.z;
        }

        // Average and scale by PI (from cosine-weighted sampling)
        float scale = 3.14159f / (float)config.samplesPerTexel;
        outIrradiance[i] = {
            irradiance.x * scale,
            irradiance.y * scale,
            irradiance.z * scale,
            1.0f
        };
    }
}
```

### 4.2 GPU Acceleration (Optional Enhancement)

For large lightmaps, consider batching texels and using compute shaders:

```
1. Upload texel worldPos/normal to GPU buffer
2. Dispatch compute shader that traces rays per texel
3. Readback irradiance results
```

This can reuse your existing DXR acceleration structure.

---

## Step 5: Post-Processing (Dilation)

**Goal**: Fill invalid texels to prevent seam bleeding.

### 5.1 Dilation Algorithm

```cpp
void DilateLightmap(
    std::vector<DirectX::XMFLOAT4>& irradiance,
    const std::vector<STexelData>& texels,
    int width, int height,
    int dilationRadius = 4)
{
    std::vector<DirectX::XMFLOAT4> dilated = irradiance;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            if (texels[idx].valid) continue;  // Already valid

            // Search for nearest valid neighbor
            bool found = false;
            for (int r = 1; r <= dilationRadius && !found; r++) {
                float accumR = 0, accumG = 0, accumB = 0;
                int count = 0;

                for (int dy = -r; dy <= r; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        if (std::abs(dx) != r && std::abs(dy) != r) continue;

                        int nx = x + dx;
                        int ny = y + dy;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;

                        int nidx = ny * width + nx;
                        if (texels[nidx].valid) {
                            accumR += irradiance[nidx].x;
                            accumG += irradiance[nidx].y;
                            accumB += irradiance[nidx].z;
                            count++;
                        }
                    }
                }

                if (count > 0) {
                    dilated[idx] = {
                        accumR / count,
                        accumG / count,
                        accumB / count,
                        1.0f
                    };
                    found = true;
                }
            }
        }
    }

    irradiance = dilated;
}
```

---

## Step 6: Runtime Sampling

### 6.1 Extend MeshRenderer

**File**: `Engine/Components/MeshRenderer.h`

```cpp
struct SMeshRenderer : public IComponent {
    // ... existing members ...

    // Lightmap info
    SLightmapInfo lightmapInfo;
    bool useLightmap = false;

    void VisitProperties(IPropertyVisitor& visitor) override {
        // ... existing ...
        visitor.VisitBool("useLightmap", useLightmap);
    }
};
```

### 6.2 Shader Modifications

**File**: `Shader/ForwardLit.hlsl`

```hlsl
// New resources
Texture2D<float4> g_LightmapAtlas : register(t10);
SamplerState g_LightmapSampler : register(s3);

// New constant buffer (per-object)
cbuffer CB_Lightmap : register(b4) {
    float4 g_LightmapScaleOffset;  // xy: scale, zw: offset
    int g_UseLightmap;
    int3 _pad;
};

// Vertex shader - pass through UV2
struct VS_INPUT {
    // ... existing ...
    float2 uv2 : TEXCOORD1;  // UV2 for lightmap
};

struct VS_OUTPUT {
    // ... existing ...
    float2 lightmapUV : TEXCOORD2;
};

VS_OUTPUT VS_Main(VS_INPUT input) {
    VS_OUTPUT output;
    // ... existing ...
    output.lightmapUV = input.uv2;
    return output;
}

// Pixel shader - sample lightmap
float4 PS_Main(VS_OUTPUT input) : SV_TARGET {
    // ... existing PBR calculations ...

    // Lightmap GI
    float3 bakedGI = float3(0, 0, 0);
    if (g_UseLightmap) {
        float2 atlasUV = input.lightmapUV * g_LightmapScaleOffset.xy
                       + g_LightmapScaleOffset.zw;
        bakedGI = g_LightmapAtlas.Sample(g_LightmapSampler, atlasUV).rgb;
    }

    // Replace or blend with existing diffuse GI
    float3 diffuseGI = g_UseLightmap ? bakedGI : existingGI;

    // ... rest of shading ...
}
```

### 6.3 Vertex Buffer Layout

Need to include UV2 in vertex buffer. Options:

**Option A**: Interleaved (modify existing layout)
```cpp
struct SVertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 uv1;
    XMFLOAT2 uv2;  // Add this
    // ...
};
```

**Option B**: Separate stream (less intrusive)
```cpp
// Bind UV2 as separate vertex buffer at slot 1
// D3D11_INPUT_ELEMENT_DESC for UV2:
{"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}
```

---

## Step 7: Editor Integration

### 7.1 Scene Light Settings Panel

Add to `Editor/Panels_SceneLightSettings.cpp`:

```cpp
// In SceneLightSettings panel
if (ImGui::CollapsingHeader("Lightmap")) {
    static int resolution = 1024;
    static int samplesPerTexel = 64;

    ImGui::SliderInt("Resolution", &resolution, 256, 4096);
    ImGui::SliderInt("Samples", &samplesPerTexel, 16, 256);

    if (ImGui::Button("Generate UV2 for Scene")) {
        // Call UV2 generation for all static meshes
    }

    if (ImGui::Button("Bake Lightmap")) {
        // Trigger baking pipeline
    }

    // Progress bar during baking
    if (isBaking) {
        ImGui::ProgressBar(bakeProgress);
    }
}
```

---

## File Structure Summary

```
Engine/Rendering/Lightmap/
├── Lightmap.h                 // Main header, includes all
├── LightmapTypes.h            // Data structures
├── LightmapUV2.h/cpp          // xatlas UV2 generation
├── LightmapAtlas.h/cpp        // Atlas packing
├── LightmapRasterizer.h/cpp   // Triangle rasterization
├── LightmapBaker.h/cpp        // Baking orchestration
└── LightmapPostProcess.h/cpp  // Dilation, filtering

Shader/
├── ForwardLit.hlsl            // Modified for lightmap sampling
└── LightmapBake.hlsl          // (Optional) GPU baking compute shader

Core/Testing/Tests/
└── TestLightmap.cpp           // Validation test
```

---

## Implementation Order

```
Week 1:
  [x] 1.1-1.4: xatlas integration
  [ ] 1.5: CMesh UV2 storage
  [ ] 1.6: TestLightmapUV2

Week 2:
  [ ] 2.1-2.3: Atlas packing
  [ ] 3.1-3.2: Rasterization

Week 3:
  [ ] 4.1-4.2: Baking (CPU first, then GPU)
  [ ] 5.1: Dilation

Week 4:
  [ ] 6.1-6.3: Runtime sampling
  [ ] 7.1: Editor UI
  [ ] TestLightmap full validation
```

---

## Validation Checklist

- [ ] UV2 coordinates in [0,1] range
- [ ] No UV2 triangle overlap
- [ ] Atlas packing fits all meshes
- [ ] Rasterized texels have valid worldPos/normal
- [ ] Baked irradiance is non-negative
- [ ] No visible seams after dilation
- [ ] Lightmap matches direct lighting in simple scene
- [ ] Performance: < 1ms runtime sampling overhead

---

## References

- [xatlas GitHub](https://github.com/jpcy/xatlas)
- [GPU Gems 2: High-Quality Global Illumination](https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows)
- [UE4 Lightmass Documentation](https://docs.unrealengine.com/4.27/en-US/RenderingAndGraphics/Lightmass/)
- [Bakery GPU Lightmapper](https://geom.io/bakery/wiki/)

---

**Last Updated**: 2025-12-26
