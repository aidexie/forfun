#pragma once
#include "GpuMeshResource.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct MeshCPU_PNT;
struct GltfMeshCPU;
struct ID3D11Device;
struct ID3D11ShaderResourceView;

// Manages GPU mesh resources with path-based caching and automatic deduplication
class MeshResourceManager {
public:
    // Get singleton instance
    static MeshResourceManager& Instance();

    // Load or retrieve cached mesh resource by path
    // Returns nullptr on failure
    // Supports .obj and .gltf files
    // For glTF files with multiple sub-meshes, returns a vector
    std::vector<std::shared_ptr<GpuMeshResource>> GetOrLoad(
        const std::string& path
    );

    // Remove unused resources from cache (resources only held by weak_ptr)
    void CollectGarbage();

    // Clear all cached resources
    void ClearCache();

private:
    MeshResourceManager() = default;
    ~MeshResourceManager() = default;

    // Non-copyable, non-movable (singleton)
    MeshResourceManager(const MeshResourceManager&) = delete;
    MeshResourceManager& operator=(const MeshResourceManager&) = delete;

    // Upload CPU mesh to GPU
    std::shared_ptr<GpuMeshResource> UploadMesh(
        const MeshCPU_PNT& cpu
    );

    // Upload glTF mesh to GPU (with textures)
    std::shared_ptr<GpuMeshResource> UploadGltfMesh(
        const GltfMeshCPU& gltfMesh
    );

private:
    // Cache: path -> weak_ptr (allows resources to be freed when no longer used)
    std::unordered_map<std::string, std::vector<std::weak_ptr<GpuMeshResource>>> m_cache;
};
