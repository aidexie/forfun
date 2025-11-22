#pragma once
#include <d3d11.h>
#include <string>
#include <ktx.h>

// Helper class to export D3D11 textures to KTX2 format
class CKTXExporter {
public:
    // Export a D3D11 cubemap texture to KTX2 file
    // texture: Source D3D11 texture (must be a cubemap)
    // filepath: Output KTX2 file path
    // numMipLevels: Number of mipmap levels to export (0 = all levels)
    static bool ExportCubemapToKTX2(
        ID3D11Texture2D* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export a D3D11 2D texture to KTX2 file
    static bool Export2DTextureToKTX2(
        ID3D11Texture2D* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

private:
    // Convert DXGI format to KTX/Vulkan format
    static uint32_t DXGIFormatToVkFormat(DXGI_FORMAT format);
};
