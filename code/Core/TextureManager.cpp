#include "TextureManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "FFLog.h"
#include "DebugPaths.h"
#include "PathManager.h"
#include "Loader/TextureLoader.h"
#include "Loader/KTXLoader.h"
#include <codecvt>
#include <locale>
#include <algorithm>

CTextureManager& CTextureManager::Instance() {
    static CTextureManager instance;
    return instance;
}

CTextureManager::CTextureManager() {
    CreateDefaultTextures();
    CFFLog::Info("TextureManager initialized");
}

RHI::TextureSharedPtr CTextureManager::Load(const std::string& path, bool srgb) {
    if (path.empty()) {
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    // Create cache key with sRGB flag
    std::string cacheKey = path + (srgb ? "|srgb" : "|linear");

    // Check cache
    auto it = m_textures.find(cacheKey);
    if (it != m_textures.end()) {
        return it->second.texture;
    }

    // Load from file
    std::string fullPath = ResolveFullPath(path);
    RHI::ITexture* texture = LoadTextureFromFile(fullPath, srgb);

    if (!texture) {
        CFFLog::Warning(("Failed to load texture: " + path + ", using default").c_str());
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    CFFLog::Info(("Loaded texture: " + path + (srgb ? " (sRGB)" : " (Linear)")).c_str());

    // Cache it with shared_ptr
    CachedTexture cached;
    cached.texture = RHI::TextureSharedPtr(texture);
    cached.isSRGB = srgb;
    m_textures[cacheKey] = std::move(cached);

    return m_textures[cacheKey].texture;
}

RHI::TextureSharedPtr CTextureManager::GetDefaultWhite() {
    return m_defaultWhite;
}

RHI::TextureSharedPtr CTextureManager::GetDefaultNormal() {
    return m_defaultNormal;
}

RHI::TextureSharedPtr CTextureManager::GetDefaultBlack() {
    return m_defaultBlack;
}

bool CTextureManager::IsLoaded(const std::string& path) const {
    std::string keySRGB = path + "|srgb";
    std::string keyLinear = path + "|linear";
    return m_textures.find(keySRGB) != m_textures.end() ||
           m_textures.find(keyLinear) != m_textures.end();
}

void CTextureManager::Clear() {
    m_textures.clear();
    CFFLog::Info("TextureManager cache cleared");
}

void CTextureManager::Shutdown() {
    m_textures.clear();
    m_defaultWhite.reset();
    m_defaultNormal.reset();
    m_defaultBlack.reset();
    CFFLog::Info("TextureManager shutdown complete");
}

void CTextureManager::CreateDefaultTextures() {
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) {
        CFFLog::Error("Failed to create default textures: RHI context not available");
        return;
    }

    // Lambda to create solid color 1x1 texture using RHI
    auto MakeSolidTexture = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a, RHI::ETextureFormat fmt) -> RHI::ITexture* {
        // Pack RGBA into uint32
        uint32_t pixel = (uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24));

        RHI::TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = fmt;
        desc.usage = RHI::ETextureUsage::ShaderResource;
        desc.debugName = "DefaultTexture";

        return rhiCtx->CreateTexture(desc, &pixel);
    };

    m_defaultWhite = RHI::TextureSharedPtr(MakeSolidTexture(255, 255, 255, 255, RHI::ETextureFormat::R8G8B8A8_UNORM_SRGB));
    m_defaultNormal = RHI::TextureSharedPtr(MakeSolidTexture(128, 128, 255, 255, RHI::ETextureFormat::R8G8B8A8_UNORM));
    m_defaultBlack = RHI::TextureSharedPtr(MakeSolidTexture(0, 0, 0, 255, RHI::ETextureFormat::R8G8B8A8_UNORM));

    CFFLog::Info("Created default textures (white, normal, black)");
}

std::string CTextureManager::ResolveFullPath(const std::string& relativePath) const {
    return FFPath::GetAbsolutePath(relativePath);
}

RHI::ITexture* CTextureManager::LoadTextureFromFile(const std::string& fullPath, bool srgb) {
    // Get file extension (case-insensitive)
    std::string ext;
    size_t dotPos = fullPath.rfind('.');
    if (dotPos != std::string::npos) {
        ext = fullPath.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    // Route to appropriate loader based on extension
    if (ext == ".ktx2" || ext == ".ktx") {
        // KTX2 loader (ignores srgb flag - format is embedded in file)
        return CKTXLoader::Load2DTextureFromKTX2(fullPath);
    }

    // Default: WIC loader for PNG/JPG/BMP/TGA/etc.
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wpath = converter.from_bytes(fullPath);
    return LoadTextureWIC(wpath, srgb);
}
