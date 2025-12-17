#include "DX11Resources.h"
#include "DX11Utils.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX11 {

// ============================================
// View Creation Implementation
// ============================================

ID3D11ShaderResourceView* CDX11Texture::GetOrCreateSRV() {
    if (m_defaultSRV) {
        return m_defaultSRV.Get();
    }

    if (!m_device) return nullptr;

    // Create default SRV (all mips, all slices)
    m_defaultSRV = createSRV(0, m_desc.mipLevels, 0, m_desc.arraySize);
    return m_defaultSRV.Get();
}

ID3D11ShaderResourceView* CDX11Texture::GetOrCreateSRVSlice(uint32_t arraySlice, uint32_t mipLevel) {
    // Check cache first
    ViewKey key{mipLevel, arraySlice};
    auto it = m_srvCache.find(key);
    if (it != m_srvCache.end()) {
        return it->second.Get();
    }

    if (!m_device) return nullptr;

    // Create slice SRV (single mip, single slice)
    auto srv = createSRV(mipLevel, 1, arraySlice, 1);
    if (srv) {
        m_srvCache[key] = srv;
    }
    return srv.Get();
}

ID3D11RenderTargetView* CDX11Texture::GetOrCreateRTV() {
    if (m_defaultRTV) {
        return m_defaultRTV.Get();
    }

    if (!m_device) return nullptr;

    m_defaultRTV = createRTV(0, 0);
    return m_defaultRTV.Get();
}

ID3D11RenderTargetView* CDX11Texture::GetOrCreateRTVSlice(uint32_t arraySlice, uint32_t mipLevel) {
    ViewKey key{mipLevel, arraySlice};
    auto it = m_rtvCache.find(key);
    if (it != m_rtvCache.end()) {
        return it->second.Get();
    }

    if (!m_device) return nullptr;

    auto rtv = createRTV(mipLevel, arraySlice);
    if (rtv) {
        m_rtvCache[key] = rtv;
    }
    return rtv.Get();
}

ID3D11DepthStencilView* CDX11Texture::GetOrCreateDSV() {
    if (m_defaultDSV) {
        return m_defaultDSV.Get();
    }

    if (!m_device) return nullptr;

    m_defaultDSV = createDSV(0);
    return m_defaultDSV.Get();
}

ID3D11DepthStencilView* CDX11Texture::GetOrCreateDSVSlice(uint32_t arraySlice) {
    auto it = m_dsvCache.find(arraySlice);
    if (it != m_dsvCache.end()) {
        return it->second.Get();
    }

    if (!m_device) return nullptr;

    auto dsv = createDSV(arraySlice);
    if (dsv) {
        m_dsvCache[arraySlice] = dsv;
    }
    return dsv.Get();
}

ID3D11UnorderedAccessView* CDX11Texture::GetOrCreateUAV() {
    if (m_defaultUAV) {
        return m_defaultUAV.Get();
    }

    if (!m_device) return nullptr;

    m_defaultUAV = createUAV(0);
    return m_defaultUAV.Get();
}

ID3D11UnorderedAccessView* CDX11Texture::GetOrCreateUAVSlice(uint32_t mipLevel) {
    // mipLevel 0 uses default UAV
    if (mipLevel == 0) {
        return GetOrCreateUAV();
    }

    // Check cache
    auto it = m_uavCache.find(mipLevel);
    if (it != m_uavCache.end()) {
        return it->second.Get();
    }

    if (!m_device) return nullptr;

    auto uav = createUAV(mipLevel);
    if (uav) {
        m_uavCache[mipLevel] = uav;
    }
    return uav.Get();
}

// ============================================
// Legacy Setters
// ============================================

void CDX11Texture::SetSliceRTV(uint32_t index, ID3D11RenderTargetView* rtv) {
    ViewKey key{0, index};
    m_rtvCache[key] = rtv;
}

void CDX11Texture::SetSliceDSV(uint32_t index, ID3D11DepthStencilView* dsv) {
    m_dsvCache[index] = dsv;
}

// ============================================
// View Creation Helpers
// ============================================

ComPtr<ID3D11ShaderResourceView> CDX11Texture::createSRV(uint32_t mipLevel, uint32_t numMips, uint32_t arraySlice, uint32_t numSlices) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

    // Use override format if specified
    DXGI_FORMAT format = (m_desc.srvFormat != ETextureFormat::Unknown)
        ? ToDXGIFormat(m_desc.srvFormat)
        : ToDXGIFormat(m_desc.format);
    srvDesc.Format = format;

    switch (m_desc.dimension) {
        case ETextureDimension::Tex2D:
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = mipLevel;
            srvDesc.Texture2D.MipLevels = numMips;
            break;

        case ETextureDimension::Tex3D:
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            srvDesc.Texture3D.MostDetailedMip = mipLevel;
            srvDesc.Texture3D.MipLevels = numMips;
            break;

        case ETextureDimension::TexCube:
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = mipLevel;
            srvDesc.TextureCube.MipLevels = numMips;
            break;

        case ETextureDimension::Tex2DArray:
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = mipLevel;
            srvDesc.Texture2DArray.MipLevels = numMips;
            srvDesc.Texture2DArray.FirstArraySlice = arraySlice;
            srvDesc.Texture2DArray.ArraySize = numSlices;
            break;

        case ETextureDimension::TexCubeArray:
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.TextureCubeArray.MostDetailedMip = mipLevel;
            srvDesc.TextureCubeArray.MipLevels = numMips;
            srvDesc.TextureCubeArray.First2DArrayFace = 0;
            srvDesc.TextureCubeArray.NumCubes = m_desc.arraySize;
            break;
    }

    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = m_device->CreateShaderResourceView(GetD3D11Resource(), &srvDesc, &srv);
    if (FAILED(hr)) {
        CFFLog::Error("CreateShaderResourceView failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }
    return srv;
}

ComPtr<ID3D11RenderTargetView> CDX11Texture::createRTV(uint32_t mipLevel, uint32_t arraySlice) {
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};

    // Use override format if specified
    DXGI_FORMAT format = (m_desc.rtvFormat != ETextureFormat::Unknown)
        ? ToDXGIFormat(m_desc.rtvFormat)
        : ToDXGIFormat(m_desc.format);
    rtvDesc.Format = format;

    bool isArray = (m_desc.dimension == ETextureDimension::Tex2DArray ||
                    m_desc.dimension == ETextureDimension::TexCube ||
                    m_desc.dimension == ETextureDimension::TexCubeArray);

    if (isArray) {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = mipLevel;
        rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
        rtvDesc.Texture2DArray.ArraySize = 1;
    } else {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = mipLevel;
    }

    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = m_device->CreateRenderTargetView(GetD3D11Resource(), &rtvDesc, &rtv);
    if (FAILED(hr)) {
        CFFLog::Error("CreateRenderTargetView failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }
    return rtv;
}

ComPtr<ID3D11DepthStencilView> CDX11Texture::createDSV(uint32_t arraySlice) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

    // Use override format if specified
    DXGI_FORMAT format = (m_desc.dsvFormat != ETextureFormat::Unknown)
        ? ToDXGIFormat(m_desc.dsvFormat)
        : ToDXGIFormat(m_desc.format);
    dsvDesc.Format = format;

    bool isArray = (m_desc.dimension == ETextureDimension::Tex2DArray ||
                    m_desc.arraySize > 1);

    if (isArray) {
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = arraySlice;
        dsvDesc.Texture2DArray.ArraySize = 1;
    } else {
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = 0;
    }

    ComPtr<ID3D11DepthStencilView> dsv;
    HRESULT hr = m_device->CreateDepthStencilView(GetD3D11Resource(), &dsvDesc, &dsv);
    if (FAILED(hr)) {
        CFFLog::Error("CreateDepthStencilView failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }
    return dsv;
}

ComPtr<ID3D11UnorderedAccessView> CDX11Texture::createUAV(uint32_t mipLevel) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

    // Use override format if specified
    DXGI_FORMAT format = (m_desc.uavFormat != ETextureFormat::Unknown)
        ? ToDXGIFormat(m_desc.uavFormat)
        : ToDXGIFormat(m_desc.format);
    uavDesc.Format = format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = mipLevel;

    ComPtr<ID3D11UnorderedAccessView> uav;
    HRESULT hr = m_device->CreateUnorderedAccessView(GetD3D11Resource(), &uavDesc, &uav);
    if (FAILED(hr)) {
        CFFLog::Error("CreateUnorderedAccessView failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }
    return uav;
}

} // namespace DX11
} // namespace RHI
