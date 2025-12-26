#include "LightmapBaker.h"
#include "LightmapUV2.h"
#include "Lightmap2DGPUBaker.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Rendering/RayTracing/SceneGeometryExport.h"
#include "Core/FFLog.h"
#include "Core/Mesh.h"
#include <algorithm>
#include <cmath>

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

bool CLightmapBaker::Bake(CScene& scene, const Config& config)
{
    reportProgress(0.0f, "Starting lightmap bake");

    // Step 1: Generate UV2
    reportProgress(0.05f, "Generating UV2 coordinates");
    if (!GenerateUV2ForScene(scene, config.atlasConfig.texelsPerUnit)) {
        CFFLog::Error("[LightmapBaker] UV2 generation failed");
        return false;
    }

    // Step 2: Pack atlas
    reportProgress(0.15f, "Packing atlas");
    if (!PackAtlas(scene, config.atlasConfig)) {
        CFFLog::Error("[LightmapBaker] Atlas packing failed");
        return false;
    }

    // Step 3: Rasterize
    reportProgress(0.25f, "Rasterizing meshes");
    if (!Rasterize(scene)) {
        CFFLog::Error("[LightmapBaker] Rasterization failed");
        return false;
    }

    // Step 4: Bake irradiance
    reportProgress(0.30f, "Baking irradiance");
    if (!BakeIrradiance(scene, config.bakeConfig)) {
        CFFLog::Error("[LightmapBaker] Baking failed");
        return false;
    }

    reportProgress(1.0f, "Bake complete");
    return true;
}

bool CLightmapBaker::GenerateUV2ForScene(CScene& scene, int texelsPerUnit)
{
    // TODO: Implement per-mesh UV2 generation
    // For now, we assume meshes already have UV2 or will use UV1
    CFFLog::Info("[LightmapBaker] UV2 generation - using existing UVs");
    return true;
}

bool CLightmapBaker::PackAtlas(CScene& scene, const SLightmapAtlasConfig& config)
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

bool CLightmapBaker::Rasterize(CScene& scene)
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

        // Get mesh data from ray tracing cache
        // Note: Mesh must be loaded with cacheForRayTracing=true
        const SRayTracingMeshData* meshData = meshCache.GetMeshData(meshRenderer->path, 0);
        if (!meshData) {
            CFFLog::Warning("[LightmapBaker] Mesh data not cached: %s (skipping)", meshRenderer->path.c_str());
            continue;
        }

        // Generate UV2 for this mesh using xatlas
        SUV2GenerationResult uv2Result = GenerateUV2(
            meshData->positions,
            meshData->normals,
            std::vector<XMFLOAT2>(meshData->positions.size(), XMFLOAT2{0, 0}),  // No UV1 needed
            meshData->indices,
            m_atlasBuilder.GetAtlas().GetAtlasResolution() / 4  // texelsPerUnit based on atlas
        );

        if (!uv2Result.success) {
            CFFLog::Warning("[LightmapBaker] UV2 generation failed for mesh: %s", meshRenderer->path.c_str());
            continue;
        }

        // Rasterize using generated UV2 data
        m_rasterizer.RasterizeMesh(
            uv2Result.positions,
            uv2Result.normals,
            uv2Result.uv2,
            uv2Result.indices,
            transform->WorldMatrix(),
            entry.atlasX, entry.atlasY,
            entry.width, entry.height
        );
    }

    int validCount = m_rasterizer.GetValidTexelCount();
    CFFLog::Info("[LightmapBaker] Rasterized %d valid texels", validCount);

    return true;
}

bool CLightmapBaker::BakeIrradiance(CScene& scene, const SLightmap2DBakeConfig& config)
{
    // Use GPU baker (DXR-based)
    CLightmap2DGPUBaker gpuBaker;

    if (!gpuBaker.Initialize()) {
        CFFLog::Error("[LightmapBaker] Failed to initialize GPU baker");
        return false;
    }

    if (!gpuBaker.IsAvailable()) {
        CFFLog::Error("[LightmapBaker] DXR not available for GPU baking");
        return false;
    }

    // Configure GPU bake
    SLightmap2DGPUBakeConfig gpuConfig;
    gpuConfig.samplesPerTexel = config.samplesPerTexel;
    gpuConfig.maxBounces = config.maxBounces;
    gpuConfig.skyIntensity = 1.0f;
    gpuConfig.progressCallback = [this](float progress, const char* stage) {
        // Map GPU baker progress (0-1) to our progress range (0.30 - 0.95)
        float mappedProgress = 0.30f + progress * 0.65f;
        reportProgress(mappedProgress, stage);
    };

    // Bake using GPU
    m_gpuTexture = gpuBaker.BakeLightmap(scene, m_rasterizer, gpuConfig);

    if (!m_gpuTexture) {
        CFFLog::Error("[LightmapBaker] GPU baking failed");
        return false;
    }

    CFFLog::Info("[LightmapBaker] GPU baking complete (%dx%d)", m_atlasWidth, m_atlasHeight);

    gpuBaker.Shutdown();
    return true;
}