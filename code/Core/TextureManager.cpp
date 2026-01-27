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
    CreatePlaceholderTexture();
    CFFLog::Info("TextureManager initialized");
}

std::string CTextureManager::MakeCacheKey(const std::string& path, bool srgb) const {
    return path + (srgb ? "|srgb" : "|linear");
}

RHI::TextureSharedPtr CTextureManager::Load(const std::string& path, bool srgb) {
    if (path.empty()) {
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    std::string cacheKey = MakeCacheKey(path, srgb);

    // Check cache
    auto it = m_textures.find(cacheKey);
    if (it != m_textures.end()) {
        return it->second.texture;
    }

    // Load from file (BLOCKING)
    std::string fullPath = ResolveFullPath(path);
    RHI::ITexture* texture = LoadTextureFromFile(fullPath, srgb);

    if (!texture) {
        CFFLog::Warning(("Failed to load texture: " + path + ", using default").c_str());
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    CFFLog::Info(("Loaded texture (sync): " + path + (srgb ? " (sRGB)" : " (Linear)")).c_str());

    // Cache it with shared_ptr
    CachedTexture cached;
    cached.texture = RHI::TextureSharedPtr(texture);
    cached.isSRGB = srgb;
    m_textures[cacheKey] = std::move(cached);

    return m_textures[cacheKey].texture;
}

TextureHandlePtr CTextureManager::LoadAsync(const std::string& path, bool srgb) {
    if (path.empty()) {
        // Return a handle that's immediately ready with default texture
        auto handle = std::make_shared<CTextureHandle>(
            srgb ? GetDefaultWhite() : GetDefaultBlack(), path, srgb);
        handle->SetReady(srgb ? GetDefaultWhite() : GetDefaultBlack());
        return handle;
    }

    std::string cacheKey = MakeCacheKey(path, srgb);

    // Check handle cache first (already loaded or pending)
    auto handleIt = m_handles.find(cacheKey);
    if (handleIt != m_handles.end()) {
        return handleIt->second;
    }

    // Check sync cache (loaded via Load())
    auto syncIt = m_textures.find(cacheKey);
    if (syncIt != m_textures.end()) {
        // Create handle that's immediately ready
        auto handle = std::make_shared<CTextureHandle>(m_placeholder, path, srgb);
        handle->SetReady(syncIt->second.texture);
        m_handles[cacheKey] = handle;
        return handle;
    }

    // Create new handle with placeholder
    auto handle = std::make_shared<CTextureHandle>(m_placeholder, path, srgb);
    m_handles[cacheKey] = handle;

    // Queue load request
    LoadRequest request;
    request.path = path;
    request.fullPath = ResolveFullPath(path);
    request.cacheKey = cacheKey;
    request.srgb = srgb;
    request.handle = handle;
    m_pendingLoads.push(std::move(request));

    CFFLog::Info(("Queued async load: " + path + " (pending: " +
                  std::to_string(m_pendingLoads.size()) + ")").c_str());

    return handle;
}

uint32_t CTextureManager::Tick(uint32_t maxLoadsPerFrame) {
    if (m_pendingLoads.empty()) {
        return 0;
    }

    uint32_t loadCount = 0;
    uint32_t limit = (maxLoadsPerFrame == 0) ? UINT32_MAX : maxLoadsPerFrame;

    while (!m_pendingLoads.empty() && loadCount < limit) {
        LoadRequest request = std::move(m_pendingLoads.front());
        m_pendingLoads.pop();

        ProcessLoadRequest(request);
        loadCount++;
    }

    if (loadCount > 0) {
        CFFLog::Info(("TextureManager::Tick processed " + std::to_string(loadCount) +
                      " loads, " + std::to_string(m_pendingLoads.size()) + " remaining").c_str());
    }

    return loadCount;
}

void CTextureManager::FlushPendingLoads() {
    uint32_t count = Tick(0);  // 0 = unlimited
    if (count > 0) {
        CFFLog::Info(("TextureManager::FlushPendingLoads: loaded " +
                      std::to_string(count) + " textures").c_str());
    }
}

void CTextureManager::ProcessLoadRequest(LoadRequest& request) {
    request.handle->SetState(CTextureHandle::EState::Loading);

    // Load from disk + GPU upload + mip generation
    RHI::ITexture* texture = LoadTextureFromFile(request.fullPath, request.srgb);

    if (!texture) {
        CFFLog::Warning(("Failed to load texture (async): " + request.path).c_str());
        request.handle->SetFailed();

        // Use default texture as fallback
        RHI::TextureSharedPtr fallback = request.srgb ? GetDefaultWhite() : GetDefaultBlack();
        request.handle->SetReady(fallback);
        return;
    }

    // Wrap in shared_ptr
    RHI::TextureSharedPtr texturePtr(texture);

    // Also add to sync cache for compatibility
    CachedTexture cached;
    cached.texture = texturePtr;
    cached.isSRGB = request.srgb;
    m_textures[request.cacheKey] = std::move(cached);

    // Mark handle as ready
    request.handle->SetReady(texturePtr);

    CFFLog::Info(("Loaded texture (async): " + request.path +
                  (request.srgb ? " (sRGB)" : " (Linear)")).c_str());
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

RHI::TextureSharedPtr CTextureManager::GetDefaultBlack3D() {
    return m_defaultBlack3D;
}

RHI::TextureSharedPtr CTextureManager::GetPlaceholder() {
    return m_placeholder;
}

bool CTextureManager::IsLoaded(const std::string& path) const {
    std::string keySRGB = path + "|srgb";
    std::string keyLinear = path + "|linear";
    return m_textures.find(keySRGB) != m_textures.end() ||
           m_textures.find(keyLinear) != m_textures.end();
}

void CTextureManager::Clear() {
    // Clear pending loads queue
    while (!m_pendingLoads.empty()) {
        m_pendingLoads.pop();
    }

    m_textures.clear();
    m_handles.clear();
    CFFLog::Info("TextureManager cache cleared");
}

void CTextureManager::Shutdown() {
    // Clear pending loads queue
    while (!m_pendingLoads.empty()) {
        m_pendingLoads.pop();
    }

    m_textures.clear();
    m_handles.clear();
    m_defaultWhite.reset();
    m_defaultNormal.reset();
    m_defaultBlack.reset();
    m_defaultBlack3D.reset();
    m_placeholder.reset();
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

    // Create 1x1x1 3D black texture for volumetric fallbacks (VolumetricLightmap, etc.)
    {
        uint32_t blackVoxel = 0xFF000000; // RGBA: (0,0,0,255)
        RHI::TextureDesc desc3D;
        desc3D.width = 1;
        desc3D.height = 1;
        desc3D.depth = 1;
        desc3D.mipLevels = 1;
        desc3D.format = RHI::ETextureFormat::R8G8B8A8_UNORM;
        desc3D.usage = RHI::ETextureUsage::ShaderResource;
        desc3D.dimension = RHI::ETextureDimension::Tex3D;
        desc3D.debugName = "DefaultBlack3D";
        m_defaultBlack3D = RHI::TextureSharedPtr(rhiCtx->CreateTexture(desc3D, &blackVoxel));
    }

    CFFLog::Info("Created default textures (white, normal, black, black3D)");
}

void CTextureManager::CreatePlaceholderTexture() {
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) {
        CFFLog::Error("Failed to create placeholder texture: RHI context not available");
        return;
    }

    // Create 8x8 checkerboard pattern (magenta/black - visually obvious)
    constexpr uint32_t SIZE = 8;
    constexpr uint32_t MAGENTA = 0xFFFF00FF;  // ABGR: magenta
    constexpr uint32_t BLACK   = 0xFF000000;  // ABGR: black

    uint32_t pixels[SIZE * SIZE];
    for (uint32_t y = 0; y < SIZE; ++y) {
        for (uint32_t x = 0; x < SIZE; ++x) {
            // 2x2 checkerboard pattern
            bool isEven = ((x / 2) + (y / 2)) % 2 == 0;
            pixels[y * SIZE + x] = isEven ? MAGENTA : BLACK;
        }
    }

    RHI::TextureDesc desc;
    desc.width = SIZE;
    desc.height = SIZE;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = RHI::ETextureFormat::R8G8B8A8_UNORM_SRGB;
    desc.usage = RHI::ETextureUsage::ShaderResource;
    desc.debugName = "PlaceholderTexture";

    RHI::ITexture* texture = rhiCtx->CreateTexture(desc, pixels);
    if (texture) {
        m_placeholder = RHI::TextureSharedPtr(texture);
        CFFLog::Info("Created placeholder texture (8x8 checkerboard)");
    } else {
        // Fallback to default white if placeholder creation fails
        m_placeholder = m_defaultWhite;
        CFFLog::Warning("Failed to create placeholder texture, using default white");
    }
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
