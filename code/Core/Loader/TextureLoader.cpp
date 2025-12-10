#include "TextureLoader.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <vector>
#include <assert.h>
#include <comdef.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static DXGI_FORMAT ToDXGIFormat(bool srgb){
    return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool LoadTextureWIC(void* devicePtr, const std::wstring& path, void** outSRV, bool srgb)
{
    ID3D11Device* device = static_cast<ID3D11Device*>(devicePtr);
    if (!device || !outSRV) return false;
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
    td.Width=w; td.Height=h;
    td.MipLevels=0;  // 0 = auto-generate full mipmap chain
    td.ArraySize=1;
    td.Format=ToDXGIFormat(srgb);
    td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // Need RENDER_TARGET for GenerateMips
    td.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;  // Enable mipmap generation

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, nullptr, tex.GetAddressOf()); if (FAILED(hr)) return false;

    // Get immediate context and upload data to mip 0
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.GetAddressOf());
    context->UpdateSubresource(tex.Get(), 0, nullptr, pixels.data(), w*4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format=td.Format;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels=-1;  // -1 = all mipmap levels
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());
    if (FAILED(hr)) return false;

    // Generate mipmaps automatically (box filter)
    context->GenerateMips(srv.Get());

    // Return SRV via void** (caller takes ownership)
    *outSRV = srv.Detach();
    return true;
}
