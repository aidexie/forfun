#include "TextureManager.h"
#include "RHI/RHIManager.h"
#include "FFLog.h"
#include "DebugPaths.h"
#include "PathManager.h"
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
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) {
        CFFLog::Error("Failed to create default textures: D3D11 device not available");
        return;
    }
    // --- 默认兜底纹理 ---
    auto MakeSolidSRV = [&](uint8_t r,uint8_t g,uint8_t b,uint8_t a, DXGI_FORMAT fmt){
        D3D11_TEXTURE2D_DESC td{}; td.Width=1; td.Height=1; td.MipLevels=1; td.ArraySize=1;
        td.Format=fmt; td.SampleDesc.Count=1; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        uint32_t px = (uint32_t(r) | (uint32_t(g)<<8) | (uint32_t(b)<<16) | (uint32_t(a)<<24));
        D3D11_SUBRESOURCE_DATA srd{ &px, 4, 0 };
        ComPtr<ID3D11Texture2D> tex; device->CreateTexture2D(&td, &srd, tex.GetAddressOf());
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=fmt; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> srv; device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());
        return srv;
    };
    m_defaultWhite = MakeSolidSRV(255,255,255,255, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); // sRGB (white)
    m_defaultNormal = MakeSolidSRV(128,128,255,255, DXGI_FORMAT_R8G8B8A8_UNORM);     // Linear (tangent-space up)
    m_defaultBlack = MakeSolidSRV(0,0,0,255, DXGI_FORMAT_R8G8B8A8_UNORM);  // Linear (G=Roughness=1, B=Metallic=1)

    CFFLog::Info("Created default textures (white, normal, black)");
}

std::string CTextureManager::ResolveFullPath(const std::string& relativePath) const {
    return FFPath::GetAbsolutePath(relativePath);
}

bool CTextureManager::LoadTextureFromFile(const std::string& fullPath, bool srgb, ComPtr<ID3D11ShaderResourceView>& outSRV) {
    ID3D11Device* device = static_cast<ID3D11Device*>(RHI::CRHIManager::Instance().GetRenderContext()->GetNativeDevice());
    if (!device) {
        return false;
    }

    // Convert to wide string
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wpath = converter.from_bytes(fullPath);

    // Use existing WIC loader
    return LoadTextureWIC(device, wpath, outSRV, srgb);
}
