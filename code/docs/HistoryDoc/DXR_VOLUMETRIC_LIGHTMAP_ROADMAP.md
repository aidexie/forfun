# DXR Volumetric Lightmap Baking Roadmap

## Overview

Migration plan: CPU Path Tracing → GPU DXR Path Tracing for Volumetric Lightmap baking.

**Current State**: CPU-based `CPathTraceBaker` with AABB-only BVH, ~6144 samples/voxel, 3 bounces
**Target State**: GPU DXR-based baking with triangle-level accuracy, massive parallelism

**Expected Speedup**: 100x-1000x (minutes → seconds)

---

## Architecture Comparison

### Current (CPU)
```
VolumetricLightmap::BakeAllBricks()
    ↓
CPathTraceBaker::BakeVoxelWithValidity()
    ↓
CRayTracer (CPU BVH, AABB-only)
    ↓
Sequential: brick-by-brick, voxel-by-voxel, sample-by-sample
```

### Target (GPU DXR)
```
VolumetricLightmap::BakeAllBricks()
    ↓
CDXRLightmapBaker::BakeAllVoxels()  // Single GPU dispatch
    ↓
DXR Pipeline (BLAS/TLAS, Triangle-level)
    ↓
Parallel: ALL voxels × ALL samples simultaneously
```

---

## Phase 0: Prerequisites & Research (1-2 days)

### 0.1 Verify DXR Support
- [ ] Check `ID3D12Device5` availability
- [ ] Query `D3D12_RAYTRACING_TIER` (need Tier 1.0 minimum)
- [ ] Add fallback path for non-RTX hardware (keep CPU baker)

### 0.2 Study DXR Concepts
- [ ] BLAS (Bottom-Level Acceleration Structure) - per-mesh
- [ ] TLAS (Top-Level Acceleration Structure) - scene instances
- [ ] Ray Generation Shader - entry point
- [ ] Closest Hit Shader - surface interaction
- [ ] Miss Shader - skybox/ambient
- [ ] Any Hit Shader - alpha testing (optional)

### 0.3 Reference Materials
- Microsoft DXR spec: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
- NVIDIA DXR Tutorial: https://developer.nvidia.com/rtx/raytracing/dxr/dx12-raytracing-tutorial-part-1
- AMD GPUOpen samples

---

## Phase 1: RHI DXR Foundation (3-5 days)

### 1.1 New RHI Interfaces

**File**: `RHI/RHIRayTracing.h`

```cpp
// Acceleration Structure
struct AccelerationStructureDesc {
    enum class EType { BottomLevel, TopLevel };
    EType type;
    // BLAS: geometry data
    // TLAS: instance data
};

class IAccelerationStructure {
public:
    virtual ~IAccelerationStructure() = default;
    virtual uint64_t GetGPUAddress() const = 0;
};

// Ray Tracing Pipeline
struct RayTracingPipelineDesc {
    IShader* rayGenShader;
    IShader* missShader;
    IShader* closestHitShader;
    IShader* anyHitShader;  // Optional
    uint32_t maxPayloadSize;
    uint32_t maxAttributeSize;
    uint32_t maxRecursionDepth;
};

class IRayTracingPipelineState {
public:
    virtual ~IRayTracingPipelineState() = default;
    virtual void* GetShaderIdentifier(const char* exportName) = 0;
};

// Shader Binding Table
struct ShaderBindingTableDesc {
    IRayTracingPipelineState* pipeline;
    // Entry sizes and counts
};

class IShaderBindingTable {
public:
    virtual ~IShaderBindingTable() = default;
    virtual D3D12_GPU_VIRTUAL_ADDRESS GetRayGenAddress() const = 0;
    virtual D3D12_GPU_VIRTUAL_ADDRESS GetMissAddress() const = 0;
    virtual D3D12_GPU_VIRTUAL_ADDRESS GetHitGroupAddress() const = 0;
};
```

**Extend IRenderContext**:
```cpp
class IRenderContext {
    // ... existing ...

    // DXR support query
    virtual bool SupportsRayTracing() const = 0;
    virtual int GetRayTracingTier() const = 0;

    // Acceleration structure
    virtual IAccelerationStructure* CreateBLAS(const BLASDesc& desc) = 0;
    virtual IAccelerationStructure* CreateTLAS(const TLASDesc& desc) = 0;

    // Ray tracing pipeline
    virtual IRayTracingPipelineState* CreateRayTracingPipeline(const RayTracingPipelineDesc& desc) = 0;
    virtual IShaderBindingTable* CreateShaderBindingTable(const ShaderBindingTableDesc& desc) = 0;
};
```

**Extend ICommandList**:
```cpp
class ICommandList {
    // ... existing ...

    // Acceleration structure commands
    virtual void BuildAccelerationStructure(IAccelerationStructure* as) = 0;

    // Ray tracing dispatch
    virtual void DispatchRays(const DispatchRaysDesc& desc) = 0;
};
```

### 1.2 DX12 Implementation

**Files**:
- `RHI/DX12/DX12AccelerationStructure.h/cpp`
- `RHI/DX12/DX12RayTracingPipeline.h/cpp`
- `RHI/DX12/DX12ShaderBindingTable.h/cpp`

**Key Implementation Points**:

```cpp
// BLAS Creation
class CDX12AccelerationStructure : public IAccelerationStructure {
    ComPtr<ID3D12Resource> m_scratch;
    ComPtr<ID3D12Resource> m_result;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS m_buildFlags;

public:
    void Build(ID3D12GraphicsCommandList4* cmdList,
               const D3D12_RAYTRACING_GEOMETRY_DESC* geometries,
               uint32_t geometryCount);
};

// TLAS Instance Buffer
struct D3D12_RAYTRACING_INSTANCE_DESC {
    float Transform[3][4];              // Row-major 3x4 transform
    uint32_t InstanceID : 24;
    uint32_t InstanceMask : 8;
    uint32_t InstanceContributionToHitGroupIndex : 24;
    uint32_t Flags : 8;
    D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructure;  // BLAS address
};
```

### 1.3 DX11 Stub Implementation

```cpp
// DX11 doesn't support DXR - return nullptr/false
bool CDX11RenderContext::SupportsRayTracing() const { return false; }
IAccelerationStructure* CDX11RenderContext::CreateBLAS(...) { return nullptr; }
// ... etc
```

---

## Phase 2: Scene Geometry Export (2-3 days)

### 2.1 Triangle Data Access

Current `CRayTracer` uses AABB only. Need actual triangles for DXR.

**New Interface**: `RHI/RHIGeometryExport.h`

```cpp
struct SExportedMesh {
    std::vector<XMFLOAT3> positions;    // Vertex positions
    std::vector<uint32_t> indices;       // Triangle indices
    XMFLOAT4X4 worldTransform;           // Instance transform

    // Material data for shading
    XMFLOAT3 albedo;
    float metallic;
    float roughness;
    int materialIndex;
};

struct SExportedScene {
    std::vector<SExportedMesh> meshes;
    std::vector<SLightData> lights;      // Directional, Point, Spot
    // Skybox cubemap handle
};
```

### 2.2 Mesh Data Extraction

**File**: `Engine/Rendering/RayTracing/SceneGeometryExporter.h/cpp`

```cpp
class CSceneGeometryExporter {
public:
    // Extract all mesh data from scene
    SExportedScene ExportScene(CScene& scene);

private:
    // Access MeshResourceManager to get vertex/index data
    void extractMeshData(SMeshRenderer* meshRenderer, SExportedMesh& out);
};
```

**Challenge**: Current `CMeshResourceManager` stores GPU buffers only.
**Solution Options**:
1. Keep CPU copy of mesh data (memory cost)
2. Readback GPU buffers (slow, one-time)
3. Store positions separately for RT (recommended)

### 2.3 GPU Buffer Layout for DXR

```cpp
// Per-mesh BLAS input
struct SBLASInput {
    RHI::IBuffer* vertexBuffer;      // XMFLOAT3 positions
    RHI::IBuffer* indexBuffer;       // uint32_t indices
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t vertexStride;
};

// Scene-wide data for shaders
struct SGPUSceneData {
    RHI::IBuffer* instanceDataBuffer;   // Per-instance transforms + material IDs
    RHI::IBuffer* materialBuffer;        // Material properties
    RHI::IBuffer* lightBuffer;           // Light data
    RHI::ITexture* skyboxCubemap;        // Environment map
};
```

---

## Phase 3: BLAS/TLAS Management (3-4 days)

### 3.1 BLAS Builder

**File**: `Engine/Rendering/RayTracing/DXRAccelerationStructureManager.h/cpp`

```cpp
class CDXRAccelerationStructureManager {
public:
    bool Initialize();
    void Shutdown();

    // Build BLAS for each unique mesh
    // Returns handle for TLAS reference
    int AddMesh(const SBLASInput& input);

    // Build TLAS from instances
    void BuildTLAS(const std::vector<SInstanceDesc>& instances);

    // Access for shader binding
    IAccelerationStructure* GetTLAS() const;

private:
    std::vector<std::unique_ptr<IAccelerationStructure>> m_blasList;
    std::unique_ptr<IAccelerationStructure> m_tlas;

    // Scratch buffer management
    RHI::BufferPtr m_scratchBuffer;
    size_t m_scratchSize = 0;
};
```

### 3.2 Instance Management

```cpp
struct SInstanceDesc {
    int blasIndex;                // Which BLAS to instance
    XMFLOAT4X4 transform;        // World transform
    uint32_t instanceID;          // Custom ID (material index, etc.)
    uint32_t hitGroupIndex;       // Shader table offset
};
```

### 3.3 Build Pipeline

```
Scene Load / Change
    ↓
1. Export geometry from MeshRenderers
    ↓
2. Build/Update BLAS for each unique mesh
    ↓
3. Collect instance transforms
    ↓
4. Build TLAS
    ↓
Ready for ray tracing
```

---

## Phase 4: DXR Shaders (4-5 days)

### 4.1 Shader Structure

**File**: `Shader/DXR/LightmapBake.hlsl`

```hlsl
// ============================================
// Global Resources
// ============================================
RaytracingAccelerationStructure g_Scene : register(t0);
TextureCube<float4> g_Skybox : register(t1);
StructuredBuffer<SMaterialData> g_Materials : register(t2);
StructuredBuffer<SLightData> g_Lights : register(t3);
StructuredBuffer<SInstanceData> g_Instances : register(t4);

// Output: SH coefficients per voxel
RWTexture3D<float4> g_OutputSH0 : register(u0);
RWTexture3D<float4> g_OutputSH1 : register(u1);
RWTexture3D<float4> g_OutputSH2 : register(u2);

// Parameters
cbuffer CB_BakeParams : register(b0) {
    float3 volumeMin;
    float3 volumeMax;
    uint3 voxelGridSize;       // Total voxels to bake
    uint samplesPerVoxel;
    uint maxBounces;
    uint frameIndex;           // For RNG seed
};

// ============================================
// Ray Payload
// ============================================
struct SRayPayload {
    float3 radiance;
    float3 throughput;
    float3 nextOrigin;
    float3 nextDirection;
    uint bounceCount;
    uint rngState;
    bool terminated;
};

// ============================================
// Ray Generation Shader
// ============================================
[shader("raygeneration")]
void RayGen() {
    // Get voxel index from dispatch ID
    uint3 voxelIdx = DispatchRaysIndex().xyz;

    // Compute world position
    float3 worldPos = VoxelIndexToWorldPos(voxelIdx);

    // Initialize SH accumulators
    float3 shCoeffs[9] = (float3)0;

    // Initialize RNG
    uint rngState = InitRNG(voxelIdx, frameIndex);

    // Sample hemisphere
    for (uint s = 0; s < samplesPerVoxel; s++) {
        // Generate uniform sphere direction
        float3 dir = SampleSphereUniform(rngState);

        // Initialize payload
        SRayPayload payload;
        payload.radiance = 0;
        payload.throughput = 1;
        payload.nextOrigin = worldPos;
        payload.nextDirection = dir;
        payload.bounceCount = 0;
        payload.rngState = rngState;
        payload.terminated = false;

        // Path trace
        while (!payload.terminated && payload.bounceCount < maxBounces) {
            RayDesc ray;
            ray.Origin = payload.nextOrigin;
            ray.Direction = payload.nextDirection;
            ray.TMin = 0.001;
            ray.TMax = 10000.0;

            TraceRay(g_Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
        }

        // Accumulate to SH
        AccumulateToSH(dir, payload.radiance, shCoeffs);

        rngState = payload.rngState;
    }

    // Normalize and store
    float weight = 4.0 * PI / samplesPerVoxel;
    for (int i = 0; i < 9; i++) {
        shCoeffs[i] *= weight;
    }

    // Pack to output textures
    g_OutputSH0[voxelIdx] = float4(shCoeffs[0], shCoeffs[1].x);
    g_OutputSH1[voxelIdx] = float4(shCoeffs[1].yz, shCoeffs[2].xy);
    g_OutputSH2[voxelIdx] = float4(shCoeffs[2].z, shCoeffs[3]);
}

// ============================================
// Closest Hit Shader
// ============================================
[shader("closesthit")]
void ClosestHit(inout SRayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    // Get instance and primitive data
    uint instanceID = InstanceID();
    uint primitiveID = PrimitiveIndex();

    // Compute hit position and normal
    float3 barycentrics = float3(1 - attr.barycentrics.x - attr.barycentrics.y,
                                  attr.barycentrics.x, attr.barycentrics.y);

    // Get geometry data (vertex positions, normals)
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 normal = GetInterpolatedNormal(instanceID, primitiveID, barycentrics);

    // Get material
    SMaterialData mat = g_Materials[g_Instances[instanceID].materialIndex];

    // Evaluate direct lighting
    float3 directLight = EvaluateDirectLighting(hitPos, normal, mat);

    // Accumulate radiance
    payload.radiance += payload.throughput * directLight;

    // Prepare next bounce
    payload.bounceCount++;

    // Russian Roulette
    if (payload.bounceCount >= 2) {
        float p = max(mat.albedo.r, max(mat.albedo.g, mat.albedo.b));
        if (Random(payload.rngState) > p) {
            payload.terminated = true;
            return;
        }
        payload.throughput /= p;
    }

    // Sample next direction (cosine-weighted hemisphere)
    float3 bounceDir = SampleHemisphereCosine(normal, payload.rngState);

    payload.nextOrigin = hitPos + normal * 0.001;  // Offset to avoid self-intersection
    payload.nextDirection = bounceDir;
    payload.throughput *= mat.albedo;  // BRDF: albedo/PI * PI (cancels)
}

// ============================================
// Miss Shader
// ============================================
[shader("miss")]
void Miss(inout SRayPayload payload) {
    // Sample skybox
    float3 skyColor = g_Skybox.SampleLevel(g_LinearSampler, WorldRayDirection(), 0).rgb;
    payload.radiance += payload.throughput * skyColor;
    payload.terminated = true;
}
```

### 4.2 Helper Functions

**File**: `Shader/DXR/LightmapBakeCommon.hlsl`

```hlsl
// RNG (PCG)
uint InitRNG(uint3 voxelIdx, uint frameIdx) {
    return voxelIdx.x * 1973 + voxelIdx.y * 9277 + voxelIdx.z * 26699 + frameIdx * 103;
}

float Random(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) / 4294967295.0;
}

// Sampling
float3 SampleSphereUniform(inout uint rng) {
    float u1 = Random(rng);
    float u2 = Random(rng);
    float z = 1.0 - 2.0 * u1;
    float r = sqrt(max(0, 1.0 - z*z));
    float phi = 2.0 * PI * u2;
    return float3(r * cos(phi), r * sin(phi), z);
}

float3 SampleHemisphereCosine(float3 normal, inout uint rng) {
    float u1 = Random(rng);
    float u2 = Random(rng);

    float r = sqrt(u1);
    float phi = 2.0 * PI * u2;

    float3 localDir = float3(r * cos(phi), r * sin(phi), sqrt(1.0 - u1));

    // Transform to world space
    float3 tangent, bitangent;
    BuildTangentBasis(normal, tangent, bitangent);

    return localDir.x * tangent + localDir.y * bitangent + localDir.z * normal;
}

// SH Basis (L2)
void EvaluateSHBasis(float3 dir, out float basis[9]) {
    basis[0] = 0.282095;
    basis[1] = 0.488603 * dir.y;
    basis[2] = 0.488603 * dir.z;
    basis[3] = 0.488603 * dir.x;
    basis[4] = 1.092548 * dir.x * dir.y;
    basis[5] = 1.092548 * dir.y * dir.z;
    basis[6] = 0.315392 * (3.0 * dir.z * dir.z - 1.0);
    basis[7] = 1.092548 * dir.x * dir.z;
    basis[8] = 0.546274 * (dir.x * dir.x - dir.y * dir.y);
}

void AccumulateToSH(float3 dir, float3 radiance, inout float3 shCoeffs[9]) {
    float basis[9];
    EvaluateSHBasis(dir, basis);

    for (int i = 0; i < 9; i++) {
        shCoeffs[i] += radiance * basis[i];
    }
}
```

### 4.3 Direct Lighting Evaluation

```hlsl
float3 EvaluateDirectLighting(float3 hitPos, float3 normal, SMaterialData mat) {
    float3 result = 0;

    // Directional lights
    for (uint i = 0; i < g_NumDirectionalLights; i++) {
        SDirectionalLight light = g_DirectionalLights[i];
        float3 L = -light.direction;
        float NdotL = saturate(dot(normal, L));

        if (NdotL > 0) {
            // Shadow ray
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + normal * 0.001;
            shadowRay.Direction = L;
            shadowRay.TMin = 0.001;
            shadowRay.TMax = 10000.0;

            SRayPayload shadowPayload;
            shadowPayload.terminated = false;

            TraceRay(g_Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     0xFF, 0, 0, 1, shadowRay, shadowPayload);  // Miss shader index 1 = shadow miss

            if (shadowPayload.terminated) {  // No hit = not shadowed
                result += light.color * light.intensity * mat.albedo * NdotL / PI;
            }
        }
    }

    // Point lights (similar with distance attenuation)
    // Spot lights (similar with cone falloff)

    return result;
}
```

---

## Phase 5: DXR Baker Integration (3-4 days)

### 5.1 Main Baker Class

**File**: `Engine/Rendering/RayTracing/DXRLightmapBaker.h/cpp`

```cpp
class CDXRLightmapBaker {
public:
    struct Config {
        uint32_t samplesPerVoxel = 256;  // Lower than CPU, more passes
        uint32_t maxBounces = 3;
        uint32_t accumulationPasses = 24; // samplesPerVoxel * passes = total samples
    };

    bool Initialize();
    void Shutdown();

    // Main baking entry point
    bool BakeVolumetricLightmap(
        CVolumetricLightmap& lightmap,
        CScene& scene,
        const Config& config
    );

private:
    // Acceleration structures
    std::unique_ptr<CDXRAccelerationStructureManager> m_asManager;

    // Pipeline
    RHI::RayTracingPipelineStatePtr m_pipeline;
    RHI::ShaderBindingTablePtr m_sbt;

    // Output textures (ping-pong for accumulation)
    RHI::TexturePtr m_outputSH[3];      // SH0, SH1, SH2
    RHI::TexturePtr m_accumulatorSH[3]; // For multi-pass accumulation

    // Scene data buffers
    RHI::BufferPtr m_materialBuffer;
    RHI::BufferPtr m_lightBuffer;
    RHI::BufferPtr m_instanceDataBuffer;

    // Methods
    bool buildAccelerationStructures(CScene& scene);
    bool createPipeline();
    bool createOutputTextures(const CVolumetricLightmap& lightmap);
    void uploadSceneData(CScene& scene);
    void dispatchBakePass(uint32_t passIndex, const Config& config);
    void copyResultsToLightmap(CVolumetricLightmap& lightmap);
};
```

### 5.2 Baking Pipeline

```cpp
bool CDXRLightmapBaker::BakeVolumetricLightmap(
    CVolumetricLightmap& lightmap,
    CScene& scene,
    const Config& config)
{
    // 1. Build acceleration structures
    if (!buildAccelerationStructures(scene)) {
        CFFLog::Error("[DXRBaker] Failed to build AS");
        return false;
    }

    // 2. Create output textures matching lightmap dimensions
    if (!createOutputTextures(lightmap)) {
        CFFLog::Error("[DXRBaker] Failed to create output textures");
        return false;
    }

    // 3. Upload scene data (materials, lights)
    uploadSceneData(scene);

    // 4. Multi-pass accumulation
    CFFLog::Info("[DXRBaker] Starting %d accumulation passes...", config.accumulationPasses);

    auto startTime = std::chrono::high_resolution_clock::now();

    for (uint32_t pass = 0; pass < config.accumulationPasses; pass++) {
        dispatchBakePass(pass, config);

        // Progress
        if ((pass + 1) % 4 == 0) {
            float progress = 100.0f * (pass + 1) / config.accumulationPasses;
            CFFLog::Info("[DXRBaker] Progress: %.1f%%", progress);
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float elapsedSec = std::chrono::duration<float>(endTime - startTime).count();
    CFFLog::Info("[DXRBaker] Baking complete in %.2f seconds", elapsedSec);

    // 5. Copy results back to lightmap CPU data
    copyResultsToLightmap(lightmap);

    return true;
}
```

### 5.3 Dispatch Configuration

```cpp
void CDXRLightmapBaker::dispatchBakePass(uint32_t passIndex, const Config& config)
{
    auto* cmdList = RHI::CRHIManager::Instance().GetRenderContext()->GetCommandList();

    // Update constant buffer
    CB_BakeParams params = {};
    params.volumeMin = m_lightmapConfig.volumeMin;
    params.volumeMax = m_lightmapConfig.volumeMax;
    params.voxelGridSize = m_voxelGridSize;  // Total voxels across all bricks
    params.samplesPerVoxel = config.samplesPerVoxel;
    params.maxBounces = config.maxBounces;
    params.frameIndex = passIndex;  // Different seed each pass

    cmdList->SetConstantBufferData(RHI::EShaderStage::Compute, 0, &params, sizeof(params));

    // Bind resources
    cmdList->SetShaderResource(RHI::EShaderStage::Compute, 0, m_asManager->GetTLAS());
    cmdList->SetShaderResource(RHI::EShaderStage::Compute, 1, m_skyboxTexture.get());
    // ... bind other resources ...

    cmdList->SetUnorderedAccessTexture(0, m_outputSH[0].get());
    cmdList->SetUnorderedAccessTexture(1, m_outputSH[1].get());
    cmdList->SetUnorderedAccessTexture(2, m_outputSH[2].get());

    // Dispatch rays
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord = m_sbt->GetRayGenRecord();
    dispatchDesc.MissShaderTable = m_sbt->GetMissTable();
    dispatchDesc.HitGroupTable = m_sbt->GetHitGroupTable();
    dispatchDesc.Width = m_voxelGridSize.x;
    dispatchDesc.Height = m_voxelGridSize.y;
    dispatchDesc.Depth = m_voxelGridSize.z;

    cmdList->DispatchRays(dispatchDesc);
}
```

---

## Phase 6: VolumetricLightmap Integration (2-3 days)

### 6.1 Dual-Path Baking

**Modify**: `Engine/Rendering/VolumetricLightmap.h/cpp`

```cpp
class CVolumetricLightmap {
public:
    enum class EBakeBackend {
        CPU,    // Existing CPathTraceBaker
        GPU_DXR // New CDXRLightmapBaker
    };

    struct BakeConfig {
        EBakeBackend backend = EBakeBackend::GPU_DXR;

        // CPU config (existing)
        int cpuSamplesPerVoxel = 6144;
        int cpuMaxBounces = 3;

        // GPU config (new)
        int gpuSamplesPerVoxel = 256;
        int gpuAccumulationPasses = 24;  // Total = 256 * 24 = 6144
        int gpuMaxBounces = 3;
    };

    void BakeAllBricks(CScene& scene, const BakeConfig& config = {});

private:
    void bakeWithCPU(CScene& scene, const BakeConfig& config);
    void bakeWithGPU(CScene& scene, const BakeConfig& config);

    std::unique_ptr<CDXRLightmapBaker> m_dxrBaker;
};
```

### 6.2 Automatic Backend Selection

```cpp
void CVolumetricLightmap::BakeAllBricks(CScene& scene, const BakeConfig& config)
{
    EBakeBackend backend = config.backend;

    // Auto-fallback if DXR not supported
    if (backend == EBakeBackend::GPU_DXR) {
        auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
        if (!ctx->SupportsRayTracing()) {
            CFFLog::Warning("[VolumetricLightmap] DXR not supported, falling back to CPU");
            backend = EBakeBackend::CPU;
        }
    }

    if (backend == EBakeBackend::GPU_DXR) {
        bakeWithGPU(scene, config);
    } else {
        bakeWithCPU(scene, config);
    }
}
```

### 6.3 Voxel Grid Linearization

Current system uses sparse octree bricks. For GPU dispatch, need continuous 3D grid mapping:

```cpp
// Option A: Dispatch per-brick (simpler)
// - Dispatch 64 threads per brick (4x4x4)
// - Multiple dispatches for all bricks

// Option B: Linearized voxel grid (more efficient)
// - Flatten all brick voxels into continuous 3D texture
// - Single large dispatch
// - Map linear index back to brick + local voxel

// Recommended: Option B for GPU efficiency
struct SVoxelMapping {
    uint32_t brickIndex;
    uint32_t localVoxelIndex;
};

std::vector<SVoxelMapping> linearizeVoxels(const std::vector<SBrick>& bricks);
```

---

## Phase 7: Validation & Optimization (2-3 days)

### 7.1 Validation

- [ ] Compare GPU vs CPU results (should be nearly identical)
- [ ] Visual comparison in editor viewport
- [ ] SH coefficient diff analysis
- [ ] Performance benchmarks

### 7.2 Debug Visualization

```cpp
// Export individual voxel radiance as cubemap (like CPU version)
void CDXRLightmapBaker::ExportDebugCubemap(
    const XMFLOAT3& worldPos,
    const std::string& outputPath);
```

### 7.3 Optimizations

1. **Wavefront Path Tracing**
   - Sort rays by direction for coherent memory access
   - Reduces warp divergence

2. **Shared Memory BVH Caching**
   - Cache top BVH levels in shared memory

3. **Async Compute**
   - Run baking on async compute queue
   - Don't block main rendering

4. **Progressive Refinement**
   - Show intermediate results during baking
   - Allow early termination

---

## Phase 8: Editor Integration (1-2 days)

### 8.1 UI Updates

**Modify**: `Editor/Panels_SceneLightSettings.cpp`

```cpp
// Backend selection
const char* backends[] = { "CPU (Path Trace)", "GPU (DXR)" };
int currentBackend = (int)bakeConfig.backend;
if (ImGui::Combo("Bake Backend", &currentBackend, backends, 2)) {
    bakeConfig.backend = (EBakeBackend)currentBackend;
}

// Show appropriate settings based on backend
if (bakeConfig.backend == EBakeBackend::GPU_DXR) {
    ImGui::SliderInt("Samples Per Pass", &bakeConfig.gpuSamplesPerVoxel, 64, 512);
    ImGui::SliderInt("Accumulation Passes", &bakeConfig.gpuAccumulationPasses, 1, 64);

    int totalSamples = bakeConfig.gpuSamplesPerVoxel * bakeConfig.gpuAccumulationPasses;
    ImGui::Text("Total Samples: %d", totalSamples);
} else {
    ImGui::SliderInt("Samples Per Voxel", &bakeConfig.cpuSamplesPerVoxel, 64, 16384);
}

ImGui::SliderInt("Max Bounces", &bakeConfig.maxBounces, 1, 8);

// Progress bar during baking
if (m_isBaking) {
    ImGui::ProgressBar(m_bakeProgress);
    ImGui::Text("ETA: %.1f seconds", m_bakeETA);
}
```

### 8.2 Async Baking

```cpp
// Non-blocking bake with progress callback
void CVolumetricLightmap::BakeAllBricksAsync(
    CScene& scene,
    const BakeConfig& config,
    std::function<void(float progress)> progressCallback,
    std::function<void(bool success)> completionCallback);
```

---

## Timeline Summary

| Phase | Description | Duration | Dependencies |
|-------|-------------|----------|--------------|
| 0 | Prerequisites & Research | 1-2 days | - |
| 1 | RHI DXR Foundation | 3-5 days | Phase 0 |
| 2 | Scene Geometry Export | 2-3 days | Phase 1 |
| 3 | BLAS/TLAS Management | 3-4 days | Phase 2 |
| 4 | DXR Shaders | 4-5 days | Phase 3 |
| 5 | DXR Baker Integration | 3-4 days | Phase 4 |
| 6 | VolumetricLightmap Integration | 2-3 days | Phase 5 |
| 7 | Validation & Optimization | 2-3 days | Phase 6 |
| 8 | Editor Integration | 1-2 days | Phase 7 |

**Total Estimated Time**: 3-4 weeks

---

## Risk Mitigation

1. **Fallback Path**: Keep CPU baker as fallback for non-RTX hardware
2. **Incremental Testing**: Test each phase independently before integration
3. **Reference Comparison**: Always compare GPU results against CPU baseline
4. **Memory Management**: Monitor VRAM usage, especially for large scenes

---

## Future Enhancements

1. **Denoising**: Add AI denoiser (NVIDIA NRD) for fewer samples
2. **Incremental Updates**: Only rebake changed regions
3. **LOD Support**: Different quality levels based on distance
4. **Probe Streaming**: Load/unload bricks based on camera position

---

*Created: 2025-12-19*
