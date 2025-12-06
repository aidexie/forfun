#include "KTXExporter.h"
#include "Core/FFLog.h"
#include "DX11Context.h"
#include <iostream>
#include <vector>
#include <DirectXPackedVector.h>
#include <filesystem>

uint32_t CKTXExporter::DXGIFormatToVkFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 97;  // VK_FORMAT_R16G16B16A16_SFLOAT
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return 109; // VK_FORMAT_R32G32B32A32_SFLOAT
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return 37;  // VK_FORMAT_R8G8B8A8_UNORM
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 43;  // VK_FORMAT_R8G8B8A8_SRGB
        case DXGI_FORMAT_R16G16_FLOAT:
            return 83;  // VK_FORMAT_R16G16_SFLOAT (for BRDF LUT)
        default:
            CFFLog::Error("Unsupported DXGI format: ", format);
            return 0;
    }
}

bool CKTXExporter::ExportCubemapToKTX2(
    ID3D11Texture2D* texture,
    const std::string& filepath,
    int numMipLevels)
{
    if (!texture) {
        CFFLog::Error("KTXExporter: Null texture");
        return false;
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Verify it's a cubemap
    if (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
        // OK
    } else {
        CFFLog::Error("KTXExporter: Texture is not a cubemap");
        return false;
    }

    int mipLevels = (numMipLevels > 0) ? numMipLevels : desc.MipLevels;

    // Create KTX texture
    ktxTextureCreateInfo createInfo = {};
    createInfo.glInternalformat = 0;  // Not using OpenGL
    createInfo.vkFormat = DXGIFormatToVkFormat(desc.Format);
    createInfo.baseWidth = desc.Width;
    createInfo.baseHeight = desc.Height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = mipLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = 6;  // Cubemap
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to create KTX texture: " , result);
        return false;
    }

    // Read texture data from GPU
    auto& ctx = CDX11Context::Instance();
    ID3D11DeviceContext* deviceContext = ctx.GetContext();

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        CFFLog::Error("KTXExporter: Failed to create staging texture");
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    // Copy to staging texture
    deviceContext->CopyResource(stagingTexture, texture);

    // Read each face and mip level
    for (int face = 0; face < 6; ++face) {
        for (int mip = 0; mip < mipLevels; ++mip) {
            UINT subresource = D3D11CalcSubresource(mip, face, desc.MipLevels);

            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = deviceContext->Map(stagingTexture, subresource, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr)) {
                CFFLog::Error("KTXExporter: Failed to map staging texture");
                stagingTexture->Release();
                ktxTexture2_Destroy(ktxTex);
                return false;
            }

            // Calculate mip size
            UINT mipWidth = desc.Width >> mip;
            UINT mipHeight = desc.Height >> mip;
            if (mipWidth == 0) mipWidth = 1;
            if (mipHeight == 0) mipHeight = 1;

            // Calculate size
            size_t imageSize = mapped.RowPitch * mipHeight;

            // Copy data to KTX
            // Note: KTX expects tightly packed data, but D3D11 may have padding
            // For simplicity, we'll copy row by row if there's padding
            size_t bytesPerPixel = 0;
            switch (desc.Format) {
                case DXGI_FORMAT_R16G16B16A16_FLOAT: bytesPerPixel = 8; break;
                case DXGI_FORMAT_R32G32B32A32_FLOAT: bytesPerPixel = 16; break;
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: bytesPerPixel = 4; break;
                case DXGI_FORMAT_R16G16_FLOAT: bytesPerPixel = 4; break;
                default: break;
            }

            size_t tightRowPitch = mipWidth * bytesPerPixel;

            if (mapped.RowPitch == tightRowPitch) {
                // No padding, direct copy
                result = ktxTexture_SetImageFromMemory(
                    ktxTexture(ktxTex),
                    mip, 0, face,
                    (const ktx_uint8_t*)mapped.pData,
                    imageSize
                );
            } else {
                // Has padding, copy row by row
                std::vector<uint8_t> tightData(tightRowPitch * mipHeight);
                for (UINT row = 0; row < mipHeight; ++row) {
                    memcpy(
                        tightData.data() + row * tightRowPitch,
                        (uint8_t*)mapped.pData + row * mapped.RowPitch,
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

            deviceContext->Unmap(stagingTexture, subresource);

            if (result != KTX_SUCCESS) {
                CFFLog::Error("KTXExporter: Failed to set image data: %s" , result);
                stagingTexture->Release();
                ktxTexture2_Destroy(ktxTex);
                return false;
            }
        }
    }

    stagingTexture->Release();

    // Write to file
    result = ktxTexture_WriteToNamedFile(ktxTexture(ktxTex), filepath.c_str());
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to write KTX file: %s" , result);
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    ktxTexture2_Destroy(ktxTex);
    CFFLog::Info("KTXExporter: Successfully exported to %s" , filepath);
    return true;
}

bool CKTXExporter::Export2DTextureToKTX2(
    ID3D11Texture2D* texture,
    const std::string& filepath,
    int numMipLevels)
{
    if (!texture) {
        CFFLog::Error("KTXExporter: Null texture");
        return false;
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    int mipLevels = (numMipLevels > 0) ? numMipLevels : desc.MipLevels;

    // Create KTX texture
    ktxTextureCreateInfo createInfo = {};
    createInfo.glInternalformat = 0;
    createInfo.vkFormat = DXGIFormatToVkFormat(desc.Format);
    createInfo.baseWidth = desc.Width;
    createInfo.baseHeight = desc.Height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = mipLevels;
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;  // 2D texture
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktxTex);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to create KTX texture: %s" , result);
        return false;
    }

    // Read texture data from GPU (similar to cubemap export)
    auto& ctx = CDX11Context::Instance();
    ID3D11DeviceContext* deviceContext = ctx.GetContext();

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = ctx.GetDevice()->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        CFFLog::Error("KTXExporter: Failed to create staging texture");
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    deviceContext->CopyResource(stagingTexture, texture);

    // Read each mip level
    for (int mip = 0; mip < mipLevels; ++mip) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = deviceContext->Map(stagingTexture, mip, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            CFFLog::Error("KTXExporter: Failed to map staging texture");
            stagingTexture->Release();
            ktxTexture2_Destroy(ktxTex);
            return false;
        }

        UINT mipWidth = desc.Width >> mip;
        UINT mipHeight = desc.Height >> mip;
        if (mipWidth == 0) mipWidth = 1;
        if (mipHeight == 0) mipHeight = 1;

        size_t bytesPerPixel = 0;
        switch (desc.Format) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT: bytesPerPixel = 8; break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: bytesPerPixel = 16; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: bytesPerPixel = 4; break;
            case DXGI_FORMAT_R16G16_FLOAT: bytesPerPixel = 4; break;
            default: break;
        }

        size_t tightRowPitch = mipWidth * bytesPerPixel;

        if (mapped.RowPitch == tightRowPitch) {
            size_t imageSize = mapped.RowPitch * mipHeight;
            result = ktxTexture_SetImageFromMemory(
                ktxTexture(ktxTex),
                mip, 0, 0,
                (const ktx_uint8_t*)mapped.pData,
                imageSize
            );
        } else {
            std::vector<uint8_t> tightData(tightRowPitch * mipHeight);
            for (UINT row = 0; row < mipHeight; ++row) {
                memcpy(
                    tightData.data() + row * tightRowPitch,
                    (uint8_t*)mapped.pData + row * mapped.RowPitch,
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

        deviceContext->Unmap(stagingTexture, mip);

        if (result != KTX_SUCCESS) {
            CFFLog::Error("KTXExporter: Failed to set image data: %s" , result);
            stagingTexture->Release();
            ktxTexture2_Destroy(ktxTex);
            return false;
        }
    }

    stagingTexture->Release();

    // Write to file
    result = ktxTexture_WriteToNamedFile(ktxTexture(ktxTex), filepath.c_str());
    if (result != KTX_SUCCESS) {
        CFFLog::Error("KTXExporter: Failed to write KTX file: %s" , result);
        ktxTexture2_Destroy(ktxTex);
        return false;
    }

    ktxTexture2_Destroy(ktxTex);
    CFFLog::Info("KTXExporter: Successfully exported 2D texture to %s" , filepath);
    return true;
}

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
    createInfo.glInternalformat = 0;  // Not using OpenGL
    createInfo.vkFormat = hdr ? 97 : 37;  // VK_FORMAT_R16G16B16A16_SFLOAT or VK_FORMAT_R8G8B8A8_UNORM
    createInfo.baseWidth = size;
    createInfo.baseHeight = size;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1;  // No mipmaps for debug output
    createInfo.numLayers = 1;
    createInfo.numFaces = 6;  // Cubemap
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
                // Convert float to half
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
                byteData[i * 4 + 3] = 255;  // Alpha = 1
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
