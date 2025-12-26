#pragma once
#include "LightmapTypes.h"
#include <vector>
#include <DirectXMath.h>

// ============================================
// Lightmap Atlas Packing
// ============================================
// Packs multiple meshes into a single lightmap atlas texture.
// Uses a simple row-based packing algorithm.

class CLightmapAtlas {
public:
    CLightmapAtlas() = default;
    ~CLightmapAtlas() = default;

    // Pack meshes into atlas
    // meshSizes: vector of (width, height) for each mesh's lightmap region
    // config: atlas configuration
    // Returns true if all meshes fit
    bool Pack(
        const std::vector<std::pair<int, int>>& meshSizes,
        const SLightmapAtlasConfig& config
    );

    // Get packing results
    const std::vector<SAtlasEntry>& GetEntries() const { return m_entries; }
    int GetAtlasCount() const { return m_atlasCount; }
    int GetAtlasResolution() const { return m_resolution; }

    // Compute scale/offset for shader binding
    // Returns float4: xy = scale, zw = offset
    static DirectX::XMFLOAT4 ComputeScaleOffset(
        const SAtlasEntry& entry,
        int atlasResolution
    );

    // Compute lightmap size for a mesh based on its world-space bounds
    // Returns (width, height) in texels
    static std::pair<int, int> ComputeMeshLightmapSize(
        const DirectX::XMFLOAT3& boundsMin,
        const DirectX::XMFLOAT3& boundsMax,
        int texelsPerUnit,
        int minSize = 4,
        int maxSize = 512
    );

private:
    std::vector<SAtlasEntry> m_entries;
    int m_atlasCount = 0;
    int m_resolution = 1024;
};

// ============================================
// Multi-Mesh Atlas Builder
// ============================================
// Higher-level interface for building lightmap atlas from scene meshes

struct SLightmapMeshInfo {
    int meshRendererIndex;           // Index in scene
    DirectX::XMFLOAT3 boundsMin;     // World-space AABB
    DirectX::XMFLOAT3 boundsMax;
    bool hasUV2;                     // Already has UV2?
};

class CLightmapAtlasBuilder {
public:
    // Add mesh to be packed
    void AddMesh(const SLightmapMeshInfo& meshInfo);

    // Build atlas with given config
    // Returns true if successful
    bool Build(const SLightmapAtlasConfig& config);

    // Get results
    const CLightmapAtlas& GetAtlas() const { return m_atlas; }
    const std::vector<SLightmapInfo>& GetLightmapInfos() const { return m_lightmapInfos; }

    // Clear all data
    void Clear();

private:
    std::vector<SLightmapMeshInfo> m_meshInfos;
    CLightmapAtlas m_atlas;
    std::vector<SLightmapInfo> m_lightmapInfos;  // Per-mesh lightmap info
};
