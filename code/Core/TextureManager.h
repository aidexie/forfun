#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIPointers.h"
#include "TextureHandle.h"
#include <string>
#include <unordered_map>
#include <queue>
#include <memory>

/**
 * Texture Manager - Singleton for managing texture resources
 *
 * Responsibilities:
 * - Load textures from disk (PNG, JPG, TGA, DDS, KTX2)
 * - Cache textures to avoid duplicate loading
 * - Manage color space (sRGB vs Linear)
 * - Provide default textures (white, normal, black)
 * - Async loading with frame-budget control
 *
 * Ownership Model:
 * - Uses shared_ptr for texture ownership (UE4/UE5 style)
 * - Multiple systems can safely hold references to the same texture
 * - Textures are automatically released when all references are dropped
 *
 * Async Loading:
 * - LoadAsync() returns TextureHandlePtr immediately with placeholder
 * - Call Tick() at frame start to process pending loads
 * - TextureHandle automatically returns real texture when ready
 */
class CTextureManager {
public:
    static CTextureManager& Instance();

    // Delete copy/move constructors
    CTextureManager(const CTextureManager&) = delete;
    CTextureManager& operator=(const CTextureManager&) = delete;

    /**
     * Load a texture synchronously (BLOCKING - legacy API)
     * @param path Relative path from assets directory (e.g., "textures/wood_albedo.png")
     * @param srgb True for sRGB color space (albedo, emissive), false for linear (normal, metallic, roughness, AO)
     * @return Shared pointer to RHI Texture, or default texture if load fails
     */
    RHI::TextureSharedPtr Load(const std::string& path, bool srgb);

    /**
     * Load a texture asynchronously (NON-BLOCKING - preferred API)
     * @param path Relative path from assets directory
     * @param srgb True for sRGB color space
     * @return TextureHandlePtr that returns placeholder until texture is ready
     *
     * The returned handle will:
     * - Immediately return a placeholder texture via GetTexture()
     * - Return the real texture once loading completes
     * - Handle load failures gracefully (returns fallback texture)
     */
    TextureHandlePtr LoadAsync(const std::string& path, bool srgb);

    /**
     * Process pending texture loads (call at frame start)
     * @param maxLoadsPerFrame Maximum number of textures to load this frame (0 = unlimited)
     * @return Number of textures actually loaded this frame
     *
     * Each load includes: Disk I/O + GPU Upload + Mip Generation
     * Typical cost: 5-15ms per texture depending on size
     */
    uint32_t Tick(uint32_t maxLoadsPerFrame = 2);

    /**
     * Get number of textures waiting to be loaded
     */
    uint32_t GetPendingCount() const { return static_cast<uint32_t>(m_pendingLoads.size()); }

    /**
     * Check if any textures are still loading
     */
    bool HasPendingLoads() const { return !m_pendingLoads.empty(); }

    /**
     * Force load all pending textures (blocking)
     * Useful for loading screens or initialization
     */
    void FlushPendingLoads();

    /**
     * Get default white texture (1x1 white pixel, sRGB)
     */
    RHI::TextureSharedPtr GetDefaultWhite();

    /**
     * Get default normal map (1x1 pixel: RGB=(128,128,255) -> normal=(0,0,1))
     */
    RHI::TextureSharedPtr GetDefaultNormal();

    /**
     * Get default black texture (1x1 black pixel, linear)
     */
    RHI::TextureSharedPtr GetDefaultBlack();

    /**
     * Get placeholder texture (used during async loading)
     */
    RHI::TextureSharedPtr GetPlaceholder();

    /**
     * Check if a texture is already loaded
     */
    bool IsLoaded(const std::string& path) const;

    /**
     * Clear all cached textures
     */
    void Clear();

    /**
     * Shutdown - release all resources (call before RHI shutdown)
     */
    void Shutdown();

private:
    CTextureManager();
    ~CTextureManager() = default;

    // Cached texture entry (for sync API)
    struct CachedTexture {
        RHI::TextureSharedPtr texture;
        bool isSRGB;
    };

    // Async load request
    struct LoadRequest {
        std::string path;
        std::string fullPath;
        std::string cacheKey;
        bool srgb;
        TextureHandlePtr handle;
    };

    // Cache for sync-loaded textures
    std::unordered_map<std::string, CachedTexture> m_textures;

    // Cache for async-loaded texture handles
    std::unordered_map<std::string, TextureHandlePtr> m_handles;

    // Queue of pending async loads
    std::queue<LoadRequest> m_pendingLoads;

    // Default textures
    RHI::TextureSharedPtr m_defaultWhite;
    RHI::TextureSharedPtr m_defaultNormal;
    RHI::TextureSharedPtr m_defaultBlack;
    RHI::TextureSharedPtr m_placeholder;  // Checkerboard pattern for loading indication

    void CreateDefaultTextures();
    void CreatePlaceholderTexture();
    std::string ResolveFullPath(const std::string& relativePath) const;
    std::string MakeCacheKey(const std::string& path, bool srgb) const;
    RHI::ITexture* LoadTextureFromFile(const std::string& fullPath, bool srgb);

    // Process a single load request
    void ProcessLoadRequest(LoadRequest& request);
};
