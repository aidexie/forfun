#pragma once
#include "LightmapTypes.h"
#include "LightmapAtlas.h"
#include "LightmapRasterizer.h"
#include "Lightmap2DGPUBaker.h"
#include "RHI/RHIPointers.h"
#include <vector>
#include <DirectXMath.h>
#include <functional>

class CScene;

// ============================================
// Lightmap Baker
// ============================================
// Main class for baking lightmaps.
// Orchestrates UV2 generation, atlas packing, rasterization, and baking.

class CLightmapBaker {
public:
    CLightmapBaker();
    ~CLightmapBaker();

    // ============================================
    // Configuration
    // ============================================
    struct Config {
        SLightmapAtlasConfig atlasConfig;
        SLightmap2DBakeConfig bakeConfig;
        bool regenerateUV2 = true;  // Regenerate UV2 even if mesh already has it
    };

    // ============================================
    // Baking Pipeline
    // ============================================

    // Full bake: UV2 → atlas → rasterize → bake → assign indices → save to file
    // lightmapPath: "scenes/MyScene.lightmap" (folder will be created)
    bool Bake(CScene& scene, const Config& config, const std::string& lightmapPath);

    // ============================================
    // Results (for debugging/inspection only)
    // ============================================

    int GetAtlasWidth() const { return m_atlasWidth; }
    int GetAtlasHeight() const { return m_atlasHeight; }

    // ============================================
    // Progress Callback
    // ============================================
    using ProgressCallback = std::function<void(float progress, const char* stage)>;
    void SetProgressCallback(ProgressCallback callback) { m_progressCallback = callback; }

private:
    // Pipeline steps (internal)
    bool packAtlas(CScene& scene, const SLightmapAtlasConfig& config);
    bool rasterize(CScene& scene);
    bool bakeIrradiance(CScene& scene, const SLightmap2DBakeConfig& config);
    void assignLightmapIndices(CScene& scene);
    bool saveToFile(const std::string& lightmapPath);

    void reportProgress(float progress, const char* stage);

    // Baking data
    CLightmapAtlasBuilder m_atlasBuilder;
    CLightmapRasterizer m_rasterizer;
    CLightmap2DGPUBaker m_gpuBaker;  // Reused across bakes (avoids shader recompilation)
    RHI::TexturePtr m_gpuTexture;
    std::vector<SLightmapInfo> m_lightmapInfos;

    int m_atlasWidth = 0;
    int m_atlasHeight = 0;

    ProgressCallback m_progressCallback;
};
