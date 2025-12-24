#pragma once

#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include <memory>
#include <functional>
#include <array>

// ============================================
// DXR Cubemap-Based Lightmap Baker
// ============================================
// GPU-accelerated lightmap baking using cubemap ray dispatch.
// Each voxel dispatches 32x32x6 = 6144 rays (matching CPU baker).
// Output is a cubemap per voxel, then projected to SH on CPU.
//
// Advantages over random sampling:
// - Sample parity with CPU baker for validation
// - Direct cubemap output for debugging
// - No [unroll] loop limitations
// - Better GPU utilization (6144 parallel rays)

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

    // Export path (directory)
    std::string debugExportPath;
};

struct SDXRCubemapBakeConfig {
    // Cubemap resolution per face (32x32 = 1024 rays per face)
    uint32_t cubemapResolution = 32;

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
// Cubemap Bake Constant Buffer (matches shader)
// ============================================

struct CB_CubemapBakeParams {
    DirectX::XMFLOAT3 voxelWorldPos;
    float padding0;

    uint32_t maxBounces;
    uint32_t frameIndex;
    uint32_t numLights;
    float skyIntensity;

    uint32_t voxelIndex;
    uint32_t padding1[3];
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
    float padding[3];
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
    // Voxel Baking (One voxel at a time)
    // ============================================

    // Dispatch bake for all voxels in lightmap
    bool DispatchBakeAllVoxels(CVolumetricLightmap& lightmap, const SDXRCubemapBakeConfig& config);

    void DispatchBakeVoxel(const DirectX::XMFLOAT3& worldPos, uint32_t voxelIndex,
                           const SDXRCubemapBakeConfig& config);
    void ReadbackCubemap();

    // Project cubemap to SH coefficients
    void ProjectCubemapToSH(std::array<DirectX::XMFLOAT3, 9>& outSH);

    // Check voxel validity (inside geometry check)
    float CheckVoxelValidity(const DirectX::XMFLOAT3& worldPos);

    // Export cubemap for debugging
    void ExportDebugCubemap(const std::string& path, uint32_t brickIdx, uint32_t voxelIdx);

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

    // Cubemap output buffer (UAV - 32x32x6 = 6144 float4)
    std::unique_ptr<RHI::IBuffer> m_cubemapOutputBuffer;

    // Readback buffer (CPU-readable staging)
    std::unique_ptr<RHI::IBuffer> m_cubemapReadbackBuffer;

    // CPU-side cubemap data (RGBA per pixel)
    std::vector<DirectX::XMFLOAT4> m_cubemapData;

    // State
    uint32_t m_numLights = 0;

    // Skybox texture (borrowed from scene)
    RHI::ITexture* m_skyboxTexture = nullptr;
    RHI::ISampler* m_skyboxTextureSampler = nullptr;
};
