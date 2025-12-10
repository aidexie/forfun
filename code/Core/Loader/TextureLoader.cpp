#include "TextureLoader.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <comdef.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

RHI::ITexture* LoadTextureWIC(const std::wstring& path, bool srgb)
{
    RHI::IRenderContext* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return nullptr;

    HRESULT hr = S_OK;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return nullptr;

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    std::vector<uint8_t> pixels(w * h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) return nullptr;

    // Create texture via RHI with mipmap generation support
    RHI::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.mipLevels = 0;  // 0 = auto-generate full mipmap chain
    desc.arraySize = 1;
    desc.format = srgb ? RHI::ETextureFormat::R8G8B8A8_UNORM_SRGB : RHI::ETextureFormat::R8G8B8A8_UNORM;
    desc.usage = RHI::ETextureUsage::ShaderResource | RHI::ETextureUsage::RenderTarget;  // RenderTarget for GenerateMips
    desc.miscFlags = RHI::ETextureMiscFlags::GenerateMips;
    desc.debugName = "WICTexture";

    // Create texture with initial data at mip 0
    RHI::ITexture* texture = ctx->CreateTexture(desc, pixels.data());
    if (!texture) return nullptr;

    // Generate mipmaps via RHI
    RHI::ICommandList* cmdList = ctx->GetCommandList();
    if (cmdList) {
        cmdList->GenerateMips(texture);
    }

    return texture;
}
