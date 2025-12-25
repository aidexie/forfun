#pragma once

#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include <memory>
#include <functional>
#include <array>

// ============================================
// DXR Cubemap-Based Lightmap Baker (Batched)
// ============================================
// GPU-accelerated lightmap baking using batched cubemap ray dispatch.
// Processes entire brick (64 voxels) in single dispatch for performance.
//
// Architecture:
// - Dispatch (128, 128, 6 * batchSize) threads per brick
// - Single GPU->CPU readback per brick (vs per voxel)
// - CPU SH projection for all voxels in batch
//
// Performance:
// - 64x fewer sync points (1 per brick vs 1 per voxel)
// - Better GPU utilization (more threads per dispatch)
// - Maintains debug cubemap export capability

namespace RHI {
    class IRenderContext;
    class IBuffer;
    class ITexture;
    class IShader;
    class IRayTracingPipelineState;
    class IShaderBindingTable;
}

class CVolumetricLightmap;
class CScene;
struct SBrick;

// ============================================
// Cubemap Baker Configuration
// ============================================

struct SDXRCubemapBakeDebugFlags {
    // Log dispatch info
    bool logDispatchInfo = true;

    // Log readback statistics
    bool logReadbackResults = true;

    // Export debug cubemap for specific voxels
    bool exportDebugCubemaps = false;

    // Max number of cubemaps to export (0 = all valid voxels)
    uint32_t maxDebugCubemaps = 1;

    // Export SH values to text file for verification
    bool exportSHToText = false;

    // Export path (directory)
    std::string debugExportPath;
};

struct SDXRCubemapBakeConfig {
    // Cubemap resolution per face (32x32 = 1024 rays per face)
    uint32_t cubemapResolution = 32;

    // Batch size (voxels per dispatch, 64 = 1 brick)
    uint32_t batchSize = 64;

    // Maximum ray bounces
    uint32_t maxBounces = 3;

    // Sky intensity multiplier
    float skyIntensity = 1.0f;

    // Progress callback (0.0 to 1.0)
    std::function<void(float)> progressCallback = nullptr;

    // Debug flags
    SDXRCubemapBakeDebugFlags debug;
};

// ============================================
// Batch Bake Constant Buffer (matches shader CB_BatchBakeParams)
// ============================================

struct CB_BatchBakeParams {
    uint32_t batchSize;       // Number of voxels in this batch (typically 64)
    uint32_t maxBounces;
    uint32_t numLights;
    float skyIntensity;

    uint32_t frameIndex;      // For RNG seeding
    uint32_t brickIndex;      // For debugging/RNG seeding
    uint32_t padding[2];
};

// ============================================
// GPU Data Structures (shared with old baker)
// ============================================

struct SGPUMaterialDataCubemap {
    DirectX::XMFLOAT3 albedo;
    float metallic;
    float roughness;
    float padding[3];
};

struct SGPULightDataCubemap {
    uint32_t type;
    float padding0[3];
    DirectX::XMFLOAT3 position;
    float padding1;
    DirectX::XMFLOAT3 direction;
    float padding2;
    DirectX::XMFLOAT3 color;
    float intensity;
    float range;
    float spotAngle;
    float padding3[2];
};

struct SGPUInstanceDataCubemap {
    uint32_t materialIndex;
    uint32_t vertexBufferOffset;  // Offset into global vertex buffer
    uint32_t indexBufferOffset;   // Offset into global index buffer (in triangles)
    uint32_t padding;
};

// ============================================
// Cubemap Output (6144 pixels = 32x32x6)
// ============================================

constexpr uint32_t CUBEMAP_BAKE_RES = 32;
constexpr uint32_t CUBEMAP_BAKE_FACES = 6;
constexpr uint32_t CUBEMAP_PIXELS_PER_FACE = CUBEMAP_BAKE_RES * CUBEMAP_BAKE_RES;
constexpr uint32_t CUBEMAP_TOTAL_PIXELS = CUBEMAP_PIXELS_PER_FACE * CUBEMAP_BAKE_FACES;

// ============================================
// CDXRCubemapBaker
// ============================================

class CDXRCubemapBaker {
public:
    CDXRCubemapBaker();
    ~CDXRCubemapBaker();

    // Non-copyable
    CDXRCubemapBaker(const CDXRCubemapBaker&) = delete;
    CDXRCubemapBaker& operator=(const CDXRCubemapBaker&) = delete;

    // Initialize baker
    bool Initialize();

    // Shutdown and release resources
    void Shutdown();

    // Check if baker is ready
    bool IsReady() const { return m_isReady; }

    // Check if DXR is available
    bool IsAvailable() const;

    // ============================================
    // Main Baking Entry Point
    // ============================================

    // Bake volumetric lightmap using cubemap-based approach
    bool BakeVolumetricLightmap(
        CVolumetricLightmap& lightmap,
        CScene& scene,
        const SDXRCubemapBakeConfig& config = {});

    // Bake from pre-exported scene data
    bool BakeVolumetricLightmap(
        CVolumetricLightmap& lightmap,
        const SRayTracingSceneData& sceneData,
        const SDXRCubemapBakeConfig& config = {});

private:
    // ============================================
    // Initialization
    // ============================================

    bool CreatePipeline();
    bool CreateShaderBindingTable();
    bool CreateConstantBuffer();
    bool CreateCubemapOutputBuffer();
    bool CreateCubemapReadbackBuffer();

    // ============================================
    // Per-Bake Setup
    // ============================================

    // Prepare all resources for baking (AS, pipeline, buffers)
    bool PrepareBakeResources(const SRayTracingSceneData& sceneData);

    bool UploadSceneData(const SRayTracingSceneData& sceneData);
    bool BuildAccelerationStructures(const SRayTracingSceneData& sceneData);

    // ============================================
    // Brick Baking (Batched - 64 voxels per dispatch)
    // ============================================
public:
    // Dispatch bake for all voxels in lightmap
    bool DispatchBakeAllVoxels(CVolumetricLightmap& lightmap, const SDXRCubemapBakeConfig& config);

private:
    // Batch operations
    bool CreateVoxelPositionsBuffer(uint32_t batchSize);
    void UploadVoxelPositions(const std::vector<DirectX::XMFLOAT4>& positions);
    void DispatchBakeBrick(uint32_t batchSize, uint32_t brickIndex,
                           const SDXRCubemapBakeConfig& config);
    void ReadbackBatchCubemaps(uint32_t batchSize);

    // Project cubemap to SH coefficients (with voxel offset for batch)
    void ProjectCubemapToSH(uint32_t voxelIdxInBatch, std::array<DirectX::XMFLOAT3, 9>& outSH);

    // Check voxel validity (inside geometry check)
    float CheckVoxelValidity(const DirectX::XMFLOAT3& worldPos);

    // Export cubemap for debugging (with voxel offset for batch)
    void ExportDebugCubemap(const std::string& path, uint32_t brickIdx, uint32_t voxelIdx);

    // Export all SH values to text file for verification
    void ExportSHToText(const CVolumetricLightmap& lightmap, const std::string& path);

    // ============================================
    // Cleanup
    // ============================================

    void ReleasePerBakeResources();

private:
    bool m_isReady = false;

    // Acceleration structure manager
    std::unique_ptr<CDXRAccelerationStructureManager> m_asManager;

    // Ray tracing pipeline (cubemap shader)
    std::unique_ptr<RHI::IRayTracingPipelineState> m_pipeline;
    std::unique_ptr<RHI::IShaderBindingTable> m_sbt;
    std::unique_ptr<RHI::IShader> m_shaderLibrary;

    // Constant buffer
    std::unique_ptr<RHI::IBuffer> m_constantBuffer;

    // Scene data buffers
    std::unique_ptr<RHI::IBuffer> m_materialBuffer;
    std::unique_ptr<RHI::IBuffer> m_lightBuffer;
    std::unique_ptr<RHI::IBuffer> m_instanceBuffer;

    // Global geometry buffers (for normal computation in shader)
    std::unique_ptr<RHI::IBuffer> m_vertexBuffer;   // All vertex positions
    std::unique_ptr<RHI::IBuffer> m_indexBuffer;    // All indices

    // Voxel positions buffer (batch mode - 64 float4 for positions + validity)
    std::unique_ptr<RHI::IBuffer> m_voxelPositionsBuffer;

    // Batched cubemap output buffer (UAV - batchSize * 32x32x6 float4)
    // For 64 voxels: 64 * 6144 = 393216 float4 values (~6 MB)
    std::unique_ptr<RHI::IBuffer> m_cubemapOutputBuffer;

    // Readback buffer (CPU-readable staging, same size as output)
    std::unique_ptr<RHI::IBuffer> m_cubemapReadbackBuffer;

    // CPU-side batched cubemap data (RGBA per pixel, for entire batch)
    std::vector<DirectX::XMFLOAT4> m_cubemapData;

    // Current batch size (for buffer sizing)
    uint32_t m_currentBatchSize = 0;

    // State
    uint32_t m_numLights = 0;

    // Skybox texture (borrowed from scene)
    RHI::ITexture* m_skyboxTexture = nullptr;
    RHI::ISampler* m_skyboxTextureSampler = nullptr;
};
