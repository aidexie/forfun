#include "TextureManager.h"
#include "DX11Context.h"
#include "FFLog.h"
#include "DebugPaths.h"
#include "Loader/TextureLoader.h"
#include <codecvt>
#include <locale>

CTextureManager& CTextureManager::Instance() {
    static CTextureManager instance;
    return instance;
}

CTextureManager::CTextureManager() {
    CreateDefaultTextures();
    CFFLog::Info("TextureManager initialized");
}

ID3D11ShaderResourceView* CTextureManager::Load(const std::string& path, bool srgb) {
    if (path.empty()) {
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    // Create cache key with sRGB flag
    std::string cacheKey = path + (srgb ? "|srgb" : "|linear");

    // Check cache
    auto it = m_textures.find(cacheKey);
    if (it != m_textures.end()) {
        return it->second.srv.Get();
    }

    // Load from file
    std::string fullPath = ResolveFullPath(path);
    ComPtr<ID3D11ShaderResourceView> srv;

    if (!LoadTextureFromFile(fullPath, srgb, srv)) {
        CFFLog::Warning(("Failed to load texture: " + path + ", using default").c_str());
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    CFFLog::Info(("Loaded texture: " + path + (srgb ? " (sRGB)" : " (Linear)")).c_str());

    // Cache it
    CachedTexture cached;
    cached.srv = srv;
    cached.isSRGB = srgb;
    m_textures[cacheKey] = cached;

    return srv.Get();
}

ID3D11ShaderResourceView* CTextureManager::GetDefaultWhite() {
    return m_defaultWhite.Get();
}

ID3D11ShaderResourceView* CTextureManager::GetDefaultNormal() {
    return m_defaultNormal.Get();
}

ID3D11ShaderResourceView* CTextureManager::GetDefaultBlack() {
    return m_defaultBlack.Get();
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

void CTextureManager::CreateDefaultTextures() {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) {
        CFFLog::Error("Failed to create default textures: D3D11 device not available");
        return;
    }

    // Create default white texture (1x1 white pixel, sRGB)
    {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_IMMUTABLE;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        UINT whitePixel = 0xFFFFFFFF;  // RGBA = (255, 255, 255, 255)
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = &whitePixel;
        initData.SysMemPitch = 4;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &tex);
        if (SUCCEEDED(hr)) {
            device->CreateShaderResourceView(tex.Get(), nullptr, &m_defaultWhite);
        }
    }

    // Create default normal map (1x1 pixel: RGB=(128,128,255), Linear)
    {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // Linear
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_IMMUTABLE;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        UINT normalPixel = 0xFF8080FF;  // RGBA = (128, 128, 255, 255) -> normal=(0,0,1)
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = &normalPixel;
        initData.SysMemPitch = 4;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &tex);
        if (SUCCEEDED(hr)) {
            device->CreateShaderResourceView(tex.Get(), nullptr, &m_defaultNormal);
        }
    }

    // Create default black texture (1x1 black pixel, Linear)
    {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // Linear
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_IMMUTABLE;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        UINT blackPixel = 0xFF000000;  // RGBA = (0, 0, 0, 255)
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = &blackPixel;
        initData.SysMemPitch = 4;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &tex);
        if (SUCCEEDED(hr)) {
            device->CreateShaderResourceView(tex.Get(), nullptr, &m_defaultBlack);
        }
    }

    CFFLog::Info("Created default textures (white, normal, black)");
}

std::string CTextureManager::ResolveFullPath(const std::string& relativePath) const {
    return std::string("E:/forfun/assets/") + relativePath;
}

bool CTextureManager::LoadTextureFromFile(const std::string& fullPath, bool srgb, ComPtr<ID3D11ShaderResourceView>& outSRV) {
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) {
        return false;
    }

    // Convert to wide string
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wpath = converter.from_bytes(fullPath);

    // Use existing WIC loader
    return LoadTextureWIC(device, wpath, outSRV, srgb);
}
