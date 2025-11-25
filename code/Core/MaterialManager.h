#pragma once

#include "MaterialAsset.h"
#include <memory>
#include <unordered_map>
#include <string>

/**
 * Material Manager - Singleton for managing material assets
 *
 * Responsibilities:
 * - Load material assets from .mat files
 * - Cache materials to avoid duplicate loading
 * - Create new materials programmatically
 * - Provide default material fallback
 */
class CMaterialManager {
public:
    static CMaterialManager& Instance();

    // Delete copy/move constructors
    CMaterialManager(const CMaterialManager&) = delete;
    CMaterialManager& operator=(const CMaterialManager&) = delete;

    /**
     * Load a material from file (with caching)
     * @param path Relative path from assets directory (e.g., "materials/wood.mat")
     * @return Pointer to material, or default material if load fails
     */
    CMaterialAsset* Load(const std::string& path);

    /**
     * Create a new material programmatically
     * @param name Material name (used as cache key)
     * @return Pointer to newly created material
     */
    CMaterialAsset* Create(const std::string& name);

    /**
     * Get default material (white, non-metallic, medium roughness)
     */
    CMaterialAsset* GetDefault();

    /**
     * Check if a material is already loaded
     */
    bool IsLoaded(const std::string& path) const;

    /**
     * Clear all cached materials (useful for hot-reload)
     */
    void Clear();

private:
    CMaterialManager();
    ~CMaterialManager() = default;

    std::unordered_map<std::string, std::unique_ptr<CMaterialAsset>> m_materials;
    std::unique_ptr<CMaterialAsset> m_defaultMaterial;

    std::string ResolveFullPath(const std::string& relativePath) const;
};
