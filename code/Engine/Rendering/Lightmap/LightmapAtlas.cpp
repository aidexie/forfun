#include "LightmapAtlas.h"
#include "Core/FFLog.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

// ============================================
// CLightmapAtlas Implementation
// ============================================

bool CLightmapAtlas::Pack(
    const std::vector<std::pair<int, int>>& meshSizes,
    const SLightmapAtlasConfig& config)
{
    m_entries.clear();
    m_entries.resize(meshSizes.size());
    m_resolution = config.resolution;
    m_atlasCount = 0;

    if (meshSizes.empty()) {
        return true;
    }

    // Sort meshes by height (tallest first) for better packing
    std::vector<size_t> sortedIndices(meshSizes.size());
    for (size_t i = 0; i < sortedIndices.size(); i++) {
        sortedIndices[i] = i;
    }
    std::sort(sortedIndices.begin(), sortedIndices.end(),
        [&meshSizes](size_t a, size_t b) {
            return meshSizes[a].second > meshSizes[b].second;  // Sort by height descending
        });

    // Simple row-based packing
    int currentX = 0;
    int currentY = 0;
    int rowHeight = 0;
    int atlasIndex = 0;

    for (size_t sortedIdx = 0; sortedIdx < sortedIndices.size(); sortedIdx++) {
        size_t i = sortedIndices[sortedIdx];
        int w = meshSizes[i].first + config.padding;
        int h = meshSizes[i].second + config.padding;

        // Check if mesh is too large for atlas
        if (w > config.resolution || h > config.resolution) {
            CFFLog::Error("[LightmapAtlas] Mesh %d too large for atlas (%dx%d > %d)",
                         (int)i, meshSizes[i].first, meshSizes[i].second, config.resolution);
            return false;
        }

        // Check if fits in current row
        if (currentX + w > config.resolution) {
            // Move to next row
            currentX = 0;
            currentY += rowHeight;
            rowHeight = 0;
        }

        // Check if fits in current atlas
        if (currentY + h > config.resolution) {
            // Need new atlas
            atlasIndex++;
            currentX = 0;
            currentY = 0;
            rowHeight = 0;
        }

        // Place mesh
        m_entries[i].meshRendererIndex = static_cast<int>(i);
        m_entries[i].atlasIndex = atlasIndex;
        m_entries[i].atlasX = currentX;
        m_entries[i].atlasY = currentY;
        m_entries[i].width = meshSizes[i].first;
        m_entries[i].height = meshSizes[i].second;

        currentX += w;
        rowHeight = std::max(rowHeight, h);
    }

    m_atlasCount = atlasIndex + 1;

    CFFLog::Info("[LightmapAtlas] Packed %d meshes into %d atlas(es) (%dx%d each)",
                (int)meshSizes.size(), m_atlasCount, config.resolution, config.resolution);

    return true;
}

XMFLOAT4 CLightmapAtlas::ComputeScaleOffset(
    const SAtlasEntry& entry,
    int atlasResolution)
{
    float invRes = 1.0f / static_cast<float>(atlasResolution);

    XMFLOAT4 result;
    result.x = static_cast<float>(entry.width) * invRes;   // scale U
    result.y = static_cast<float>(entry.height) * invRes;  // scale V
    result.z = static_cast<float>(entry.atlasX) * invRes;  // offset U
    result.w = static_cast<float>(entry.atlasY) * invRes;  // offset V

    return result;
}

std::pair<int, int> CLightmapAtlas::ComputeMeshLightmapSize(
    const XMFLOAT3& boundsMin,
    const XMFLOAT3& boundsMax,
    int texelsPerUnit,
    int minSize,
    int maxSize)
{
    // Compute world-space dimensions
    float sizeX = boundsMax.x - boundsMin.x;
    float sizeY = boundsMax.y - boundsMin.y;
    float sizeZ = boundsMax.z - boundsMin.z;

    // Use the two largest dimensions for UV space
    // (assuming the mesh is roughly planar or box-like)
    float dims[3] = {sizeX, sizeY, sizeZ};
    std::sort(dims, dims + 3, std::greater<float>());

    float uvWidth = dims[0];
    float uvHeight = dims[1];

    // Convert to texels
    int width = static_cast<int>(std::ceil(uvWidth * texelsPerUnit));
    int height = static_cast<int>(std::ceil(uvHeight * texelsPerUnit));

    // Clamp to valid range
    width = std::max(minSize, std::min(maxSize, width));
    height = std::max(minSize, std::min(maxSize, height));

    // Round up to power of 2 (optional, helps with mipmapping)
    // For lightmaps, not strictly necessary
    // width = nextPowerOf2(width);
    // height = nextPowerOf2(height);

    return {width, height};
}

// ============================================
// CLightmapAtlasBuilder Implementation
// ============================================

void CLightmapAtlasBuilder::AddMesh(const SLightmapMeshInfo& meshInfo)
{
    m_meshInfos.push_back(meshInfo);
}

bool CLightmapAtlasBuilder::Build(const SLightmapAtlasConfig& config)
{
    m_lightmapInfos.clear();

    if (m_meshInfos.empty()) {
        CFFLog::Info("[LightmapAtlasBuilder] No meshes to pack");
        return true;
    }

    // Compute lightmap sizes for each mesh
    std::vector<std::pair<int, int>> meshSizes;
    meshSizes.reserve(m_meshInfos.size());

    for (const auto& info : m_meshInfos) {
        auto size = CLightmapAtlas::ComputeMeshLightmapSize(
            info.boundsMin,
            info.boundsMax,
            config.texelsPerUnit,
            4,    // minSize
            512   // maxSize
        );
        meshSizes.push_back(size);

        CFFLog::Info("[LightmapAtlasBuilder] Mesh %d: bounds (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) -> %dx%d texels",
                    info.meshRendererIndex,
                    info.boundsMin.x, info.boundsMin.y, info.boundsMin.z,
                    info.boundsMax.x, info.boundsMax.y, info.boundsMax.z,
                    size.first, size.second);
    }

    // Pack into atlas
    if (!m_atlas.Pack(meshSizes, config)) {
        CFFLog::Error("[LightmapAtlasBuilder] Failed to pack atlas");
        return false;
    }

    // Generate lightmap info for each mesh
    m_lightmapInfos.resize(m_meshInfos.size());
    const auto& entries = m_atlas.GetEntries();

    for (size_t i = 0; i < m_meshInfos.size(); i++) {
        m_lightmapInfos[i].lightmapIndex = entries[i].atlasIndex;
        m_lightmapInfos[i].scaleOffset = CLightmapAtlas::ComputeScaleOffset(
            entries[i],
            m_atlas.GetAtlasResolution()
        );
    }

    return true;
}

void CLightmapAtlasBuilder::Clear()
{
    m_meshInfos.clear();
    m_lightmapInfos.clear();
    m_atlas = CLightmapAtlas();
}
