#pragma once
#include "RHI/RHIResources.h"
#include <d3d11.h>
#include <string>
#include <array>
#include <vector>
#include <DirectXMath.h>
#include <ktx.h>

// Helper class to export D3D11 textures to KTX2 format
class CKTXExporter {
public:
    // Export an RHI texture (cubemap) to KTX2 file
    // This is the preferred interface for external use
    static bool ExportCubemapToKTX2(
        RHI::ITexture* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export an RHI texture (2D) to KTX2 file
    // This is the preferred interface for external use
    static bool Export2DTextureToKTX2(
        RHI::ITexture* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export a D3D11 cubemap texture to KTX2 file (internal use)
    // texture: Source D3D11 texture (must be a cubemap)
    // filepath: Output KTX2 file path
    // numMipLevels: Number of mipmap levels to export (0 = all levels)
    static bool ExportCubemapToKTX2(
        ID3D11Texture2D* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export a D3D11 2D texture to KTX2 file (internal use)
    static bool Export2DTextureToKTX2(
        ID3D11Texture2D* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export CPU cubemap data (XMFLOAT4, 6 faces) to KTX2 file
    // cubemapData: Array of 6 faces, each face is a vector of RGBA pixels
    // size: Width/Height of each face
    // filepath: Output KTX2 file path
    // hdr: If true, export as R16G16B16A16_FLOAT; if false, R8G8B8A8_UNORM
    static bool ExportCubemapFromCPUData(
        const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
        int size,
        const std::string& filepath,
        bool hdr = true
    );

private:
    // Convert DXGI format to KTX/Vulkan format
    static uint32_t DXGIFormatToVkFormat(DXGI_FORMAT format);
};
