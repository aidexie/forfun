#include "Screenshot.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include "Core/FFLog.h"
#include "Engine/Rendering/RenderPipeline.h"
#include "TestCase.h"
#include <vector>
#include <filesystem>
#include <memory>

// stb_image_write: Single-header library for writing images
// https://github.com/nothings/stb/blob/master/stb_image_write.h
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using namespace RHI;

bool CScreenshot::Capture(ITexture* texture, const std::string& path) {
    if (!texture) {
        CFFLog::Error("Screenshot: Null texture");
        return false;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();
    if (!ctx || !cmdList) {
        CFFLog::Error("Screenshot: RHI context not initialized");
        return false;
    }

    // Get texture dimensions
    uint32_t width = texture->GetWidth();
    uint32_t height = texture->GetHeight();
    ETextureFormat format = texture->GetFormat();

    // Verify format (accept R8G8B8A8 variants)
    if (format != ETextureFormat::R8G8B8A8_UNORM &&
        format != ETextureFormat::R8G8B8A8_UNORM_SRGB &&
        format != ETextureFormat::R8G8B8A8_TYPELESS) {
        CFFLog::Error("Screenshot: Unsupported format (expected R8G8B8A8, got %d)", (int)format);
        return false;
    }

    // Create staging texture for CPU readback
    TextureDesc stagingDesc;
    stagingDesc.width = width;
    stagingDesc.height = height;
    stagingDesc.mipLevels = 1;
    stagingDesc.arraySize = 1;
    stagingDesc.format = ETextureFormat::R8G8B8A8_UNORM;  // Use UNORM for staging
    stagingDesc.usage = ETextureUsage::Staging;
    stagingDesc.cpuAccess = ECPUAccess::Read;
    stagingDesc.debugName = "ScreenshotStaging";

    std::unique_ptr<ITexture> stagingTexture(ctx->CreateTexture(stagingDesc));
    if (!stagingTexture) {
        CFFLog::Error("Screenshot: Failed to create staging texture");
        return false;
    }

    // Copy source texture to staging texture
    cmdList->CopyTextureToSlice(stagingTexture.get(), 0, 0, texture);
    ctx->ExecuteAndWait();

    // Map staging texture to read pixels
    MappedTexture mapped = stagingTexture->Map(0, 0);
    if (!mapped.pData) {
        CFFLog::Error("Screenshot: Failed to map staging texture");
        return false;
    }

    // Copy pixel data (handle row pitch)
    std::vector<uint8_t> pixels(width * height * 4);
    uint8_t* src = static_cast<uint8_t*>(mapped.pData);
    uint8_t* dst = pixels.data();

    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst + y * width * 4, src + y * mapped.rowPitch, width * 4);
    }

    stagingTexture->Unmap(0, 0);

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

bool CScreenshot::CaptureFromPipeline(CRenderPipeline* pipeline, const std::string& path) {
    if (!pipeline) {
        CFFLog::Error("Screenshot: Null render pipeline");
        return false;
    }

    ITexture* texture = pipeline->GetOffscreenTextureRHI();
    if (!texture) {
        CFFLog::Error("Screenshot: Render pipeline offscreen texture is null");
        return false;
    }

    return Capture(texture, path);
}

bool CScreenshot::CaptureTest(CRenderPipeline* pipeline, const std::string& testName, int frame) {
    if (!pipeline) {
        CFFLog::Error("Screenshot: Null render pipeline");
        return false;
    }
    CFFLog::Error("Screenshot: cur claude code not support read image");
    return false;

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
