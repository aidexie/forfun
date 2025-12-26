#pragma once

#include "../RHI/RHIRayTracing.h"
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

// ============================================
// Scene Geometry Export for DXR
// ============================================
// Structures and utilities for exporting scene geometry
// to build DXR acceleration structures.

namespace RHI {
    class IBuffer;
    class IRenderContext;
}

class CScene;
class GpuMeshResource;

// ============================================
// Exported Mesh Data
// ============================================

// CPU-side mesh geometry data for BLAS building
struct SRayTracingMeshData {
    // Vertex positions (float3)
    std::vector<DirectX::XMFLOAT3> positions;

    // Vertex normals (float3) - for lightmap baking
    std::vector<DirectX::XMFLOAT3> normals;

    // Triangle indices
    std::vector<uint32_t> indices;

    // Vertex count and index count
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;

    // Local space bounds
    DirectX::XMFLOAT3 boundsMin{0, 0, 0};
    DirectX::XMFLOAT3 boundsMax{0, 0, 0};

    // Source mesh path (for debugging)
    std::string sourcePath;
};

// Instance data for TLAS building
struct SRayTracingInstance {
    // World transform (4x4 matrix, row-major)
    DirectX::XMFLOAT4X4 worldTransform;

    // Index into mesh array (which BLAS to instance)
    uint32_t meshIndex = 0;

    // Material index for shader access
    uint32_t materialIndex = 0;

    // Instance ID (user-defined, accessible via InstanceID() in shader)
    uint32_t instanceID = 0;

    // Visibility mask (for ray masking)
    uint8_t instanceMask = 0xFF;

    // Global buffer offsets (for geometry lookup in shader)
    uint32_t vertexBufferOffset = 0;  // Offset into global vertex buffer (in vertices)
    uint32_t indexBufferOffset = 0;   // Offset into global index buffer (in triangles)
};

// Material data for shaders
struct SRayTracingMaterial {
    DirectX::XMFLOAT3 albedo{1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float padding[3];
};

// Light data for shaders
struct SRayTracingLight {
    enum class EType : uint32_t {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    EType type = EType::Directional;
    DirectX::XMFLOAT3 position{0, 0, 0};
    DirectX::XMFLOAT3 direction{0, -1, 0};
    DirectX::XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f;
    float padding;
};

// Complete exported scene data
struct SRayTracingSceneData {
    // Unique meshes (each becomes a BLAS)
    std::vector<SRayTracingMeshData> meshes;

    // Instances referencing meshes (each becomes a TLAS instance)
    std::vector<SRayTracingInstance> instances;

    // Materials
    std::vector<SRayTracingMaterial> materials;

    // Lights
    std::vector<SRayTracingLight> lights;

    // ============================================
    // Global Geometry Buffers (for shader access)
    // ============================================
    // All vertex positions concatenated (float4 for alignment)
    std::vector<DirectX::XMFLOAT4> globalVertexPositions;

    // All indices concatenated
    std::vector<uint32_t> globalIndices;

    // Scene bounds (for volumetric lightmap)
    DirectX::XMFLOAT3 sceneBoundsMin{0, 0, 0};
    DirectX::XMFLOAT3 sceneBoundsMax{0, 0, 0};

    // Statistics
    uint32_t totalTriangles = 0;
    uint32_t totalVertices = 0;
};

// ============================================
// Scene Geometry Exporter
// ============================================

class CSceneGeometryExporter {
public:
    // Export scene geometry for ray tracing
    // Returns nullptr on failure
    static std::unique_ptr<SRayTracingSceneData> ExportScene(CScene& scene);

    // Export a single mesh from GpuMeshResource
    // Note: This requires the mesh to have CPU data stored
    static bool ExportMesh(
        const GpuMeshResource& meshResource,
        const std::string& path,
        SRayTracingMeshData& outData);

private:
    // Extract positions from vertex buffer (VBO contains SVertexPNT)
    // This requires reading back from GPU or having CPU copy
    static bool ExtractPositionsFromVertices(
        const void* vertexData,
        uint32_t vertexCount,
        uint32_t vertexStride,
        std::vector<DirectX::XMFLOAT3>& outPositions);
};

// ============================================
// Ray Tracing Mesh Cache
// ============================================
// Stores CPU mesh data for BLAS building.
// Works alongside GpuMeshResource.

class CRayTracingMeshCache {
public:
    static CRayTracingMeshCache& Instance();

    // Store mesh data for ray tracing (called during mesh loading)
    void StoreMeshData(const std::string& path, uint32_t subMeshIndex, const SRayTracingMeshData& data);

    // Get cached mesh data
    const SRayTracingMeshData* GetMeshData(const std::string& path, uint32_t subMeshIndex) const;

    // Check if mesh data is cached
    bool HasMeshData(const std::string& path, uint32_t subMeshIndex) const;

    // Clear all cached data
    void Clear();

private:
    CRayTracingMeshCache() = default;

    // Key: "path:subMeshIndex"
    std::unordered_map<std::string, SRayTracingMeshData> m_cache;

    std::string MakeKey(const std::string& path, uint32_t subMeshIndex) const {
        return path + ":" + std::to_string(subMeshIndex);
    }
};
