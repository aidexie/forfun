#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIPointers.h"
#include <string>
#include <unordered_map>
#include <memory>

/**
 * Texture Manager - Singleton for managing texture resources
 *
 * Responsibilities:
 * - Load textures from disk (PNG, JPG, TGA, DDS, KTX2)
 * - Cache textures to avoid duplicate loading
 * - Manage color space (sRGB vs Linear)
 * - Provide default textures (white, normal, black)
 *
 * Ownership Model:
 * - Uses shared_ptr for texture ownership (UE4/UE5 style)
 * - Multiple systems can safely hold references to the same texture
 * - Textures are automatically released when all references are dropped
 */
class CTextureManager {
public:
    static CTextureManager& Instance();

    // Delete copy/move constructors
    CTextureManager(const CTextureManager&) = delete;
    CTextureManager& operator=(const CTextureManager&) = delete;

    /**
     * Load a texture from file (with caching)
     * @param path Relative path from assets directory (e.g., "textures/wood_albedo.png")
     * @param srgb True for sRGB color space (albedo, emissive), false for linear (normal, metallic, roughness, AO)
     * @return Shared pointer to RHI Texture, or default texture if load fails
     */
    RHI::TextureSharedPtr Load(const std::string& path, bool srgb);

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

    struct CachedTexture {
        RHI::TextureSharedPtr texture;
        bool isSRGB;
    };

    std::unordered_map<std::string, CachedTexture> m_textures;

    RHI::TextureSharedPtr m_defaultWhite;
    RHI::TextureSharedPtr m_defaultNormal;
    RHI::TextureSharedPtr m_defaultBlack;

    void CreateDefaultTextures();
    std::string ResolveFullPath(const std::string& relativePath) const;
    RHI::ITexture* LoadTextureFromFile(const std::string& fullPath, bool srgb);
};
