#include "KTXLoader.h"
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
            std::cerr << "KTXLoader: Unsupported Vulkan format: " << vkFormat << std::endl;
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
        std::cerr << "KTXLoader: Failed to load " << filepath << " (error " << result << ")" << std::endl;
        return nullptr;
    }

    // Verify it's a cubemap
    if (ktxTex->numFaces != 6) {
        std::cerr << "KTXLoader: " << filepath << " is not a cubemap (faces=" << ktxTex->numFaces << ")" << std::endl;
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
                std::cerr << "KTXLoader: Failed to get image offset" << std::endl;
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
        std::cerr << "KTXLoader: Failed to create D3D11 texture" << std::endl;
        return nullptr;
    }

    std::cout << "KTXLoader: Loaded cubemap " << filepath << " (" << desc.Width << "x" << desc.Height << ", " << desc.MipLevels << " mips)" << std::endl;
    return texture;
}

ID3D11Texture2D* CKTXLoader::Load2DTextureFromKTX2(const std::string& filepath) {
    ktxTexture2* ktxTex = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromNamedFile(filepath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
    if (result != KTX_SUCCESS) {
        std::cerr << "KTXLoader: Failed to load " << filepath << " (error " << result << ")" << std::endl;
        return nullptr;
    }

    // Verify it's a 2D texture
    if (ktxTex->numFaces != 1) {
        std::cerr << "KTXLoader: " << filepath << " is not a 2D texture (faces=" << ktxTex->numFaces << ")" << std::endl;
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
            std::cerr << "KTXLoader: Failed to get image offset" << std::endl;
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
        std::cerr << "KTXLoader: Failed to create D3D11 texture" << std::endl;
        return nullptr;
    }

    std::cout << "KTXLoader: Loaded 2D texture " << filepath << " (" << desc.Width << "x" << desc.Height << ", " << desc.MipLevels << " mips)" << std::endl;
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
        std::cerr << "KTXLoader: Failed to create SRV" << std::endl;
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
        std::cerr << "KTXLoader: Failed to create SRV" << std::endl;
        return nullptr;
    }

    return srv;
}
