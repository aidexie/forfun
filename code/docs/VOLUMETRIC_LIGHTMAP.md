# Volumetric Lightmap System

**实现日期**: 2025-12-09
**最后更新**: 2025-12-25

---

## Overview

UE4/5 风格的 Volumetric Lightmap，用于高质量 Per-Pixel 漫反射全局光照。

### 核心特性

- **自适应八叉树**: 根据场景几何密度自动细分 Brick
- **两级 GPU 查找**: World Position → Indirection Texture → Brick Atlas
- **L2 球谐编码**: 9 SH 系数 × RGB = 27 个 float
- **硬件 Trilinear**: 3D 纹理采样实现平滑 Per-Pixel 插值
- **Overlap Baking**: 相邻 Brick 边缘采样相同世界位置，消除接缝
- **GPU DXR 烘焙**: 64 voxels 批量 dispatch，GPU Path Tracing

---

## Architecture

### 数据结构

#### SBrick (基本存储单元)

```cpp
// Engine/Rendering/VolumetricLightmap.h
struct SBrick {
    // 八叉树位置（在当前 Level 的整数坐标）
    int treeX, treeY, treeZ;

    // 细分级别（0 = 最粗，越大越精细）
    int level;

    // 在 Atlas 纹理中的位置（Brick 坐标）
    int atlasX, atlasY, atlasZ;

    // 世界空间 AABB
    XMFLOAT3 worldMin, worldMax;

    // SH 数据（4×4×4 = 64 个体素，每个 9 个 RGB 系数）
    std::array<std::array<XMFLOAT3, 9>, 64> shData;

    // Validity data (leak prevention)
    std::array<bool, 64> validity;
};
```

#### SOctreeNode (八叉树节点)

```cpp
struct SOctreeNode {
    XMFLOAT3 boundsMin, boundsMax;  // AABB 边界
    int children[8];                 // 子节点索引 (-1 = 无)
    int brickIndex;                  // 叶子节点指向 Brick (-1 = 非叶子)
    int level;                       // 细分级别
};
```

### GPU 资源

| 资源 | 格式 | 大小 | 用途 |
|------|------|------|------|
| Indirection Texture | R32_UINT | N×N×N | 世界坐标 → Brick 索引 |
| Brick Atlas SH0/SH1/SH2 | R16G16B16A16_FLOAT | M×M×M ×3 | SH 系数存储 |
| Brick Info Buffer | StructuredBuffer | numBricks × 32 bytes | Brick 元数据 |
| Constant Buffer | b6 | 128 bytes | Volume 参数 |

### Constant Buffer (b6)

```hlsl
cbuffer CB_VolumetricLightmap : register(b6) {
    float3 vl_volumeMin;              // Volume AABB 最小点
    float3 vl_volumeMax;              // Volume AABB 最大点
    float3 vl_volumeInvSize;          // 1 / (max - min)
    float3 vl_indirectionInvSize;     // 1 / indirectionResolution
    float3 vl_brickAtlasInvSize;      // 1 / brickAtlasSize
    int vl_indirectionResolution;     // Indirection 纹理分辨率
    int vl_brickAtlasSize;            // Atlas 纹理分辨率
    int vl_maxLevel;                  // 最大八叉树层级
    int vl_enabled;                   // 是否启用
    int vl_brickCount;                // Brick 数量
};
```

---

## Two-Level Lookup Algorithm

**Shader 实现** (`Shader/VolumetricLightmap.hlsl`):

```hlsl
float3 SampleVolumetricLightmap(float3 worldPos, float3 normal) {
    // Step 1: World → Indirection UV
    float3 volumeUV = (worldPos - vl_volumeMin) * vl_volumeInvSize;
    if (any(volumeUV < 0) || any(volumeUV > 1)) return 0;  // Outside volume

    // Step 2: Sample Indirection Texture (Point sampling)
    float3 indirUV = volumeUV * (vl_indirectionResolution - 1) + 0.5;
    uint packedData = g_VLIndirection.Load(int4(indirUV, 0));
    uint brickIndex = packedData & 0xFFFF;

    // Step 3: Get Brick Info
    BrickInfo brick = g_VLBrickInfo[brickIndex];

    // Step 4: Calculate Brick-local UV
    float3 brickUV = (worldPos - brick.worldMin) / (brick.worldMax - brick.worldMin);
    brickUV = saturate(brickUV);

    // Step 5: Calculate Atlas UV (Overlap Baking)
    float3 voxelCoordInBrick = 0.5 + brickUV * (VL_BRICK_SIZE - 1);
    float3 atlasCoord = brick.atlasOffset + voxelCoordInBrick;
    float3 atlasUV = atlasCoord * vl_brickAtlasInvSize;

    // Step 6: Sample SH Atlas (Trilinear)
    float4 sh0 = g_VLAtlasSH0.Sample(g_VLSampler, atlasUV);
    float4 sh1 = g_VLAtlasSH1.Sample(g_VLSampler, atlasUV);
    float4 sh2 = g_VLAtlasSH2.Sample(g_VLSampler, atlasUV);

    // Step 7: Decode SH
    return EvaluateSH9(sh0, sh1, sh2, normal);
}
```

---

## Overlap Baking

### 问题

传统中心采样导致相邻 Brick 边缘不连续

**传统方式** (体素中心采样):
```
Brick A: voxel[3] samples at (0.5 + 3) / 4 = 0.875 of brick
Brick B: voxel[0] samples at (0.5 + 0) / 4 = 0.125 of brick
→ Different world positions → Discontinuity!
```

### 解决方案

**Overlap Baking** (边缘到边缘):
```cpp
// bakeBrick() 中的采样位置计算
float tx = (float)x / (VL_BRICK_SIZE - 1);  // 0, 1/3, 2/3, 1
float ty = (float)y / (VL_BRICK_SIZE - 1);
float tz = (float)z / (VL_BRICK_SIZE - 1);

XMFLOAT3 voxelPos = {
    brick.worldMin.x + tx * brickSize.x,  // voxel[0] at worldMin
    brick.worldMin.y + ty * brickSize.y,  // voxel[3] at worldMax
    brick.worldMin.z + tz * brickSize.z
};
```

### 结果

```
Brick A: voxel[3] samples at brick.worldMax
Brick B: voxel[0] samples at brick.worldMin (= A.worldMax for adjacent bricks)
→ Same world position → C0 Continuity!
```

---

## Baking Backend

### Backend Selection

```cpp
// Engine/Rendering/VolumetricLightmap.h
enum class ELightmapBakeBackend {
    CPU,        // CPathTraceBaker - CPU path tracing (fallback)
    GPU_DXR     // CDXRCubemapBaker - GPU DXR ray tracing (default)
};
```

### SLightmapBakeConfig

```cpp
struct SLightmapBakeConfig {
    ELightmapBakeBackend backend = ELightmapBakeBackend::GPU_DXR;

    // CPU backend settings
    int cpuSamplesPerVoxel = 6144;
    int cpuMaxBounces = 3;

    // GPU backend settings
    int gpuSamplesPerVoxel = 256;
    int gpuAccumulationPasses = 24;
    int gpuMaxBounces = 3;
    float gpuSkyIntensity = 1.0f;

    std::function<void(float)> progressCallback = nullptr;
};
```

---

## DXR GPU Baking (CDXRCubemapBaker)

### Architecture

**Cubemap-Based Ray Tracing** with batched dispatch for optimal GPU utilization.

```
Dispatch: (32, 32, 6 * batchSize) threads
- batchSize = 64 (1 brick)
- 32×32 = 1024 rays per face
- 6 faces per voxel
- Total: 6144 rays per voxel
```

### Performance

| Approach | Sync Points | GPU Utilization |
|----------|-------------|-----------------|
| Per-Voxel | 6400 (100 bricks × 64 voxels) | Low |
| Per-Brick (Batched) | 100 (100 bricks) | High |

**64x fewer GPU→CPU sync points!**

### SDXRCubemapBakeConfig

```cpp
struct SDXRCubemapBakeConfig {
    uint32_t cubemapResolution = 32;    // Per face
    uint32_t batchSize = 64;            // Voxels per dispatch (= 1 brick)
    uint32_t maxBounces = 3;
    float skyIntensity = 1.0f;

    struct SDXRCubemapBakeDebugFlags {
        bool logDispatchInfo = true;
        bool logReadbackResults = true;
        bool exportDebugCubemaps = false;
        bool exportSHToText = false;
        std::string debugExportPath;
    } debug;
};
```

### Shader Constant Buffer

```hlsl
// Shader/DXR/LightmapBakeCubemap.hlsl
cbuffer CB_BatchBakeParams : register(b0) {
    uint g_BatchSize;       // 64
    uint g_MaxBounces;      // 3
    uint g_NumLights;
    float g_SkyIntensity;

    uint g_FrameIndex;      // RNG seeding
    uint g_BrickIndex;      // Debugging
    uint2 g_Padding;
};
```

### Voxel Positions Buffer

```hlsl
// Voxel world positions (t7)
StructuredBuffer<float4> g_VoxelPositions : register(t7);
// float4(worldPos.xyz, validity)
```

### Output Layout

```hlsl
// RayGen shader computes voxel index from dispatch z
uint voxelIdx = idx.z / CUBEMAP_FACES;  // 0-63
uint face = idx.z % CUBEMAP_FACES;       // 0-5

// Output: linear array [voxel0_face0_pixel0, ..., voxel63_face5_pixel1023]
uint outputIndex = voxelIdx * TOTAL_RAYS + face * PIXELS_PER_FACE + y * RES + x;
g_OutputCubemap[outputIndex] = float4(radiance, 1.0);
```

### Baking Flow

```cpp
// CDXRCubemapBaker::BakeVolumetricLightmap
for (size_t brickIdx = 0; brickIdx < bricks.size(); brickIdx++) {
    // 1. Collect 64 voxel world positions
    std::vector<XMFLOAT4> positions;
    for (int voxelIdx = 0; voxelIdx < 64; voxelIdx++) {
        positions.push_back({voxelWorldPos, 1.0f});
    }

    // 2. Upload voxel positions
    UploadVoxelPositions(positions);

    // 3. Single dispatch for entire brick
    DispatchBakeBrick(64, brickIdx, config);
    // cmdList->DispatchRays(32, 32, 6 * 64);

    // 4. Single GPU→CPU readback
    ReadbackBatchCubemaps(64);

    // 5. CPU SH projection for all 64 voxels
    for (int voxelIdx = 0; voxelIdx < 64; voxelIdx++) {
        ProjectCubemapToSH(voxelIdx, brick.shData[voxelIdx]);
    }
}
```

### Debug Outputs

- **Cubemap Export**: `E:/forfun/debug/{TestName}/cubemap_brick{N}_voxel{M}.ktx2`
- **SH Text Export**: `E:/forfun/debug/{TestName}/sh_values.txt`
- **SH Reconstruction**: `E:/forfun/debug/{TestName}/sh_reconstructed_brick0_voxel0.ktx2`

---

## Diffuse GI Mode

**设计理念**: `diffuseGIMode` 是场景级别设置，不属于 VL 系统

### 枚举定义

```cpp
// Engine/SceneLightSettings.h
enum class EDiffuseGIMode : int {
    VolumetricLightmap = 0,  // Per-Pixel GI from baked lightmap
    GlobalIBL = 1,           // Skybox Irradiance (ambient)
    None = 2                 // Disabled (for baking first pass)
};
```

### CB_Frame 集成

```cpp
// Engine/Rendering/SceneRenderer.cpp
struct alignas(16) CB_Frame {
    // ... existing fields ...
    float shadowBias;
    float iblIntensity;
    int diffuseGIMode;  // EDiffuseGIMode
    float _pad4;
};
```

### Shader 使用

```hlsl
// Shader/MainPass.ps.hlsl
if (gDiffuseGIMode == DIFFUSE_GI_VOLUMETRIC_LIGHTMAP) {
    if (IsVolumetricLightmapEnabled()) {
        float3 vlIrradiance = GetVolumetricLightmapDiffuse(worldPos, N);
        diffuseIBL = vlIrradiance * albedo;
    }
} else if (gDiffuseGIMode == DIFFUSE_GI_GLOBAL_IBL) {
    float3 irradiance = gIrradianceArray.Sample(gSamp, float4(N, probeIdx)).rgb;
    diffuseIBL = irradiance * albedo;
}
// else: DIFFUSE_GI_NONE - no diffuse GI
```

---

## Editor Integration

### Scene Light Settings Panel

- **Diffuse GI Mode** dropdown: VolumetricLightmap / GlobalIBL / None
- **Volume Bounds**: Min/Max world coordinates
- **Min Brick Size**: Minimum brick world size (controls octree depth)
- **Bake Backend**: CPU / GPU_DXR
- **Build & Bake**: One-click octree generation + SH baking
- **Show Octree Debug**: Visualize brick wireframes

### Workflow

1. 设置 Volume Bounds 覆盖场景
2. 调整 Min Brick Size（小=更精细，但更多内存）
3. 选择 Bake Backend (GPU_DXR 推荐)
4. 点击 "Build & Bake"
5. 等待烘焙完成（有进度条和 ETA）
6. 切换 Diffuse GI Mode 到 VolumetricLightmap

---

## Known Issues

1. **Descriptor Heap Overflow During Baking**
   - 当前每次 dispatch 创建大量 descriptor
   - 单帧内 bake 多个 brick 会超出 descriptor heap 限制
   - 需要实现 descriptor 复用或分帧烘焙

2. **Edge Discontinuity (边缘不连续)**
   - 边缘 probe 在采样数量不足时方差较大
   - 相邻 brick 边缘 probe 由于 RNG 不同导致结果差异
   - 解决方案：增加采样数量或实现边缘 probe 共享烘焙

3. **室内边缘漏光 (Light Leaking)**
   - 当前只烘焙直接光，没有遮挡信息
   - 需要实现 Visibility/Occlusion 烘焙

---

## File Locations

| 文件 | 用途 |
|------|------|
| `Engine/Rendering/VolumetricLightmap.h` | 核心类定义、SBrick、CB 结构 |
| `Engine/Rendering/VolumetricLightmap.cpp` | 八叉树、烘焙、GPU 资源 |
| `Engine/Rendering/RayTracing/DXRCubemapBaker.h` | DXR 烘焙器定义 |
| `Engine/Rendering/RayTracing/DXRCubemapBaker.cpp` | GPU 批量烘焙实现 |
| `Shader/VolumetricLightmap.hlsl` | GPU 采样算法 |
| `Shader/DXR/LightmapBakeCubemap.hlsl` | DXR 烘焙 shader |
| `Engine/SceneLightSettings.h` | EDiffuseGIMode 枚举 |
| `Editor/Panels_SceneLightSettings.cpp` | 编辑器 UI |
| `Tests/TestDXRBakeVisualize.cpp` | GPU 烘焙测试 |

---

## Constants

```cpp
// VolumetricLightmap.h
static const int VL_BRICK_SIZE = 4;           // 每个 Brick 4×4×4 体素
static const int VL_BRICK_VOXEL_COUNT = 64;   // 4^3 = 64
static const int VL_SH_COEFF_COUNT = 9;       // L2 球谐系数数量
static const int VL_MAX_LEVEL = 8;            // 最大细分级别限制

// DXRCubemapBaker.h
constexpr uint32_t CUBEMAP_BAKE_RES = 32;           // Cubemap face resolution
constexpr uint32_t CUBEMAP_BAKE_FACES = 6;          // Faces per cubemap
constexpr uint32_t CUBEMAP_PIXELS_PER_FACE = 1024;  // 32×32
constexpr uint32_t CUBEMAP_TOTAL_PIXELS = 6144;     // 1024×6 rays per voxel
```

---

**Last Updated**: 2025-12-25
