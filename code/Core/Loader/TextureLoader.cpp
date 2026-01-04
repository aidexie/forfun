#include "TextureLoader.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "Core/FFLog.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <comdef.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// Helper: Convert wide string to narrow (UTF-8)
static std::string WideToNarrow(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, narrow.data(), len, nullptr, nullptr);
    return narrow;
}

// Helper: Log HRESULT error with path context
static void LogHRError(const std::wstring& path, const char* operation, HRESULT hr) {
    _com_error err(hr);
    std::string narrowPath = WideToNarrow(path);
    std::string narrowMsg = WideToNarrow(err.ErrorMessage());
    CFFLog::Error("[TextureLoader] %s failed: %s (HRESULT=0x%08X: %s)",
                  operation, narrowPath.c_str(), hr, narrowMsg.c_str());
}

RHI::ITexture* LoadTextureWIC(const std::wstring& path, bool srgb)
{
    RHI::IRenderContext* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[TextureLoader] RHI context not available: %s", WideToNarrow(path).c_str());
        return nullptr;
    }

    HRESULT hr = S_OK;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        LogHRError(path, "CoCreateInstance(WICImagingFactory)", hr);
        return nullptr;
    }

    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) {
        LogHRError(path, "CreateDecoderFromFilename", hr);
        return nullptr;
    }

    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        LogHRError(path, "GetFrame(0)", hr);
        return nullptr;
    }

    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        LogHRError(path, "CreateFormatConverter", hr);
        return nullptr;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        LogHRError(path, "FormatConverter::Initialize", hr);
        return nullptr;
    }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    std::vector<uint8_t> pixels(w * h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) {
        LogHRError(path, "CopyPixels", hr);
        return nullptr;
    }

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
    if (!texture) {
        CFFLog::Error("[TextureLoader] CreateTexture failed: %s (%ux%u)",
                      WideToNarrow(path).c_str(), w, h);
        return nullptr;
    }

    // Generate mipmaps via RHI
    RHI::ICommandList* cmdList = ctx->GetCommandList();
    if (cmdList) {
        cmdList->GenerateMips(texture);
    }

    return texture;
}
