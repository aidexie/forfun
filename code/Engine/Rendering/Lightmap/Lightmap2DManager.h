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
// Note: Saving is handled by CLightmapBaker::SaveToFile()

class CLightmap2DManager {
public:
    CLightmap2DManager() = default;
    ~CLightmap2DManager() = default;

    // ============================================
    // Load (called at runtime)
    // ============================================

    // Load lightmap data from disk
    bool LoadLightmap(const std::string& lightmapPath);

    // Unload current lightmap
    void UnloadLightmap();

    // ============================================
    // Direct Data Transfer (from baker, no file I/O)
    // ============================================

    // Set baked data directly from CLightmapBaker
    // This avoids the round-trip to disk after baking
    // atlasTexture: ownership transferred via std::move
    // infos: copied
    void SetBakedData(
        RHI::TexturePtr atlasTexture,
        const std::vector<SLightmapInfo>& infos
    );

    // ============================================
    // Bind to command list (called in SceneRenderer)
    // ============================================

    // Bind lightmap resources to shader (t16, t17)
    void Bind(RHI::ICommandList* cmdList);

    // ============================================
    // Hot-Reload
    // ============================================

    // Reload lightmap from last loaded path
    // Returns false if no lightmap was previously loaded
    bool ReloadLightmap();

    // ============================================
    // Query
    // ============================================

    bool IsLoaded() const { return m_isLoaded; }
    const std::string& GetLoadedPath() const { return m_loadedPath; }

    RHI::ITexture* GetAtlasTexture() const;
    RHI::IBuffer* GetScaleOffsetBuffer() const { return m_scaleOffsetBuffer.get(); }

    const SLightmapInfo* GetLightmapInfo(int index) const;
    int GetLightmapInfoCount() const { return static_cast<int>(m_lightmapInfos.size()); }

private:
    // Load helpers
    bool LoadLightmapData(const std::string& dataPath);
    bool LoadAtlasTexture(const std::string& atlasPath);
    bool CreateScaleOffsetBuffer();

private:
    bool m_isLoaded = false;
    std::string m_loadedPath;  // Track loaded path for hot-reload

    // Runtime data
    std::vector<SLightmapInfo> m_lightmapInfos;

    // Atlas texture - can be owned (from baker) or shared (from TextureManager)
    RHI::TexturePtr m_atlasTextureOwned;           // Unique ownership (from baker)
    RHI::TextureSharedPtr m_atlasTextureShared;    // Shared ownership (from TextureManager)

    RHI::BufferPtr m_scaleOffsetBuffer;  // StructuredBuffer<float4>
};
