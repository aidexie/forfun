#pragma once
#include "LightmapTypes.h"
#include "LightmapAtlas.h"
#include "LightmapRasterizer.h"
#include "RHI/RHIPointers.h"
#include <vector>
#include <DirectXMath.h>
#include <functional>

class CScene;
class CPathTraceBaker;

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

    // Full bake: UV2 generation + atlas packing + rasterization + baking
    bool Bake(CScene& scene, const Config& config);

    // Individual steps (for debugging or incremental workflow)
    bool GenerateUV2ForScene(CScene& scene, int texelsPerUnit);
    bool PackAtlas(CScene& scene, const SLightmapAtlasConfig& config);
    bool Rasterize(CScene& scene);
    bool BakeIrradiance(CScene& scene, const SLightmap2DBakeConfig& config);

    // ============================================
    // Results
    // ============================================

    // Get baked lightmap texture (GPU texture from GPU baker)
    RHI::ITexture* GetLightmapTexture() const { return m_gpuTexture.get(); }
    int GetAtlasWidth() const { return m_atlasWidth; }
    int GetAtlasHeight() const { return m_atlasHeight; }

    // Get lightmap info for each MeshRenderer (for shader binding)
    const std::vector<SLightmapInfo>& GetLightmapInfos() const { return m_lightmapInfos; }

    // ============================================
    // Progress Callback
    // ============================================
    using ProgressCallback = std::function<void(float progress, const char* stage)>;
    void SetProgressCallback(ProgressCallback callback) { m_progressCallback = callback; }

private:
    void reportProgress(float progress, const char* stage);

    // Baking data
    CLightmapAtlasBuilder m_atlasBuilder;
    CLightmapRasterizer m_rasterizer;
    RHI::TexturePtr m_gpuTexture;  // GPU-baked lightmap texture
    std::vector<SLightmapInfo> m_lightmapInfos;

    int m_atlasWidth = 0;
    int m_atlasHeight = 0;

    ProgressCallback m_progressCallback;
};
