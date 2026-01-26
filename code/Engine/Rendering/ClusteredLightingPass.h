#pragma once
#include "RHI/RHIPointers.h"
#include "IPerFrameContributor.h"
#include "RHI/PerFrameSlots.h"
#include <DirectXMath.h>
#include <vector>
#include <cstdint>

// Forward declarations
class CScene;
namespace RHI {
    class ICommandList;
}

// Clustered Shading parameters
// Based on configuration: 32×32 tiles, 16 depth slices
namespace ClusteredConfig {
    constexpr uint32_t TILE_SIZE = 32;           // Pixels per tile (32×32)
    constexpr uint32_t DEPTH_SLICES = 16;        // Logarithmic depth slices
    constexpr uint32_t MAX_LIGHTS_PER_CLUSTER = 100;  // Maximum lights in one cluster
    constexpr uint32_t MAX_TOTAL_LIGHT_REFS = 1024 * 1024;  // 1M light references (compact list)
}

// GPU data structures (must match HLSL)
struct SClusterAABB {
    DirectX::XMFLOAT4 minPoint;  // xyz = min, w = unused
    DirectX::XMFLOAT4 maxPoint;  // xyz = max, w = unused
};

struct SClusterData {
    uint32_t offset;  // Offset in compact light index list
    uint32_t count;   // Number of lights in this cluster
};

// Light type enumeration (must match HLSL)
enum class ELightType : uint32_t {
    Point = 0,
    Spot = 1
};

// Unified GPU light structure (supports both Point and Spot lights)
// Union-style: Spot-specific fields are unused for Point lights
struct SGpuLight {
    DirectX::XMFLOAT3 position;  // World space position (all types)
    float range;                 // Maximum light radius (all types)
    DirectX::XMFLOAT3 color;     // Linear RGB (all types)
    float intensity;             // Luminous intensity (all types)

    // Spot light specific (unused for point lights, zero-initialized)
    DirectX::XMFLOAT3 direction; // World space direction (normalized)
    float innerConeAngle;        // cos(innerAngle) - precomputed for shader
    float outerConeAngle;        // cos(outerAngle) - precomputed for shader
    uint32_t type;               // ELightType (0=Point, 1=Spot)
    DirectX::XMFLOAT2 padding;   // Align to 16 bytes
};

// Legacy typedef for compatibility (will be removed)
using SGpuPointLight = SGpuLight;

// Constant buffer for pixel shader (b3)
struct alignas(16) CB_ClusteredParams {
    float nearZ;
    float farZ;
    uint32_t numClustersX;
    uint32_t numClustersY;
    uint32_t numClustersZ;
    uint32_t _pad[3];
};

// Clustered Lighting Pass
// Responsibilities:
// 1. Build cluster grid (AABB for each cluster)
// 2. Cull lights into clusters
// 3. Provide cluster data to MainPass
// 4. Debug visualization (optional)
class CClusteredLightingPass : public IPerFrameContributor {
public:
    CClusteredLightingPass();
    ~CClusteredLightingPass();

    // Initialize with RHI (no longer needs device parameter)
    void Initialize();
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    // Build cluster grid (view-space AABBs for all clusters)
    // Call once per frame when camera changes
    void BuildClusterGrid(RHI::ICommandList* cmdList,
                          const DirectX::XMMATRIX& projection,
                          float nearZ, float farZ);

    // Cull lights into clusters
    // Call once per frame after gathering point lights from scene
    void CullLights(RHI::ICommandList* cmdList,
                    CScene* scene,
                    const DirectX::XMMATRIX& view);

    // Bind cluster data to MainPass pixel shader
    // Binds: g_ClusterData (t8), g_CompactLightList (t9), g_Lights (t10)
    void BindToMainPass(RHI::ICommandList* cmdList);

    // Debug visualization
    enum class EDebugMode {
        None,
        LightCountHeatmap,   // Show light count per cluster as heatmap
        ClusterAABB          // Show cluster bounding boxes
    };
    void SetDebugMode(EDebugMode mode) { m_debugMode = mode; }
    void RenderDebug(RHI::ICommandList* cmdList);

    // IPerFrameContributor implementation
    void PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) override;

    // Get current cluster grid dimensions
    uint32_t GetNumClustersX() const { return m_numClustersX; }
    uint32_t GetNumClustersY() const { return m_numClustersY; }
    uint32_t GetNumClustersZ() const { return ClusteredConfig::DEPTH_SLICES; }

private:
    void CreateBuffers();
    void CreateShaders();
    void CreateDebugShaders();

    // Screen dimensions
    uint32_t m_screenWidth = 0;
    uint32_t m_screenHeight = 0;

    // Cluster grid dimensions
    uint32_t m_numClustersX = 0;  // ceil(width / TILE_SIZE)
    uint32_t m_numClustersY = 0;  // ceil(height / TILE_SIZE)
    uint32_t m_totalClusters = 0; // X * Y * Z

    // GPU Buffers (RHI)
    RHI::BufferPtr m_clusterAABBBuffer;       // SClusterAABB[totalClusters]
    RHI::BufferPtr m_clusterDataBuffer;       // SClusterData[totalClusters]
    RHI::BufferPtr m_compactLightListBuffer;  // uint[MAX_TOTAL_LIGHT_REFS]
    RHI::BufferPtr m_pointLightBuffer;        // SGpuPointLight[maxLights]
    RHI::BufferPtr m_globalCounterBuffer;     // uint (atomic counter for light list)

    // Compute Shaders (RHI)
    RHI::ShaderPtr m_buildClusterGridCS;
    RHI::ShaderPtr m_cullLightsCS;

    // Compute Pipeline States (cached to avoid per-frame creation)
    RHI::PipelineStatePtr m_buildClusterGridPSO;
    RHI::PipelineStatePtr m_cullLightsPSO;

    // Debug visualization
    EDebugMode m_debugMode = EDebugMode::None;
    RHI::ShaderPtr m_debugVS;
    RHI::ShaderPtr m_debugHeatmapPS;
    RHI::ShaderPtr m_debugAABBPS;

    // Cached projection parameters for dirty checking
    float m_cachedNearZ = 0.0f;
    float m_cachedFarZ = 0.0f;
    float m_cachedFovY = 0.0f;  // Field of view (from projection matrix)
    bool m_clusterGridDirty = true;  // Force rebuild on first frame
    bool m_initialized = false;
};
