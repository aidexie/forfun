#pragma once

#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include <memory>
#include <functional>

// ============================================
// DXR Lightmap Baker
// ============================================
// GPU-accelerated lightmap baking using DXR ray tracing.
// Replaces CPU CPathTraceBaker for massive speedup.

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
struct SBrick;  // Forward declaration from VolumetricLightmap.h

// ============================================
// Baker Configuration
// ============================================

// Debug flags for step-by-step verification with Nsight Graphics
// See docs/DXR_VERIFICATION_ROADMAP.md for details
struct SDXRDebugFlags {
    // Phase 0: Pause for Nsight capture (5 seconds before dispatch)
    bool enableCaptureDelay = false;

    // Phase 1: Log acceleration structure info (BLAS/TLAS)
    bool logAccelerationStructure = true;

    // Phase 2: Log pipeline and SBT creation
    bool logPipelineCreation = true;

    // Phase 3: Log resource binding details
    bool logResourceBinding = true;

    // Phase 4: Log DispatchRays parameters
    bool logDispatchInfo = true;

    // Phase 5: Log readback SH values and statistics
    bool logReadbackResults = true;

    // Phase 6: Pause after first brick for inspection
    bool pauseAfterFirstBrick = false;

    // Verbose mode: log every brick (instead of just first)
    bool verboseLogging = false;
};

struct SDXRBakeConfig {
    // Samples per voxel per pass (GPU handles many in parallel)
    uint32_t samplesPerVoxel = 256;

    // Number of accumulation passes (total samples = samplesPerVoxel * accumulationPasses)
    uint32_t accumulationPasses = 24;

    // Maximum ray bounces
    uint32_t maxBounces = 3;

    // Sky intensity multiplier
    float skyIntensity = 1.0f;

    // Progress callback (0.0 to 1.0)
    std::function<void(float)> progressCallback = nullptr;

    // Debug flags for verification
    SDXRDebugFlags debug;
};

// ============================================
// Bake Parameters (matches shader CB_BakeParams)
// Per-brick dispatch - updated to match LightmapBake.hlsl
// ============================================

struct CB_BakeParams {
    // Current brick info
    DirectX::XMFLOAT3 brickWorldMin;
    float padding0;
    DirectX::XMFLOAT3 brickWorldMax;
    float padding1;

    // Bake parameters
    uint32_t samplesPerVoxel;
    uint32_t maxBounces;
    uint32_t frameIndex;
    uint32_t numLights;

    float skyIntensity;
    uint32_t brickIndex;      // Current brick being baked
    uint32_t totalBricks;     // Total number of bricks
    float padding2;
};

// ============================================
// GPU Voxel SH Output (matches shader SVoxelSHOutput)
// Output structure for one voxel in a brick
// ============================================

struct SVoxelSHOutput {
    DirectX::XMFLOAT3 sh[9];  // L2 SH coefficients (9 RGB values) = 108 bytes
    float validity;            // 4 bytes
    DirectX::XMFLOAT3 padding; // 12 bytes to align to 16-byte boundary
};

// 9 * 12 (XMFLOAT3) + 4 (validity) + 12 (padding) = 124 bytes
// But HLSL structured buffer may have different packing rules
// Verify with actual GPU output

// ============================================
// GPU Material Data (matches shader SMaterialData)
// ============================================

struct SGPUMaterialData {
    DirectX::XMFLOAT3 albedo;
    float metallic;
    float roughness;
    float padding[3];
};

// ============================================
// GPU Light Data (matches shader SLightData)
// ============================================

struct SGPULightData {
    uint32_t type;          // 0=Directional, 1=Point, 2=Spot
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

// ============================================
// GPU Instance Data (matches shader SInstanceData)
// ============================================

struct SGPUInstanceData {
    uint32_t materialIndex;
    float padding[3];
};

// ============================================
// CDXRLightmapBaker
// ============================================

class CDXRLightmapBaker {
public:
    CDXRLightmapBaker();
    ~CDXRLightmapBaker();

    // Non-copyable
    CDXRLightmapBaker(const CDXRLightmapBaker&) = delete;
    CDXRLightmapBaker& operator=(const CDXRLightmapBaker&) = delete;

    // Initialize baker (creates pipeline, allocates resources)
    bool Initialize();

    // Shutdown and release resources
    void Shutdown();

    // Check if baker is ready
    bool IsReady() const { return m_isReady; }

    // Check if DXR is available
    bool IsAvailable() const;

    // ============================================
    // Main Baking Entry Points
    // ============================================

    // Bake a volumetric lightmap using DXR
    // Returns true on success
    bool BakeVolumetricLightmap(
        CVolumetricLightmap& lightmap,
        CScene& scene,
        const SDXRBakeConfig& config = {});

    // Bake from pre-exported scene data
    bool BakeVolumetricLightmap(
        CVolumetricLightmap& lightmap,
        const SRayTracingSceneData& sceneData,
        const SDXRBakeConfig& config = {});

private:
    // ============================================
    // Initialization
    // ============================================

    bool CreatePipeline();
    bool CreateShaderBindingTable();
    bool CreateConstantBuffer();

    // ============================================
    // Per-Bake Setup
    // ============================================

    bool CreateOutputBuffer();  // Per-brick structured buffer (64 voxels)
    bool CreateReadbackBuffer();  // CPU-readable staging buffer
    bool UploadSceneData(const SRayTracingSceneData& sceneData);
    bool BuildAccelerationStructures(const SRayTracingSceneData& sceneData);

    // ============================================
    // Baking
    // ============================================

    void DispatchBakeBrick(uint32_t brickIndex, const SBrick& brick, const SDXRBakeConfig& config);
    void ReadbackBrickResults(SBrick& brick, const SDXRBakeConfig& config);
    void CopyResultsToLightmap(CVolumetricLightmap& lightmap);

    // ============================================
    // Cleanup
    // ============================================

    void ReleasePerBakeResources();

private:
    bool m_isReady = false;

    // Acceleration structure manager
    std::unique_ptr<CDXRAccelerationStructureManager> m_asManager;

    // Ray tracing pipeline
    std::unique_ptr<RHI::IRayTracingPipelineState> m_pipeline;
    std::unique_ptr<RHI::IShaderBindingTable> m_sbt;
    std::unique_ptr<RHI::IShader> m_shaderLibrary;

    // Constant buffer
    std::unique_ptr<RHI::IBuffer> m_constantBuffer;

    // Linear sampler for skybox sampling
    std::unique_ptr<RHI::ISampler> m_linearSampler;

    // Scene data buffers
    std::unique_ptr<RHI::IBuffer> m_materialBuffer;
    std::unique_ptr<RHI::IBuffer> m_lightBuffer;
    std::unique_ptr<RHI::IBuffer> m_instanceBuffer;

    // Per-brick output buffer (UAV - 64 voxels × SVoxelSHOutput)
    std::unique_ptr<RHI::IBuffer> m_outputBuffer;

    // Readback buffer (CPU-readable staging)
    std::unique_ptr<RHI::IBuffer> m_readbackBuffer;

    // CPU-side readback data
    std::vector<SVoxelSHOutput> m_readbackData;

    // Current bake state
    DirectX::XMFLOAT3 m_volumeMin;
    DirectX::XMFLOAT3 m_volumeMax;
    uint32_t m_numLights = 0;
    uint32_t m_totalBricks = 0;

    // Skybox texture for environment lighting (not owned, borrowed from scene)
    RHI::ITexture* m_skyboxTexture = nullptr;
    RHI::ISampler* m_skyboxTextureSample = nullptr;
};
