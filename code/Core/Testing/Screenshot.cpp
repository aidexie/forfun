#include "Screenshot.h"
#include "RHI/RHIManager.h"
#include "Core/FFLog.h"
#include "Engine/Rendering/ForwardRenderPipeline.h"
#include "TestCase.h"
#include <d3d11.h>
#include <vector>
#include <filesystem>

// stb_image_write: Single-header library for writing images
// https://github.com/nothings/stb/blob/master/stb_image_write.h
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

bool CScreenshot::Capture(ID3D11Texture2D* texture, const std::string& path) {
    if (!texture) {
        CFFLog::Error("Screenshot: Null texture");
        return false;
    }

    RHI::IRenderContext* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
    auto* device = static_cast<ID3D11Device*>(rhiCtx->GetNativeDevice());
    auto* context = static_cast<ID3D11DeviceContext*>(rhiCtx->GetNativeContext());

    if (!device || !context) {
        CFFLog::Error("Screenshot: D3D11 context not initialized");
        return false;
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Verify format (accept TYPELESS as it's used for multi-view textures)
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        CFFLog::Error("Screenshot: Unsupported format (expected R8G8B8A8, got %d)", desc.Format);
        return false;
    }

    // Create staging texture (CPU-readable)
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // Use UNORM for staging (compatible with TYPELESS source)
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        CFFLog::Error("Screenshot: Failed to create staging texture (HRESULT: 0x%08X)", hr);
        return false;
    }

    // Copy GPU texture to staging texture
    context->CopyResource(stagingTexture, texture);

    // Map staging texture to read pixels
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        CFFLog::Error("Screenshot: Failed to map staging texture (HRESULT: 0x%08X)", hr);
        stagingTexture->Release();
        return false;
    }

    // Copy pixel data (handle row pitch)
    UINT width = desc.Width;
    UINT height = desc.Height;
    std::vector<uint8_t> pixels(width * height * 4);

    uint8_t* src = static_cast<uint8_t*>(mapped.pData);
    uint8_t* dst = pixels.data();

    for (UINT y = 0; y < height; ++y) {
        memcpy(dst + y * width * 4, src + y * mapped.RowPitch, width * 4);
    }

    context->Unmap(stagingTexture, 0);
    stagingTexture->Release();

    // Ensure output directory exists
    if (!EnsureDirectoryExists(path)) {
        CFFLog::Error("Screenshot: Failed to create output directory");
        return false;
    }

    // Write PNG file
    int result = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
    if (!result) {
        CFFLog::Error("Screenshot: Failed to write PNG file: %s", path.c_str());
        return false;
    }

    CFFLog::Info("Screenshot saved: %s (%dx%d)", path.c_str(), width, height);
    return true;
}

bool CScreenshot::CaptureFromPipeline(CForwardRenderPipeline* pipeline, const std::string& path) {
    if (!pipeline) {
        CFFLog::Error("Screenshot: Null ForwardRenderPipeline");
        return false;
    }

    ID3D11Texture2D* texture = pipeline->GetOffscreenTexture();
    if (!texture) {
        CFFLog::Error("Screenshot: ForwardRenderPipeline offscreen texture is null");
        return false;
    }

    return Capture(texture, path);
}

bool CScreenshot::CaptureTest(CForwardRenderPipeline* pipeline, const std::string& testName, int frame) {
    if (!pipeline) {
        CFFLog::Error("Screenshot: Null ForwardRenderPipeline");
        return false;
    }

    // Build path: E:/forfun/debug/{testName}/screenshot_frame{frame}.png
    std::string path = GetTestScreenshotPath(testName.c_str(), frame);

    return CaptureFromPipeline(pipeline, path);
}

bool CScreenshot::EnsureDirectoryExists(const std::string& filePath) {
    try {
        std::filesystem::path p(filePath);
        std::filesystem::path dir = p.parent_path();

        if (!dir.empty() && !std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
            CFFLog::Info("Created directory: %s", dir.string().c_str());
        }
        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error("Failed to create directory: %s", e.what());
        return false;
    }
}
