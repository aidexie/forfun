#include "LightmapBaker.h"
#include "LightmapUV2.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Rendering/RayTracing/PathTraceBaker.h"
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

    // Step 5: Post-process (dilation)
    reportProgress(0.95f, "Post-processing (dilation)");
    Dilate(4);

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

        // Get world-space AABB
        XMMATRIX worldMatrix = transform->WorldMatrix();

        // Simple AABB estimation based on unit cube transformed
        XMFLOAT3 boundsMin = {-0.5f, -0.5f, -0.5f};
        XMFLOAT3 boundsMax = {0.5f, 0.5f, 0.5f};

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

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];
        auto* obj = world.Get(entry.meshRendererIndex);
        if (!obj) continue;

        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();
        if (!meshRenderer || !transform) continue;

        // TODO: Get actual mesh data with UV2
        // For now, create a simple placeholder
        // In production, we'd get this from MeshResourceManager

        // Create simple quad as placeholder
        std::vector<XMFLOAT3> positions = {
            {-0.5f, 0.0f, -0.5f},
            { 0.5f, 0.0f, -0.5f},
            { 0.5f, 0.0f,  0.5f},
            {-0.5f, 0.0f,  0.5f},
        };
        std::vector<XMFLOAT3> normals = {
            {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}
        };
        std::vector<XMFLOAT2> uv2 = {
            {0, 0}, {1, 0}, {1, 1}, {0, 1}
        };
        std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

        m_rasterizer.RasterizeMesh(
            positions, normals, uv2, indices,
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
    const auto& texels = m_rasterizer.GetTexels();
    int texelCount = static_cast<int>(texels.size());

    m_irradiance.resize(texelCount);

    // Initialize path tracer
    CPathTraceBaker baker;
    SPathTraceConfig ptConfig;
    ptConfig.samplesPerVoxel = config.samplesPerTexel;
    ptConfig.maxBounces = config.maxBounces;

    if (!baker.Initialize(scene, ptConfig)) {
        CFFLog::Error("[LightmapBaker] Failed to initialize path tracer");
        return false;
    }

    // Bake each valid texel
    int validCount = 0;
    int progressInterval = std::max(1, texelCount / 100);

    for (int i = 0; i < texelCount; i++) {
        if (!texels[i].valid) {
            m_irradiance[i] = {0, 0, 0, 0};
            continue;
        }

        // Bake irradiance at this texel
        std::array<XMFLOAT3, 9> sh;
        baker.BakeVoxel(texels[i].worldPos, scene, sh);

        // Convert SH to irradiance in the normal direction
        // For diffuse, we only need the DC term (L0) and directional term (L1)
        // E(n) ≈ π * (c0 * Y0 + c1 * Y1)
        // Simplified: just use L0 term for now
        float irradianceScale = 3.14159f;
        m_irradiance[i] = {
            sh[0].x * irradianceScale,
            sh[0].y * irradianceScale,
            sh[0].z * irradianceScale,
            1.0f
        };

        validCount++;

        // Report progress
        if (i % progressInterval == 0) {
            float progress = 0.30f + 0.65f * (static_cast<float>(i) / texelCount);
            reportProgress(progress, "Baking irradiance");
        }
    }

    CFFLog::Info("[LightmapBaker] Baked %d texels", validCount);
    baker.Shutdown();

    return true;
}

void CLightmapBaker::Dilate(int radius)
{
    auto& texels = m_rasterizer.GetTexelsMutable();
    std::vector<XMFLOAT4> dilated = m_irradiance;

    int width = m_rasterizer.GetWidth();
    int height = m_rasterizer.GetHeight();

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            if (texels[idx].valid) continue;  // Already valid

            // Search for nearest valid neighbor
            float accumR = 0, accumG = 0, accumB = 0;
            int count = 0;

            for (int r = 1; r <= radius && count == 0; r++) {
                for (int dy = -r; dy <= r; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        // Only check border of current radius
                        if (std::abs(dx) != r && std::abs(dy) != r) continue;

                        int nx = x + dx;
                        int ny = y + dy;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;

                        int nidx = ny * width + nx;
                        if (texels[nidx].valid) {
                            accumR += m_irradiance[nidx].x;
                            accumG += m_irradiance[nidx].y;
                            accumB += m_irradiance[nidx].z;
                            count++;
                        }
                    }
                }
            }

            if (count > 0) {
                dilated[idx] = {
                    accumR / count,
                    accumG / count,
                    accumB / count,
                    1.0f
                };
            }
        }
    }

    m_irradiance = dilated;
}

RHI::TexturePtr CLightmapBaker::CreateGPUTexture()
{
    if (m_irradiance.empty() || m_atlasWidth == 0 || m_atlasHeight == 0) {
        CFFLog::Error("[LightmapBaker] No data to create GPU texture");
        return nullptr;
    }

    // TODO: Implement GPU texture creation
    // Need to include proper RHI headers and use CreateTexture
    CFFLog::Info("[LightmapBaker] GPU texture creation not yet implemented");
    return nullptr;
}
