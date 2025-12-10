#include "TextureManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "FFLog.h"
#include "DebugPaths.h"
#include "PathManager.h"
#include "Loader/TextureLoader.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <codecvt>
#include <locale>

using Microsoft::WRL::ComPtr;

CTextureManager& CTextureManager::Instance() {
    static CTextureManager instance;
    return instance;
}

CTextureManager::CTextureManager() {
    CreateDefaultTextures();
    CFFLog::Info("TextureManager initialized");
}

RHI::ITexture* CTextureManager::Load(const std::string& path, bool srgb) {
    if (path.empty()) {
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    // Create cache key with sRGB flag
    std::string cacheKey = path + (srgb ? "|srgb" : "|linear");

    // Check cache
    auto it = m_textures.find(cacheKey);
    if (it != m_textures.end()) {
        return it->second.texture.get();
    }

    // Load from file
    std::string fullPath = ResolveFullPath(path);
    RHI::ITexture* texture = LoadTextureFromFile(fullPath, srgb);

    if (!texture) {
        CFFLog::Warning(("Failed to load texture: " + path + ", using default").c_str());
        return srgb ? GetDefaultWhite() : GetDefaultBlack();
    }

    CFFLog::Info(("Loaded texture: " + path + (srgb ? " (sRGB)" : " (Linear)")).c_str());

    // Cache it
    CachedTexture cached;
    cached.texture.reset(texture);
    cached.isSRGB = srgb;
    m_textures[cacheKey] = std::move(cached);

    return m_textures[cacheKey].texture.get();
}

RHI::ITexture* CTextureManager::GetDefaultWhite() {
    return m_defaultWhite.get();
}

RHI::ITexture* CTextureManager::GetDefaultNormal() {
    return m_defaultNormal.get();
}

RHI::ITexture* CTextureManager::GetDefaultBlack() {
    return m_defaultBlack.get();
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
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) {
        CFFLog::Error("Failed to create default textures: RHI context not available");
        return;
    }

    ID3D11Device* device = static_cast<ID3D11Device*>(rhiCtx->GetNativeDevice());
    if (!device) {
        CFFLog::Error("Failed to create default textures: D3D11 device not available");
        return;
    }

    // Lambda to create solid color 1x1 texture
    auto MakeSolidTexture = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a, DXGI_FORMAT fmt, RHI::ETextureFormat rhiFmt) -> RHI::ITexture* {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        uint32_t px = (uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24));
        D3D11_SUBRESOURCE_DATA srd{ &px, 4, 0 };

        ComPtr<ID3D11Texture2D> tex;
        device->CreateTexture2D(&td, &srd, tex.GetAddressOf());

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;

        ComPtr<ID3D11ShaderResourceView> srv;
        device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());

        // Wrap into RHI texture - transfer ownership
        ID3D11Texture2D* rawTex = tex.Detach();
        ID3D11ShaderResourceView* rawSRV = srv.Detach();
        return rhiCtx->WrapNativeTexture(rawTex, rawSRV, 1, 1, rhiFmt);
    };

    m_defaultWhite.reset(MakeSolidTexture(255, 255, 255, 255,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, RHI::ETextureFormat::R8G8B8A8_UNORM_SRGB));
    m_defaultNormal.reset(MakeSolidTexture(128, 128, 255, 255,
        DXGI_FORMAT_R8G8B8A8_UNORM, RHI::ETextureFormat::R8G8B8A8_UNORM));
    m_defaultBlack.reset(MakeSolidTexture(0, 0, 0, 255,
        DXGI_FORMAT_R8G8B8A8_UNORM, RHI::ETextureFormat::R8G8B8A8_UNORM));

    CFFLog::Info("Created default textures (white, normal, black)");
}

std::string CTextureManager::ResolveFullPath(const std::string& relativePath) const {
    return FFPath::GetAbsolutePath(relativePath);
}

RHI::ITexture* CTextureManager::LoadTextureFromFile(const std::string& fullPath, bool srgb) {
    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!rhiCtx) {
        return nullptr;
    }

    ID3D11Device* device = static_cast<ID3D11Device*>(rhiCtx->GetNativeDevice());
    if (!device) {
        return nullptr;
    }

    // Convert to wide string
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wpath = converter.from_bytes(fullPath);

    // Use existing WIC loader - returns SRV
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!LoadTextureWIC(device, wpath, srv, srgb)) {
        return nullptr;
    }

    // Get texture info from SRV
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(resource.GetAddressOf());

    ComPtr<ID3D11Texture2D> texture;
    resource.As(&texture);

    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);

    RHI::ETextureFormat rhiFmt = srgb ? RHI::ETextureFormat::R8G8B8A8_UNORM_SRGB : RHI::ETextureFormat::R8G8B8A8_UNORM;

    // Wrap into RHI texture - transfer ownership
    ID3D11Texture2D* rawTex = texture.Detach();
    ID3D11ShaderResourceView* rawSRV = srv.Detach();
    return rhiCtx->WrapNativeTexture(rawTex, rawSRV, texDesc.Width, texDesc.Height, rhiFmt);
}
