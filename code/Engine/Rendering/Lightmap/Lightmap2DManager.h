#pragma once
#include "LightmapTypes.h"
#include "RHI/RHIPointers.h"
#include <vector>
#include <string>

// Forward declarations
namespace RHI {
    class ICommandList;
}

// ============================================
// Lightmap 2D Manager
// ============================================
// Manages runtime 2D lightmap data (atlas texture + per-object scaleOffset)
// Owned by CScene

class CLightmap2DManager {
public:
    CLightmap2DManager() = default;
    ~CLightmap2DManager() = default;

    // ============================================
    // Save (called after baking in Editor)
    // ============================================

    // Save lightmap data to disk
    // lightmapPath: "scenes/MyScene.lightmap" (folder will be created)
    // infos: per-object lightmap info (scaleOffset)
    // atlasTexture: baked atlas texture (will be saved as atlas.ktx2)
    bool SaveLightmap(
        const std::string& lightmapPath,
        const std::vector<SLightmapInfo>& infos,
        RHI::ITexture* atlasTexture
    );

    // ============================================
    // Load (called at runtime)
    // ============================================

    // Load lightmap data from disk
    bool LoadLightmap(const std::string& lightmapPath);

    // Unload current lightmap
    void UnloadLightmap();

    // ============================================
    // Bind to command list (called in SceneRenderer)
    // ============================================

    // Bind lightmap resources to shader (t16, t17, b7)
    void Bind(RHI::ICommandList* cmdList);

    // ============================================
    // Query
    // ============================================

    bool IsLoaded() const { return m_isLoaded; }

    RHI::ITexture* GetAtlasTexture() const { return m_atlasTexture.get(); }
    RHI::IBuffer* GetScaleOffsetBuffer() const { return m_scaleOffsetBuffer.get(); }

    const SLightmapInfo* GetLightmapInfo(int index) const;
    int GetLightmapInfoCount() const { return static_cast<int>(m_lightmapInfos.size()); }

private:
    // Save helpers
    bool SaveLightmapData(const std::string& dataPath, const std::vector<SLightmapInfo>& infos);
    bool SaveAtlasTexture(const std::string& atlasPath, RHI::ITexture* texture);

    // Load helpers
    bool LoadLightmapData(const std::string& dataPath);
    bool LoadAtlasTexture(const std::string& atlasPath);
    bool CreateScaleOffsetBuffer();

private:
    bool m_isLoaded = false;

    // Runtime data
    std::vector<SLightmapInfo> m_lightmapInfos;
    RHI::TexturePtr m_atlasTexture;
    RHI::BufferPtr m_scaleOffsetBuffer;  // StructuredBuffer<float4>
};
