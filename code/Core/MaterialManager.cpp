#include "MaterialManager.h"
#include "FFLog.h"
#include "DebugPaths.h"

CMaterialManager& CMaterialManager::Instance() {
    static CMaterialManager instance;
    return instance;
}

CMaterialManager::CMaterialManager() {
    // Create default material
    m_defaultMaterial = std::make_unique<CMaterialAsset>("__default__");
    m_defaultMaterial->albedo = {1.0f, 1.0f, 1.0f};
    m_defaultMaterial->metallic = 0.0f;
    m_defaultMaterial->roughness = 0.5f;
    m_defaultMaterial->ao = 1.0f;
    m_defaultMaterial->emissive = {0.0f, 0.0f, 0.0f};
    m_defaultMaterial->emissiveStrength = 0.0f;

    CFFLog::Info("MaterialManager initialized");
}

CMaterialAsset* CMaterialManager::Load(const std::string& path) {
    if (path.empty()) {
        return GetDefault();
    }

    // Check cache
    auto it = m_materials.find(path);
    if (it != m_materials.end()) {
        return it->second.get();
    }

    // Load from file
    std::string fullPath = ResolveFullPath(path);
    auto material = std::make_unique<CMaterialAsset>();

    if (!material->LoadFromFile(fullPath)) {
        CFFLog::Warning(("Failed to load material: " + path + ", using default").c_str());
        return GetDefault();
    }

    CFFLog::Info(("Loaded material: " + path).c_str());
    CMaterialAsset* ptr = material.get();
    m_materials[path] = std::move(material);
    return ptr;
}

CMaterialAsset* CMaterialManager::Create(const std::string& name) {
    // Check if already exists
    auto it = m_materials.find(name);
    if (it != m_materials.end()) {
        CFFLog::Warning(("Material already exists: " + name).c_str());
        return it->second.get();
    }

    // Create new material
    auto material = std::make_unique<CMaterialAsset>(name);
    CMaterialAsset* ptr = material.get();
    m_materials[name] = std::move(material);

    CFFLog::Info(("Created material: " + name).c_str());
    return ptr;
}

CMaterialAsset* CMaterialManager::GetDefault() {
    return m_defaultMaterial.get();
}

bool CMaterialManager::IsLoaded(const std::string& path) const {
    return m_materials.find(path) != m_materials.end();
}

void CMaterialManager::Clear() {
    m_materials.clear();
    CFFLog::Info("MaterialManager cache cleared");
}

std::string CMaterialManager::ResolveFullPath(const std::string& relativePath) const {
    // Resolve relative to assets directory
    // E.g., "materials/wood.mat" -> "E:/forfun/assets/materials/wood.mat"
    return std::string("E:/forfun/assets/") + relativePath;
}
