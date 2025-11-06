#pragma once
#include <d3d11.h>
#include <wrl.h>

struct OffscreenRT {
    UINT width=0, height=0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

    bool Create(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM);
    void Release();
    bool Resize(ID3D11Device* dev, UINT w, UINT h);
};
