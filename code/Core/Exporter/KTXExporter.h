#pragma once
#include "RHI/RHIResources.h"
#include <string>
#include <array>
#include <vector>
#include <DirectXMath.h>

// Helper class to export textures to KTX2 format
class CKTXExporter {
public:
    // Export an RHI texture (cubemap) to KTX2 file
    static bool ExportCubemapToKTX2(
        RHI::ITexture* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export an RHI texture (2D) to KTX2 file
    static bool Export2DTextureToKTX2(
        RHI::ITexture* texture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export native texture (void* to ID3D11Texture2D) to KTX2 file
    // For internal use when RHI texture is not available
    static bool ExportCubemapToKTX2Native(
        void* nativeTexture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    static bool Export2DTextureToKTX2Native(
        void* nativeTexture,
        const std::string& filepath,
        int numMipLevels = 0
    );

    // Export CPU cubemap data (XMFLOAT4, 6 faces) to KTX2 file
    static bool ExportCubemapFromCPUData(
        const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
        int size,
        const std::string& filepath,
        bool hdr = true
    );

    // Export CPU 2D float3 buffer to KTX2 file (for debugging)
    // data: RGB float buffer (width * height * 3 floats)
    static bool Export2DFromFloat3Buffer(
        const float* data,
        int width,
        int height,
        const std::string& filepath
    );
};
