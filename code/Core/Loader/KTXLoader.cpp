#include "KTXLoader.h"
#include "Core/FFLog.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/RHIDescriptors.h"
#include <ktx.h>
#include <vector>

using namespace RHI;

// Helper: Convert Vulkan format to RHI format
static ETextureFormat VkFormatToRHIFormat(uint32_t vkFormat) {
    switch (vkFormat) {
        case 97:  return ETextureFormat::R16G16B16A16_FLOAT;  // VK_FORMAT_R16G16B16A16_SFLOAT
        case 109: return ETextureFormat::R32G32B32A32_FLOAT;  // VK_FORMAT_R32G32B32A32_SFLOAT
        case 37:  return ETextureFormat::R8G8B8A8_UNORM;      // VK_FORMAT_R8G8B8A8_UNORM
        case 43:  return ETextureFormat::R8G8B8A8_UNORM_SRGB; // VK_FORMAT_R8G8B8A8_SRGB
        case 83:  return ETextureFormat::R16G16_FLOAT;        // VK_FORMAT_R16G16_SFLOAT
        default:
            CFFLog::Error("KTXLoader: Unsupported Vulkan format: %d", vkFormat);
            return ETextureFormat::Unknown;
    }
}

ITexture* CKTXLoader::LoadCubemapFromKTX2(const std::string& filepath) {
    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromNamedFile(filepath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXLoader: Failed to load %s (error %d)", filepath.c_str(), result);
        return nullptr;
    }

    // Verify it's a cubemap
    if (ktxTex->numFaces != 6) {
        CFFLog::Error("KTXLoader: %s is not a cubemap (faces=%d)", filepath.c_str(), ktxTex->numFaces);
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    // Convert format
    ETextureFormat rhiFormat = VkFormatToRHIFormat(ktxTex->vkFormat);
    if (rhiFormat == ETextureFormat::Unknown) {
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("KTXLoader: RHI context not available");
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    // Prepare subresource data (faces * mipLevels)
    uint32_t bytesPerPixel = GetBytesPerPixel(rhiFormat);
    std::vector<SubresourceData> subresources;
    subresources.reserve(6 * ktxTex->numLevels);

    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < ktxTex->numLevels; ++mip) {
            size_t offset;
            result = ktxTexture_GetImageOffset(ktxTexture(ktxTex), mip, 0, face, &offset);
            if (result != KTX_SUCCESS) {
                CFFLog::Error("KTXLoader: Failed to get image offset");
                ktxTexture2_Destroy(ktxTex);
                return nullptr;
            }

            uint32_t mipWidth = ktxTex->baseWidth >> mip;
            if (mipWidth == 0) mipWidth = 1;

            SubresourceData data;
            data.pData = ktxTex->pData + offset;
            data.rowPitch = mipWidth * bytesPerPixel;
            data.slicePitch = 0;
            subresources.push_back(data);
        }
    }

    // Create texture via RHI
    TextureDesc desc;
    desc.width = ktxTex->baseWidth;
    desc.height = ktxTex->baseHeight;
    desc.mipLevels = ktxTex->numLevels;
    desc.format = rhiFormat;
    desc.usage = ETextureUsage::ShaderResource;
    desc.isCubemap = true;
    desc.debugName = "KTXCubemap";

    ITexture* texture = ctx->CreateTextureWithData(desc, subresources.data(), (uint32_t)subresources.size());

    CFFLog::Info("KTXLoader: Loaded cubemap %s (%dx%d, %d mips)", filepath.c_str(), desc.width, desc.height, desc.mipLevels);

    ktxTexture2_Destroy(ktxTex);
    return texture;
}

ITexture* CKTXLoader::Load2DTextureFromKTX2(const std::string& filepath) {
    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromNamedFile(filepath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXLoader: Failed to load %s (error %d)", filepath.c_str(), result);
        return nullptr;
    }

    // Verify it's a 2D texture
    if (ktxTex->numFaces != 1) {
        CFFLog::Error("KTXLoader: %s is not a 2D texture (faces=%d)", filepath.c_str(), ktxTex->numFaces);
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    // Convert format
    ETextureFormat rhiFormat = VkFormatToRHIFormat(ktxTex->vkFormat);
    if (rhiFormat == ETextureFormat::Unknown) {
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("KTXLoader: RHI context not available");
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    // Prepare subresource data (mipLevels)
    uint32_t bytesPerPixel = GetBytesPerPixel(rhiFormat);
    std::vector<SubresourceData> subresources;
    subresources.reserve(ktxTex->numLevels);

    for (uint32_t mip = 0; mip < ktxTex->numLevels; ++mip) {
        size_t offset;
        result = ktxTexture_GetImageOffset(ktxTexture(ktxTex), mip, 0, 0, &offset);
        if (result != KTX_SUCCESS) {
            CFFLog::Error("KTXLoader: Failed to get image offset");
            ktxTexture2_Destroy(ktxTex);
            return nullptr;
        }

        uint32_t mipWidth = ktxTex->baseWidth >> mip;
        if (mipWidth == 0) mipWidth = 1;

        SubresourceData data;
        data.pData = ktxTex->pData + offset;
        data.rowPitch = mipWidth * bytesPerPixel;
        data.slicePitch = 0;
        subresources.push_back(data);
    }

    // Create texture via RHI
    TextureDesc desc;
    desc.width = ktxTex->baseWidth;
    desc.height = ktxTex->baseHeight;
    desc.mipLevels = ktxTex->numLevels;
    desc.format = rhiFormat;
    desc.usage = ETextureUsage::ShaderResource;
    desc.debugName = "KTX2DTexture";

    ITexture* texture = ctx->CreateTextureWithData(desc, subresources.data(), (uint32_t)subresources.size());

    CFFLog::Info("KTXLoader: Loaded 2D texture %s (%dx%d, %d mips)", filepath.c_str(), desc.width, desc.height, desc.mipLevels);

    ktxTexture2_Destroy(ktxTex);
    return texture;
}

// ============================================
// CPU-side loading for path tracing
// ============================================

// Helper: Convert half-float to float
static float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            // Zero
            uint32_t result = sign << 31;
            return *reinterpret_cast<float*>(&result);
        } else {
            // Denormalized
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                exponent--;
            }
            exponent++;
            mantissa &= ~0x400;
        }
    } else if (exponent == 31) {
        // Inf/NaN
        uint32_t result = (sign << 31) | 0x7F800000 | (mantissa << 13);
        return *reinterpret_cast<float*>(&result);
    }

    exponent = exponent + (127 - 15);
    mantissa = mantissa << 13;

    uint32_t result = (sign << 31) | (exponent << 23) | mantissa;
    return *reinterpret_cast<float*>(&result);
}

bool CKTXLoader::LoadCubemapToCPU(const std::string& filepath, SCubemapCPUData& outData) {
    outData.valid = false;
    outData.size = 0;
    for (int i = 0; i < 6; i++) {
        outData.faces[i].clear();
    }

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromNamedFile(filepath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXLoader: Failed to load %s for CPU (error %d)", filepath.c_str(), result);
        return false;
    }

    // Verify it's a cubemap
    if (ktxTex->numFaces != 6) {
        CFFLog::Error("KTXLoader: %s is not a cubemap (faces=%d)", filepath.c_str(), ktxTex->numFaces);
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    outData.size = ktxTex->baseWidth;
    int pixelCount = outData.size * outData.size;

    // Only load mip level 0
    for (uint32_t face = 0; face < 6; ++face) {
        outData.faces[face].resize(pixelCount);

        size_t offset;
        result = ktxTexture_GetImageOffset(ktxTexture(ktxTex), 0, 0, face, &offset);
        if (result != KTX_SUCCESS) {
            CFFLog::Error("KTXLoader: Failed to get image offset for face %d", face);
            ktxTexture2_Destroy(ktxTex);
            return false;
        }

        const uint8_t* srcData = ktxTex->pData + offset;

        // Convert based on format
        switch (ktxTex->vkFormat) {
            case 97: // VK_FORMAT_R16G16B16A16_SFLOAT
            {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(srcData);
                for (int i = 0; i < pixelCount; i++) {
                    outData.faces[face][i].x = HalfToFloat(src[i * 4 + 0]);
                    outData.faces[face][i].y = HalfToFloat(src[i * 4 + 1]);
                    outData.faces[face][i].z = HalfToFloat(src[i * 4 + 2]);
                    outData.faces[face][i].w = HalfToFloat(src[i * 4 + 3]);
                }
                break;
            }
            case 109: // VK_FORMAT_R32G32B32A32_SFLOAT
            {
                const float* src = reinterpret_cast<const float*>(srcData);
                for (int i = 0; i < pixelCount; i++) {
                    outData.faces[face][i].x = src[i * 4 + 0];
                    outData.faces[face][i].y = src[i * 4 + 1];
                    outData.faces[face][i].z = src[i * 4 + 2];
                    outData.faces[face][i].w = src[i * 4 + 3];
                }
                break;
            }
            case 37: // VK_FORMAT_R8G8B8A8_UNORM
            case 43: // VK_FORMAT_R8G8B8A8_SRGB
            {
                for (int i = 0; i < pixelCount; i++) {
                    outData.faces[face][i].x = srcData[i * 4 + 0] / 255.0f;
                    outData.faces[face][i].y = srcData[i * 4 + 1] / 255.0f;
                    outData.faces[face][i].z = srcData[i * 4 + 2] / 255.0f;
                    outData.faces[face][i].w = srcData[i * 4 + 3] / 255.0f;
                }
                break;
            }
            default:
                CFFLog::Error("KTXLoader: Unsupported format %d for CPU loading", ktxTex->vkFormat);
                ktxTexture2_Destroy(ktxTex);
                return false;
        }
    }

    ktxTexture2_Destroy(ktxTex);
    outData.valid = true;

    CFFLog::Info("KTXLoader: Loaded cubemap to CPU %s (%dx%d)", filepath.c_str(), outData.size, outData.size);
    return true;
}
