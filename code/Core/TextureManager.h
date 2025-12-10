#pragma once

#include "RHI/RHIResources.h"
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
     * @return RHI Texture, or default texture if load fails
     */
    RHI::ITexture* Load(const std::string& path, bool srgb);

    /**
     * Get default white texture (1x1 white pixel, sRGB)
     */
    RHI::ITexture* GetDefaultWhite();

    /**
     * Get default normal map (1x1 pixel: RGB=(128,128,255) -> normal=(0,0,1))
     */
    RHI::ITexture* GetDefaultNormal();

    /**
     * Get default black texture (1x1 black pixel, linear)
     */
    RHI::ITexture* GetDefaultBlack();

    /**
     * Check if a texture is already loaded
     */
    bool IsLoaded(const std::string& path) const;

    /**
     * Clear all cached textures
     */
    void Clear();

private:
    CTextureManager();
    ~CTextureManager() = default;

    struct CachedTexture {
        std::unique_ptr<RHI::ITexture> texture;
        bool isSRGB;
    };

    std::unordered_map<std::string, CachedTexture> m_textures;

    std::unique_ptr<RHI::ITexture> m_defaultWhite;
    std::unique_ptr<RHI::ITexture> m_defaultNormal;
    std::unique_ptr<RHI::ITexture> m_defaultBlack;

    void CreateDefaultTextures();
    std::string ResolveFullPath(const std::string& relativePath) const;
    RHI::ITexture* LoadTextureFromFile(const std::string& fullPath, bool srgb);
};
