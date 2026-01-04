#include "KTXExporter.h"
#include "Core/FFLog.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ICommandList.h"
#include <ktx.h>
#include <vector>
#include <DirectXPackedVector.h>
#include <filesystem>
#include <memory>

using namespace RHI;

// ============================================
// Helper Functions
// ============================================

static uint32_t RHIFormatToVkFormat(ETextureFormat format) {
    switch (format) {
        case ETextureFormat::R16G16B16A16_FLOAT:
            return 97;  // VK_FORMAT_R16G16B16A16_SFLOAT
        case ETextureFormat::R32G32B32A32_FLOAT:
            return 109; // VK_FORMAT_R32G32B32A32_SFLOAT
        case ETextureFormat::R8G8B8A8_UNORM:
            return 37;  // VK_FORMAT_R8G8B8A8_UNORM
        case ETextureFormat::R8G8B8A8_UNORM_SRGB:
            return 43;  // VK_FORMAT_R8G8B8A8_SRGB
        case ETextureFormat::R16G16_FLOAT:
            return 83;  // VK_FORMAT_R16G16_SFLOAT (for BRDF LUT)
        default:
            CFFLog::Error("KTXExporter: Unsupported RHI format: %d", (int)format);
            return 0;
    }
}

// ============================================
// Internal Export Functions using RHI
// ============================================

static bool ExportCubemapToKTX2_RHI(ITexture* texture, const std::string& filepath, int numMipLevels) {
    if (!texture) {
        CFFLog::Error("KTXExporter: Null texture");
        return false;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();
    if (!ctx || !cmdList) {
        CFFLog::Error("KTXExporter: RHI context not available");
        return false;
    }

    uint32_t width = texture->GetWidth();
    uint32_t height = texture->GetHeight();
    ETextureFormat format = texture->GetFormat();
    uint32_t mipLevels = (numMipLevels > 0) ? numMipLevels : texture->GetMipLevels();
    uint32_t bytesPerPixel = GetBytesPerPixel(format);

    if (bytesPerPixel == 0) {
        CFFLog::Error("KTXExporter: Unsupported format for export");
        return false;
    }

    // Create KTX texture
    ktxTextureCreateInfo createInfo = {};
    createInfo.glInternalformat = 0;
    createInfo.vkFormat = RHIFormatToVkFormat(format);
    createInfo.baseWidth = width;
    createInfo.baseHeight = height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = mipLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = 6;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    if (createInfo.vkFormat == 0) {
        return false;
    }

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to create KTX texture: %d", result);
        return false;
    }

    // Create staging texture for readback
    TextureDesc stagingDesc;
    stagingDesc.width = width;
    stagingDesc.height = height;
    stagingDesc.mipLevels = mipLevels;
    stagingDesc.arraySize = 6;
    stagingDesc.format = format;
    stagingDesc.usage = ETextureUsage::Staging;
    stagingDesc.cpuAccess = ECPUAccess::Read;
    stagingDesc.debugName = "KTXExportStaging";

    std::unique_ptr<ITexture> stagingTexture(ctx->CreateTexture(stagingDesc));
    if (!stagingTexture) {
        CFFLog::Error("KTXExporter: Failed to create staging texture");
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    // Copy source to staging
    cmdList->CopyTexture(stagingTexture.get(), texture);

    // Read each face and mip level
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < mipLevels; ++mip) {
            MappedTexture mapped = stagingTexture->Map(face, mip);
            if (!mapped.pData) {
                CFFLog::Error("KTXExporter: Failed to map staging texture");
                ktxTexture2_Destroy(ktxTex);
                return false;
            }

            uint32_t mipWidth = width >> mip;
            uint32_t mipHeight = height >> mip;
            if (mipWidth == 0) mipWidth = 1;
            if (mipHeight == 0) mipHeight = 1;

            size_t tightRowPitch = mipWidth * bytesPerPixel;

            if (mapped.rowPitch == tightRowPitch) {
                // No padding, direct copy
                size_t imageSize = tightRowPitch * mipHeight;
                result = ktxTexture_SetImageFromMemory(
                    ktxTexture(ktxTex),
                    mip, 0, face,
                    (const ktx_uint8_t*)mapped.pData,
                    imageSize
                );
            } else {
                // Has padding, copy row by row
                std::vector<uint8_t> tightData(tightRowPitch * mipHeight);
                for (uint32_t row = 0; row < mipHeight; ++row) {
                    memcpy(
                        tightData.data() + row * tightRowPitch,
                        (uint8_t*)mapped.pData + row * mapped.rowPitch,
                        tightRowPitch
                    );
                }
                result = ktxTexture_SetImageFromMemory(
                    ktxTexture(ktxTex),
                    mip, 0, face,
                    tightData.data(),
                    tightData.size()
                );
            }

            stagingTexture->Unmap(face, mip);

            if (result != KTX_SUCCESS) {
                CFFLog::Error("KTXExporter: Failed to set image data: %d", result);
                ktxTexture2_Destroy(ktxTex);
                return false;
            }
        }
    }

    // Write to file
    result = ktxTexture_WriteToNamedFile(ktxTexture(ktxTex), filepath.c_str());
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to write KTX file: %d", result);
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    ktxTexture2_Destroy(ktxTex);
    CFFLog::Info("KTXExporter: Successfully exported cubemap to %s", filepath.c_str());
    return true;
}

static bool Export2DTextureToKTX2_RHI(ITexture* texture, const std::string& filepath, int numMipLevels) {
    if (!texture) {
        CFFLog::Error("KTXExporter: Null texture");
        return false;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();
    if (!ctx || !cmdList) {
        CFFLog::Error("KTXExporter: RHI context not available");
        return false;
    }

    uint32_t width = texture->GetWidth();
    uint32_t height = texture->GetHeight();
    ETextureFormat format = texture->GetFormat();
    uint32_t mipLevels = (numMipLevels > 0) ? numMipLevels : texture->GetMipLevels();
    uint32_t bytesPerPixel = GetBytesPerPixel(format);

    if (bytesPerPixel == 0) {
        CFFLog::Error("KTXExporter: Unsupported format for export");
        return false;
    }

    // Create KTX texture
    ktxTextureCreateInfo createInfo = {};
    createInfo.glInternalformat = 0;
    createInfo.vkFormat = RHIFormatToVkFormat(format);
    createInfo.baseWidth = width;
    createInfo.baseHeight = height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = mipLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    if (createInfo.vkFormat == 0) {
        return false;
    }

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to create KTX texture: %d", result);
        return false;
    }

    // Create staging texture for readback
    TextureDesc stagingDesc;
    stagingDesc.width = width;
    stagingDesc.height = height;
    stagingDesc.mipLevels = mipLevels;
    stagingDesc.arraySize = 1;
    stagingDesc.format = format;
    stagingDesc.usage = ETextureUsage::Staging;
    stagingDesc.cpuAccess = ECPUAccess::Read;
    stagingDesc.debugName = "KTXExportStaging2D";

    std::unique_ptr<ITexture> stagingTexture(ctx->CreateTexture(stagingDesc));
    if (!stagingTexture) {
        CFFLog::Error("KTXExporter: Failed to create staging texture");
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    // Copy source to staging
    cmdList->CopyTextureToSlice(stagingTexture.get(), 0, 0, texture);
    ctx->ExecuteAndWait();
    // Read each mip level
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        MappedTexture mapped = stagingTexture->Map(0, mip);
        if (!mapped.pData) {
            CFFLog::Error("KTXExporter: Failed to map staging texture");
            ktxTexture2_Destroy(ktxTex);
            return false;
        }

        uint32_t mipWidth = width >> mip;
        uint32_t mipHeight = height >> mip;
        if (mipWidth == 0) mipWidth = 1;
        if (mipHeight == 0) mipHeight = 1;

        size_t tightRowPitch = mipWidth * bytesPerPixel;

        if (mapped.rowPitch == tightRowPitch) {
            size_t imageSize = tightRowPitch * mipHeight;
            result = ktxTexture_SetImageFromMemory(
                ktxTexture(ktxTex),
                mip, 0, 0,
                (const ktx_uint8_t*)mapped.pData,
                imageSize
            );
        } else {
            std::vector<uint8_t> tightData(tightRowPitch * mipHeight);
            for (uint32_t row = 0; row < mipHeight; ++row) {
                memcpy(
                    tightData.data() + row * tightRowPitch,
                    (uint8_t*)mapped.pData + row * mapped.rowPitch,
                    tightRowPitch
                );
            }
            result = ktxTexture_SetImageFromMemory(
                ktxTexture(ktxTex),
                mip, 0, 0,
                tightData.data(),
                tightData.size()
            );
        }

        stagingTexture->Unmap(0, mip);

        if (result != KTX_SUCCESS) {
            CFFLog::Error("KTXExporter: Failed to set image data: %d", result);
            ktxTexture2_Destroy(ktxTex);
            return false;
        }
    }

    // Write to file
    result = ktxTexture_WriteToNamedFile(ktxTexture(ktxTex), filepath.c_str());
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to write KTX file: %d", result);
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    ktxTexture2_Destroy(ktxTex);
    CFFLog::Info("KTXExporter: Successfully exported 2D texture to %s", filepath.c_str());
    return true;
}

// ============================================
// CPU Data Export (no D3D11 dependency)
// ============================================

bool CKTXExporter::ExportCubemapFromCPUData(
    const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
    int size,
    const std::string& filepath,
    bool hdr)
{
    using namespace DirectX;
    using namespace DirectX::PackedVector;

    // Ensure output directory exists
    std::filesystem::path path(filepath);
    std::filesystem::create_directories(path.parent_path());

    // Create KTX texture
    ktxTextureCreateInfo createInfo = {};
    createInfo.glInternalformat = 0;
    createInfo.vkFormat = hdr ? 97 : 37;  // VK_FORMAT_R16G16B16A16_SFLOAT or VK_FORMAT_R8G8B8A8_UNORM
    createInfo.baseWidth = size;
    createInfo.baseHeight = size;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1;
    createInfo.numLayers = 1;
    createInfo.numFaces = 6;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("[KTXExporter] Failed to create KTX texture: %d", result);
        return false;
    }

    // Write each face
    for (int face = 0; face < 6; ++face) {
        const std::vector<XMFLOAT4>& faceData = cubemapData[face];

        if (faceData.size() != (size_t)(size * size)) {
            CFFLog::Error("[KTXExporter] Face %d data size mismatch: expected %d, got %zu",
                         face, size * size, faceData.size());
            ktxTexture2_Destroy(ktxTex);
            return false;
        }

        if (hdr) {
            // Convert XMFLOAT4 to R16G16B16A16_FLOAT
            std::vector<XMHALF4> halfData(size * size);
            for (int i = 0; i < size * size; ++i) {
                halfData[i].x = XMConvertFloatToHalf(faceData[i].x);
                halfData[i].y = XMConvertFloatToHalf(faceData[i].y);
                halfData[i].z = XMConvertFloatToHalf(faceData[i].z);
                halfData[i].w = XMConvertFloatToHalf(faceData[i].w);
            }

            result = ktxTexture_SetImageFromMemory(
                ktxTexture(ktxTex),
                0, 0, face,
                (const ktx_uint8_t*)halfData.data(),
                halfData.size() * sizeof(XMHALF4)
            );
        } else {
            // Convert XMFLOAT4 to R8G8B8A8_UNORM with tone mapping
            std::vector<uint8_t> byteData(size * size * 4);
            for (int i = 0; i < size * size; ++i) {
                // Simple Reinhard tone mapping + gamma correction
                float r = faceData[i].x / (1.0f + faceData[i].x);
                float g = faceData[i].y / (1.0f + faceData[i].y);
                float b = faceData[i].z / (1.0f + faceData[i].z);

                r = std::pow(r, 1.0f / 2.2f);
                g = std::pow(g, 1.0f / 2.2f);
                b = std::pow(b, 1.0f / 2.2f);

                byteData[i * 4 + 0] = (uint8_t)(std::min(r, 1.0f) * 255.0f);
                byteData[i * 4 + 1] = (uint8_t)(std::min(g, 1.0f) * 255.0f);
                byteData[i * 4 + 2] = (uint8_t)(std::min(b, 1.0f) * 255.0f);
                byteData[i * 4 + 3] = 255;
            }

            result = ktxTexture_SetImageFromMemory(
                ktxTexture(ktxTex),
                0, 0, face,
                byteData.data(),
                byteData.size()
            );
        }

        if (result != KTX_SUCCESS) {
            CFFLog::Error("[KTXExporter] Failed to set image data for face %d: %d", face, result);
            ktxTexture2_Destroy(ktxTex);
            return false;
        }
    }

    // Write to file
    result = ktxTexture_WriteToNamedFile(ktxTexture(ktxTex), filepath.c_str());
    if (result != KTX_SUCCESS) {
        CFFLog::Error("[KTXExporter] Failed to write KTX file: %d", result);
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    ktxTexture2_Destroy(ktxTex);
    CFFLog::Info("[KTXExporter] Successfully exported CPU cubemap to %s", filepath.c_str());
    return true;
}

// ============================================
// Public API (RHI Interface)
// ============================================

bool CKTXExporter::ExportCubemapToKTX2(ITexture* texture, const std::string& filepath, int numMipLevels) {
    return ExportCubemapToKTX2_RHI(texture, filepath, numMipLevels);
}

bool CKTXExporter::Export2DTextureToKTX2(ITexture* texture, const std::string& filepath, int numMipLevels) {
    return Export2DTextureToKTX2_RHI(texture, filepath, numMipLevels);
}

bool CKTXExporter::ExportCubemapToKTX2Native(void* nativeTexture, const std::string& filepath, int numMipLevels) {
    // This function is deprecated - native textures should be wrapped with RHI first
    CFFLog::Warning("KTXExporter: ExportCubemapToKTX2Native is deprecated, use RHI texture instead");
    return false;
}

bool CKTXExporter::Export2DTextureToKTX2Native(void* nativeTexture, const std::string& filepath, int numMipLevels) {
    // This function is deprecated - native textures should be wrapped with RHI first
    CFFLog::Warning("KTXExporter: Export2DTextureToKTX2Native is deprecated, use RHI texture instead");
    return false;
}
