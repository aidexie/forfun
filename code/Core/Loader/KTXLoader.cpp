#include "KTXLoader.h"
#include "Core/FFLog.h"
#include "DX11Context.h"
#include <ktx.h>
#include <iostream>
#include <vector>

// Helper: Convert Vulkan format to DXGI format
static DXGI_FORMAT VkFormatToDXGIFormat(uint32_t vkFormat) {
    switch (vkFormat) {
        case 97:  return DXGI_FORMAT_R16G16B16A16_FLOAT;  // VK_FORMAT_R16G16B16A16_SFLOAT
        case 109: return DXGI_FORMAT_R32G32B32A32_FLOAT;  // VK_FORMAT_R32G32B32A32_SFLOAT
        case 37:  return DXGI_FORMAT_R8G8B8A8_UNORM;      // VK_FORMAT_R8G8B8A8_UNORM
        case 43:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // VK_FORMAT_R8G8B8A8_SRGB
        case 83:  return DXGI_FORMAT_R16G16_FLOAT;        // VK_FORMAT_R16G16_SFLOAT
        default:
            CFFLog::Error("KTXLoader: Unsupported Vulkan format: %d" , vkFormat);
            return DXGI_FORMAT_UNKNOWN;
    }
}

// Helper: Get bytes per pixel
static size_t GetBytesPerPixel(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 4;
        case DXGI_FORMAT_R16G16_FLOAT: return 4;
        default: return 0;
    }
}

ID3D11Texture2D* CKTXLoader::LoadCubemapFromKTX2(const std::string& filepath) {
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
    DXGI_FORMAT dxgiFormat = VkFormatToDXGIFormat(ktxTex->vkFormat);
    if (dxgiFormat == DXGI_FORMAT_UNKNOWN) {
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = ktxTex->baseWidth;
    desc.Height = ktxTex->baseHeight;
    desc.MipLevels = ktxTex->numLevels;
    desc.ArraySize = 6;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    // Prepare initial data
    std::vector<D3D11_SUBRESOURCE_DATA> initData;
    initData.reserve(6 * ktxTex->numLevels);

    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < ktxTex->numLevels; ++mip) {
            size_t offset;
            result = ktxTexture_GetImageOffset(ktxTexture(ktxTex), mip, 0, face, &offset);
            if (result != KTX_SUCCESS) {
                CFFLog::Error("KTXLoader: Failed to get image offset");
                ktxTexture2_Destroy(ktxTex);
                return nullptr;
            }

            D3D11_SUBRESOURCE_DATA data = {};
            data.pSysMem = ktxTex->pData + offset;

            uint32_t mipWidth = ktxTex->baseWidth >> mip;
            uint32_t mipHeight = ktxTex->baseHeight >> mip;
            if (mipWidth == 0) mipWidth = 1;
            if (mipHeight == 0) mipHeight = 1;

            data.SysMemPitch = mipWidth * GetBytesPerPixel(dxgiFormat);
            data.SysMemSlicePitch = 0;

            initData.push_back(data);
        }
    }

    // Create texture
    auto& ctx = CDX11Context::Instance();
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&desc, initData.data(), &texture);

    ktxTexture2_Destroy(ktxTex);

    if (FAILED(hr)) {
        CFFLog::Error("KTXLoader: Failed to create D3D11 texture");
        return nullptr;
    }

    CFFLog::Info("KTXLoader: Loaded cubemap %s (%dx%d, %d mips)", filepath.c_str(), desc.Width, desc.Height, desc.MipLevels);
    return texture;
}

ID3D11Texture2D* CKTXLoader::Load2DTextureFromKTX2(const std::string& filepath) {
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
    DXGI_FORMAT dxgiFormat = VkFormatToDXGIFormat(ktxTex->vkFormat);
    if (dxgiFormat == DXGI_FORMAT_UNKNOWN) {
        ktxTexture2_Destroy(ktxTex);
        return nullptr;
    }

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = ktxTex->baseWidth;
    desc.Height = ktxTex->baseHeight;
    desc.MipLevels = ktxTex->numLevels;
    desc.ArraySize = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Prepare initial data
    std::vector<D3D11_SUBRESOURCE_DATA> initData;
    initData.reserve(ktxTex->numLevels);

    for (uint32_t mip = 0; mip < ktxTex->numLevels; ++mip) {
        size_t offset;
        result = ktxTexture_GetImageOffset(ktxTexture(ktxTex), mip, 0, 0, &offset);
        if (result != KTX_SUCCESS) {
            CFFLog::Error("KTXLoader: Failed to get image offset");
            ktxTexture2_Destroy(ktxTex);
            return nullptr;
        }

        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = ktxTex->pData + offset;

        uint32_t mipWidth = ktxTex->baseWidth >> mip;
        uint32_t mipHeight = ktxTex->baseHeight >> mip;
        if (mipWidth == 0) mipWidth = 1;
        if (mipHeight == 0) mipHeight = 1;

        data.SysMemPitch = mipWidth * GetBytesPerPixel(dxgiFormat);
        data.SysMemSlicePitch = 0;

        initData.push_back(data);
    }

    // Create texture
    auto& ctx = CDX11Context::Instance();
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&desc, initData.data(), &texture);

    ktxTexture2_Destroy(ktxTex);

    if (FAILED(hr)) {
        CFFLog::Error("KTXLoader: Failed to create D3D11 texture");
        return nullptr;
    }

    CFFLog::Info("KTXLoader: Loaded 2D texture %s (%dx%d, %d mips)", filepath.c_str(), desc.Width, desc.Height, desc.MipLevels);
    return texture;
}

ID3D11ShaderResourceView* CKTXLoader::LoadCubemapSRVFromKTX2(const std::string& filepath) {
    ID3D11Texture2D* texture = LoadCubemapFromKTX2(filepath);
    if (!texture) return nullptr;

    auto& ctx = CDX11Context::Instance();

    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = texDesc.MipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateShaderResourceView(texture, &srvDesc, &srv);
    texture->Release();

    if (FAILED(hr)) {
        CFFLog::Error("KTXLoader: Failed to create SRV");
        return nullptr;
    }

    return srv;
}

ID3D11ShaderResourceView* CKTXLoader::Load2DTextureSRVFromKTX2(const std::string& filepath) {
    ID3D11Texture2D* texture = Load2DTextureFromKTX2(filepath);
    if (!texture) return nullptr;

    auto& ctx = CDX11Context::Instance();

    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateShaderResourceView(texture, &srvDesc, &srv);
    texture->Release();

    if (FAILED(hr)) {
        CFFLog::Error("KTXLoader: Failed to create SRV");
        return nullptr;
    }

    return srv;
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
