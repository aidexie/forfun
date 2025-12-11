#include "RHIHelpers.h"
#include "DX11/DX11Resources.h"

namespace RHI {

void* GetNativeSRV(ITexture* texture) {
    if (!texture) return nullptr;

    // Cast to DX11 texture and get SRV
    DX11::CDX11Texture* dx11Tex = static_cast<DX11::CDX11Texture*>(texture);
    return dx11Tex->GetOrCreateSRV();
}

void* GetNativeSRVSlice(ITexture* texture, uint32_t arraySlice, uint32_t mipLevel) {
    if (!texture) return nullptr;

    // Cast to DX11 texture and get slice SRV
    DX11::CDX11Texture* dx11Tex = static_cast<DX11::CDX11Texture*>(texture);
    return dx11Tex->GetOrCreateSRVSlice(arraySlice, mipLevel);
}

void* GetNativeRTV(ITexture* texture) {
    if (!texture) return nullptr;

    DX11::CDX11Texture* dx11Tex = static_cast<DX11::CDX11Texture*>(texture);
    return dx11Tex->GetOrCreateRTV();
}

void* GetNativeDSV(ITexture* texture) {
    if (!texture) return nullptr;

    DX11::CDX11Texture* dx11Tex = static_cast<DX11::CDX11Texture*>(texture);
    return dx11Tex->GetOrCreateDSV();
}

} // namespace RHI
