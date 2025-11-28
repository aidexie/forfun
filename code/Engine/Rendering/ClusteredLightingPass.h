#pragma once
#include <d3d11_1.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

class CScene;

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

// Clustered Lighting Pass
// Responsibilities:
// 1. Build cluster grid (AABB for each cluster)
// 2. Cull lights into clusters
// 3. Provide cluster data to MainPass
// 4. Debug visualization (optional)
class CClusteredLightingPass {
public:
    CClusteredLightingPass();
    ~CClusteredLightingPass();

    void Initialize(ID3D11Device* device);
    void Resize(uint32_t width, uint32_t height);

    // Build cluster grid (view-space AABBs for all clusters)
    // Call once per frame when camera changes
    void BuildClusterGrid(ID3D11DeviceContext* context,
                          const DirectX::XMMATRIX& projection,
                          float nearZ, float farZ);

    // Cull lights into clusters
    // Call once per frame after gathering point lights from scene
    void CullLights(ID3D11DeviceContext* context,
                    CScene* scene,
                    const DirectX::XMMATRIX& view);

    // Bind cluster data to MainPass pixel shader
    // Binds: g_ClusterData (t10), g_CompactLightList (t11), g_PointLights (t12)
    void BindToMainPass(ID3D11DeviceContext* context);

    // Debug visualization
    enum class EDebugMode {
        None,
        LightCountHeatmap,   // Show light count per cluster as heatmap
        ClusterAABB          // Show cluster bounding boxes
    };
    void SetDebugMode(EDebugMode mode) { m_debugMode = mode; }
    void RenderDebug(ID3D11DeviceContext* context);

    // Get current cluster grid dimensions
    uint32_t GetNumClustersX() const { return m_numClustersX; }
    uint32_t GetNumClustersY() const { return m_numClustersY; }
    uint32_t GetNumClustersZ() const { return ClusteredConfig::DEPTH_SLICES; }

private:
    void CreateBuffers(ID3D11Device* device);
    void CreateShaders(ID3D11Device* device);
    void CreateDebugShaders(ID3D11Device* device);

    // Screen dimensions
    uint32_t m_screenWidth = 0;
    uint32_t m_screenHeight = 0;

    // Cluster grid dimensions
    uint32_t m_numClustersX = 0;  // ceil(width / TILE_SIZE)
    uint32_t m_numClustersY = 0;  // ceil(height / TILE_SIZE)
    uint32_t m_totalClusters = 0; // X * Y * Z

    // GPU Buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_clusterAABBBuffer;       // SClusterAABB[totalClusters]
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_clusterDataBuffer;       // SClusterData[totalClusters]
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_compactLightListBuffer;  // uint[MAX_TOTAL_LIGHT_REFS]
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_pointLightBuffer;        // SGpuPointLight[maxLights]
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_globalCounterBuffer;     // uint (atomic counter for light list)

    // Shader Resource Views (for pixel shader)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_clusterDataSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_compactLightListSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pointLightSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_clusterAABBSRV;

    // Unordered Access Views (for compute shader)
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_clusterAABBUAV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_clusterDataUAV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_compactLightListUAV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_globalCounterUAV;

    // Compute Shaders
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_buildClusterGridCS;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_cullLightsCS;

    // Constant Buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_clusterCB;  // Projection params, near/far

    // Debug visualization
    EDebugMode m_debugMode = EDebugMode::None;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_debugVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_debugHeatmapPS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_debugAABBPS;

    // Cached projection parameters for dirty checking
    float m_cachedNearZ = 0.0f;
    float m_cachedFarZ = 0.0f;
    float m_cachedFovY = 0.0f;  // Field of view (from projection matrix)
    bool m_clusterGridDirty = true;  // Force rebuild on first frame
};
