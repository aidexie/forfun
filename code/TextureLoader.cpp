
#include "TextureLoader.h"
#include <wincodec.h>
#include <vector>
#include <assert.h>
#include <comdef.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static DXGI_FORMAT ToDXGIFormat(bool srgb){
    return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool LoadTextureWIC(ID3D11Device* device, const std::wstring& path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV, bool srgb)
{
    HRESULT hr = S_OK;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    UINT w=0,h=0; converter->GetSize(&w,&h);
    std::vector<uint8_t> pixels(w*h*4);
    hr = converter->CopyPixels(nullptr, w*4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width=w; td.Height=h; td.MipLevels=1; td.ArraySize=1;
    td.Format=ToDXGIFormat(srgb);
    td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{ pixels.data(), w*4, 0 };

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, &srd, tex.GetAddressOf()); if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format=td.Format;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels=1;
    hr = device->CreateShaderResourceView(tex.Get(), &sd, outSRV.GetAddressOf());
    if (FAILED(hr)) return false;

    return true;
}
