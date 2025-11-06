#include "Offscreen.h"

bool OffscreenRT::Create(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
    width=w; height=h; format=fmt;
    D3D11_TEXTURE2D_DESC td{};
    td.Width=w; td.Height=h; td.MipLevels=1; td.ArraySize=1;
    td.Format=fmt; td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    auto hr = dev->CreateTexture2D(&td, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = dev->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = dev->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
    if (FAILED(hr)) return false;
    return true;
}

void OffscreenRT::Release() {
    srv.Reset(); rtv.Reset(); tex.Reset();
    width=height=0;
}

bool OffscreenRT::Resize(ID3D11Device* dev, UINT w, UINT h) {
    if (w==width && h==height && tex) return true;
    Release();
    return Create(dev, w, h, format);
}
