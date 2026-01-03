#pragma once

#include "LightmapTypes.h"
#include "../RayTracing/DXRAccelerationStructureManager.h"
#include "../RayTracing/SceneGeometryExport.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/RHIPointers.h"
#include <memory>
#include <functional>
#include <vector>

// ============================================
// 2D Lightmap GPU Baker (DXR-based)
// ============================================
// GPU-accelerated lightmap baking using DXR ray tracing.
// Designed specifically for 2D texture atlas lightmaps.
//
// Key differences from Volumetric Lightmap baker:
// - Samples hemisphere above surface normal (not full sphere/cubemap)
// - Output is RGB irradiance per texel (not SH coefficients)
// - Texels are 2D atlas coordinates (not 3D voxel positions)
//
// Architecture (Option B+C):
// 1. Linearize valid texels from rasterizer
// 2. Batched ray dispatch (1024 texels per batch)
// 3. GPU accumulation buffer
// 4. GPU finalize pass (compute shader)
// 5. Optional dilation pass

namespace RHI {
    class IRenderContext;
    class IBuffer;
    class ITexture;
    class IShader;
    class IRayTracingPipelineState;
    class IShaderBindingTable;
    class IComputePipelineState;
}

class CScene;
class CLightmapRasterizer;

// ============================================
// GPU Texel Data (uploaded to GPU)
// ============================================

struct SGPUTexelData {
    DirectX::XMFLOAT3 worldPos;
    float validity;             // 1.0 = valid, 0.0 = invalid
    DirectX::XMFLOAT3 normal;
    float padding;
    uint32_t atlasX;            // Position in atlas
    uint32_t atlasY;
    uint32_t padding2[2];
};

// ============================================
// Bake Configuration
// ============================================

struct SLightmap2DGPUBakeConfig {
    uint32_t samplesPerTexel = 64;      // Monte Carlo samples per texel
    uint32_t maxBounces = 3;            // Max ray bounces for GI
    float skyIntensity = 1.0f;          // Sky light intensity multiplier

    // Progress callback (0.0 to 1.0)
    std::function<void(float, const char*)> progressCallback = nullptr;

    // Debug options
    bool exportDebugInfo = false;
    std::string debugExportPath;
};

// ============================================
// Constant Buffer (matches shader)
// ============================================

struct CB_Lightmap2DBakeParams {
    uint32_t totalTexels;           // Valid texel count
    uint32_t samplesPerTexel;       // e.g., 64
    uint32_t maxBounces;            // e.g., 3
    float skyIntensity;

    uint32_t atlasWidth;
    uint32_t atlasHeight;
    uint32_t batchOffset;           // Offset into texel buffer for this batch
    uint32_t batchSize;             // Texels in this batch

    uint32_t frameIndex;            // For RNG seeding
    uint32_t numLights;
    uint32_t padding[2];
};

// ============================================
// GPU Material/Light/Instance Data
// ============================================

struct SGPUMaterialData2D {
    DirectX::XMFLOAT3 albedo;
    float metallic;
    float roughness;
    float padding[3];
};

struct SGPULightData2D {
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

struct SGPUInstanceData2D {
    uint32_t materialIndex;
    uint32_t vertexBufferOffset;
    uint32_t indexBufferOffset;
    uint32_t padding;
};

// ============================================
// CLightmap2DGPUBaker
// ============================================

class CLightmap2DGPUBaker {
public:
    CLightmap2DGPUBaker();
    ~CLightmap2DGPUBaker();

    // Non-copyable
    CLightmap2DGPUBaker(const CLightmap2DGPUBaker&) = delete;
    CLightmap2DGPUBaker& operator=(const CLightmap2DGPUBaker&) = delete;

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

    // Bake lightmap from scene and rasterizer data
    // Returns HDR atlas texture (R16G16B16A16_FLOAT)
    RHI::TexturePtr BakeLightmap(
        CScene& scene,
        const CLightmapRasterizer& rasterizer,
        const SLightmap2DGPUBakeConfig& config = {});

    // Bake from pre-exported scene data
    RHI::TexturePtr BakeLightmap(
        const SRayTracingSceneData& sceneData,
        const std::vector<STexelData>& texels,
        uint32_t atlasWidth,
        uint32_t atlasHeight,
        const SLightmap2DGPUBakeConfig& config = {});

private:
    // ============================================
    // Initialization
    // ============================================

    bool CreatePipeline();
    bool CreateFinalizePipeline();
    bool CreateDilatePipeline();
    bool CreateConstantBuffer();

    // ============================================
    // Per-Bake Setup
    // ============================================

    bool PrepareBakeResources(const SRayTracingSceneData& sceneData);
    bool UploadSceneData(const SRayTracingSceneData& sceneData);
    bool BuildAccelerationStructures(const SRayTracingSceneData& sceneData);

    // ============================================
    // Texel Data Management
    // ============================================

    // Linearize valid texels from rasterizer
    void LinearizeTexels(
        const std::vector<STexelData>& texels,
        uint32_t atlasWidth,
        uint32_t atlasHeight);

    // Create/resize texel buffer
    bool CreateTexelBuffer(uint32_t texelCount);

    // Upload texel data to GPU
    void UploadTexelData();

    // ============================================
    // Baking
    // ============================================

    // Create accumulation buffer
    bool CreateAccumulationBuffer(uint32_t atlasWidth, uint32_t atlasHeight);

    // Create output texture
    bool CreateOutputTexture(uint32_t atlasWidth, uint32_t atlasHeight);

    // Dispatch batched ray tracing
    void DispatchBake(const SLightmap2DGPUBakeConfig& config);

    // Finalize: normalize accumulation and write to atlas
    void FinalizeAtlas();

    // Optional: Dilation pass
    void DilateLightmap(int radius);

    // ============================================
    // Cleanup
    // ============================================

    void ReleasePerBakeResources();

    // Progress reporting
    void ReportProgress(float progress, const char* stage);

private:
    bool m_isReady = false;

    // Acceleration structure manager
    std::unique_ptr<CDXRAccelerationStructureManager> m_asManager;

    // Ray tracing pipeline
    std::unique_ptr<RHI::IRayTracingPipelineState> m_rtPipeline;
    std::unique_ptr<RHI::IShaderBindingTable> m_sbt;
    std::unique_ptr<RHI::IShader> m_rtShaderLibrary;

    // Finalize compute pipeline
    std::unique_ptr<RHI::IPipelineState> m_finalizePipeline;
    std::unique_ptr<RHI::IShader> m_finalizeShader;

    // Dilation compute pipeline
    std::unique_ptr<RHI::IPipelineState> m_dilatePipeline;
    std::unique_ptr<RHI::IShader> m_dilateShader;

    // Ping-pong texture for dilation passes
    RHI::TexturePtr m_dilateTemp;

    // Constant buffer
    std::unique_ptr<RHI::IBuffer> m_constantBuffer;

    // Scene data buffers
    std::unique_ptr<RHI::IBuffer> m_materialBuffer;
    std::unique_ptr<RHI::IBuffer> m_lightBuffer;
    std::unique_ptr<RHI::IBuffer> m_instanceBuffer;

    // Global geometry buffers
    std::unique_ptr<RHI::IBuffer> m_vertexBuffer;
    std::unique_ptr<RHI::IBuffer> m_indexBuffer;

    // Texel data buffer (linearized valid texels)
    std::unique_ptr<RHI::IBuffer> m_texelBuffer;
    std::vector<SGPUTexelData> m_linearizedTexels;

    // Texel index to atlas coordinate mapping
    std::vector<uint32_t> m_texelToAtlasX;
    std::vector<uint32_t> m_texelToAtlasY;

    // Accumulation buffer (uint4 per atlas texel) for atomic operations
    // xyz = fixed-point accumulated radiance (scale 65536), w = sample count
    std::unique_ptr<RHI::IBuffer> m_accumulationBuffer;

    // Output texture (R16G16B16A16_FLOAT)
    RHI::TexturePtr m_outputTexture;

    // Bake state
    uint32_t m_atlasWidth = 0;
    uint32_t m_atlasHeight = 0;
    uint32_t m_validTexelCount = 0;
    uint32_t m_numLights = 0;

    // Progress callback
    std::function<void(float, const char*)> m_progressCallback;

    // Skybox texture (borrowed from scene)
    RHI::ITexture* m_skyboxTexture = nullptr;
    RHI::ISampler* m_skyboxSampler = nullptr;
};
