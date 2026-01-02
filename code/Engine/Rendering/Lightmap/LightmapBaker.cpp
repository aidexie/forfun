#include "LightmapBaker.h"
#include "Lightmap2DGPUBaker.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Rendering/RayTracing/SceneGeometryExport.h"
#include "Core/FFLog.h"
#include "Core/Mesh.h"
#include "Core/PathManager.h"
#include "Core/Exporter/KTXExporter.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>

using namespace DirectX;

CLightmapBaker::CLightmapBaker() = default;
CLightmapBaker::~CLightmapBaker() = default;

void CLightmapBaker::reportProgress(float progress, const char* stage)
{
    if (m_progressCallback) {
        m_progressCallback(progress, stage);
    }
    CFFLog::Info("[LightmapBaker] %.0f%% - %s", progress * 100.0f, stage);
}

bool CLightmapBaker::Bake(CScene& scene, const Config& config, const std::string& lightmapPath)
{
    reportProgress(0.0f, "Starting lightmap bake");

    // Step 1: Pack atlas
    reportProgress(0.10f, "Packing atlas");
    if (!packAtlas(scene, config.atlasConfig)) {
        CFFLog::Error("[LightmapBaker] Atlas packing failed");
        return false;
    }

    // Step 2: Rasterize
    reportProgress(0.20f, "Rasterizing meshes");
    if (!rasterize(scene)) {
        CFFLog::Error("[LightmapBaker] Rasterization failed");
        return false;
    }

    // Step 3: Bake irradiance
    reportProgress(0.30f, "Baking irradiance");
    if (!bakeIrradiance(scene, config.bakeConfig)) {
        CFFLog::Error("[LightmapBaker] Baking failed");
        return false;
    }

    // Step 4: Assign lightmapInfosIndex to MeshRenderers
    reportProgress(0.96f, "Assigning lightmap indices");
    assignLightmapIndices(scene);

    // Step 5: Save to file
    reportProgress(0.98f, "Saving to file");
    if (!saveToFile(lightmapPath)) {
        CFFLog::Error("[LightmapBaker] Failed to save lightmap");
        return false;
    }

    reportProgress(1.0f, "Bake complete");
    return true;
}

bool CLightmapBaker::packAtlas(CScene& scene, const SLightmapAtlasConfig& config)
{
    m_atlasBuilder.Clear();

    auto& world = scene.GetWorld();
    int meshCount = 0;

    // Collect all static meshes
    for (int i = 0; i < world.Count(); i++) {
        auto* obj = world.Get(i);
        if (!obj) continue;

        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();
        if (!meshRenderer || !transform) continue;

        // TODO: Check if mesh is marked as static for lightmapping
        // For now, include all meshes

        // Get local-space AABB from mesh resource
        XMFLOAT3 boundsMin, boundsMax;
        if (!meshRenderer->GetLocalBounds(boundsMin, boundsMax)) {
            // Fallback to unit cube if bounds not available
            boundsMin = {-0.5f, -0.5f, -0.5f};
            boundsMax = {0.5f, 0.5f, 0.5f};
        }

        // Get world-space AABB by transforming local bounds
        XMMATRIX worldMatrix = transform->WorldMatrix();

        // Transform corners and find new AABB
        XMFLOAT3 corners[8] = {
            {boundsMin.x, boundsMin.y, boundsMin.z},
            {boundsMax.x, boundsMin.y, boundsMin.z},
            {boundsMin.x, boundsMax.y, boundsMin.z},
            {boundsMax.x, boundsMax.y, boundsMin.z},
            {boundsMin.x, boundsMin.y, boundsMax.z},
            {boundsMax.x, boundsMin.y, boundsMax.z},
            {boundsMin.x, boundsMax.y, boundsMax.z},
            {boundsMax.x, boundsMax.y, boundsMax.z},
        };

        XMFLOAT3 worldMin = {FLT_MAX, FLT_MAX, FLT_MAX};
        XMFLOAT3 worldMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

        for (int c = 0; c < 8; c++) {
            XMVECTOR corner = XMLoadFloat3(&corners[c]);
            corner = XMVector3TransformCoord(corner, worldMatrix);
            XMFLOAT3 worldCorner;
            XMStoreFloat3(&worldCorner, corner);

            worldMin.x = std::min(worldMin.x, worldCorner.x);
            worldMin.y = std::min(worldMin.y, worldCorner.y);
            worldMin.z = std::min(worldMin.z, worldCorner.z);
            worldMax.x = std::max(worldMax.x, worldCorner.x);
            worldMax.y = std::max(worldMax.y, worldCorner.y);
            worldMax.z = std::max(worldMax.z, worldCorner.z);
        }

        SLightmapMeshInfo meshInfo;
        meshInfo.meshRendererIndex = i;
        meshInfo.boundsMin = worldMin;
        meshInfo.boundsMax = worldMax;
        meshInfo.hasUV2 = false;  // TODO: Check actual UV2 availability

        m_atlasBuilder.AddMesh(meshInfo);
        meshCount++;
    }

    if (meshCount == 0) {
        CFFLog::Info("[LightmapBaker] No meshes to lightmap");
        return true;
    }

    // Build atlas
    if (!m_atlasBuilder.Build(config)) {
        return false;
    }

    // Store results
    m_lightmapInfos = m_atlasBuilder.GetLightmapInfos();
    m_atlasWidth = config.resolution;
    m_atlasHeight = config.resolution;

    CFFLog::Info("[LightmapBaker] Packed %d meshes into %dx%d atlas",
                meshCount, m_atlasWidth, m_atlasHeight);

    return true;
}

bool CLightmapBaker::rasterize(CScene& scene)
{
    if (m_atlasWidth == 0 || m_atlasHeight == 0) {
        CFFLog::Error("[LightmapBaker] Atlas not initialized");
        return false;
    }

    m_rasterizer.Initialize(m_atlasWidth, m_atlasHeight);

    const auto& atlas = m_atlasBuilder.GetAtlas();
    const auto& entries = atlas.GetEntries();
    auto& world = scene.GetWorld();
    auto& meshCache = CRayTracingMeshCache::Instance();

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];
        auto* obj = world.Get(entry.meshRendererIndex);
        if (!obj) continue;

        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();
        if (!meshRenderer || !transform) continue;

        // Get mesh data from ray tracing cache (includes UV2)
        // Note: Mesh must be loaded with cacheForRayTracing=true and generateLightmapUV2=true
        const SRayTracingMeshData* meshData = meshCache.GetMeshData(meshRenderer->path, 0);
        if (!meshData) {
            CFFLog::Warning("[LightmapBaker] Mesh data not cached: %s (skipping)", meshRenderer->path.c_str());
            continue;
        }

        // Check if UV2 is available
        if (meshData->uv2.empty()) {
            CFFLog::Warning("[LightmapBaker] Mesh has no UV2: %s (skipping)", meshRenderer->path.c_str());
            continue;
        }

        // Rasterize using mesh's UV2 data
        m_rasterizer.RasterizeMesh(
            meshData->positions,
            meshData->normals,
            meshData->uv2,
            meshData->indices,
            transform->WorldMatrix(),
            entry.atlasX, entry.atlasY,
            entry.width, entry.height
        );
    }

    int validCount = m_rasterizer.GetValidTexelCount();
    CFFLog::Info("[LightmapBaker] Rasterized %d valid texels", validCount);

    return true;
}

bool CLightmapBaker::bakeIrradiance(CScene& scene, const SLightmap2DBakeConfig& config)
{
    // Initialize GPU baker if not already done (lazy init, reused across bakes)
    if (!m_gpuBaker.IsAvailable()) {
        if (!m_gpuBaker.Initialize()) {
            CFFLog::Error("[LightmapBaker] Failed to initialize GPU baker");
            return false;
        }

        if (!m_gpuBaker.IsAvailable()) {
            CFFLog::Error("[LightmapBaker] DXR not available for GPU baking");
            return false;
        }
    }

    // Configure GPU bake
    SLightmap2DGPUBakeConfig gpuConfig;
    gpuConfig.samplesPerTexel = config.samplesPerTexel;
    gpuConfig.maxBounces = config.maxBounces;
    gpuConfig.skyIntensity = config.skyIntensity;
    gpuConfig.progressCallback = [this](float progress, const char* stage) {
        // Map GPU baker progress (0-1) to our progress range (0.30 - 0.95)
        float mappedProgress = 0.30f + progress * 0.65f;
        reportProgress(mappedProgress, stage);
    };

    // Bake using GPU
    m_gpuTexture = m_gpuBaker.BakeLightmap(scene, m_rasterizer, gpuConfig);

    if (!m_gpuTexture) {
        CFFLog::Error("[LightmapBaker] GPU baking failed");
        return false;
    }

    CFFLog::Info("[LightmapBaker] GPU baking complete (%dx%d)", m_atlasWidth, m_atlasHeight);
    return true;
}

void CLightmapBaker::assignLightmapIndices(CScene& scene)
{
    auto& world = scene.GetWorld();
    int infoCount = static_cast<int>(m_lightmapInfos.size());

    for (int i = 0; i < infoCount; i++) {
        auto* obj = world.Get(i);
        if (!obj) continue;

        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        if (meshRenderer) {
            meshRenderer->lightmapInfosIndex = i;
        }
    }

    CFFLog::Info("[LightmapBaker] Assigned lightmap indices to %d MeshRenderers", infoCount);
}

// ============================================
// File Format (same as Lightmap2DManager)
// ============================================

struct SLightmapDataHeader {
    uint32_t magic = 0x4C4D3244;  // "LM2D"
    uint32_t version = 1;
    uint32_t infoCount = 0;
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    uint32_t reserved[3] = {0, 0, 0};
};

bool CLightmapBaker::saveToFile(const std::string& lightmapPath)
{
    if (m_lightmapInfos.empty()) {
        CFFLog::Error("[LightmapBaker] No lightmap infos to save");
        return false;
    }

    if (!m_gpuTexture) {
        CFFLog::Error("[LightmapBaker] No atlas texture to save");
        return false;
    }

    // Create lightmap folder
    std::string absLightmapPath = FFPath::GetAbsolutePath(lightmapPath);
    std::filesystem::path folderPath(absLightmapPath);

    if (!std::filesystem::exists(folderPath)) {
        std::filesystem::create_directories(folderPath);
    }

    // Save data.bin
    std::string dataPath = absLightmapPath + "/data.bin";
    {
        std::ofstream file(dataPath, std::ios::binary);
        if (!file) {
            CFFLog::Error("[LightmapBaker] Failed to create file: %s", dataPath.c_str());
            return false;
        }

        SLightmapDataHeader header;
        header.infoCount = static_cast<uint32_t>(m_lightmapInfos.size());
        header.atlasWidth = static_cast<uint32_t>(m_atlasWidth);
        header.atlasHeight = static_cast<uint32_t>(m_atlasHeight);

        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(m_lightmapInfos.data()),
                   m_lightmapInfos.size() * sizeof(SLightmapInfo));

        CFFLog::Info("[LightmapBaker] Saved %d lightmap infos to: %s",
                     static_cast<int>(m_lightmapInfos.size()), dataPath.c_str());
    }

    // Save atlas.ktx2
    std::string atlasPath = absLightmapPath + "/atlas.ktx2";
    if (!CKTXExporter::Export2DTextureToKTX2(m_gpuTexture.get(), atlasPath)) {
        CFFLog::Error("[LightmapBaker] Failed to export atlas texture: %s", atlasPath.c_str());
        return false;
    }

    CFFLog::Info("[LightmapBaker] Saved lightmap to: %s", lightmapPath.c_str());
    return true;
}