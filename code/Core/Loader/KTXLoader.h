#pragma once
#include "RHI/RHIResources.h"
#include <string>
#include <vector>
#include <DirectXMath.h>

class CKTXLoader {
public:
    // Load KTX2 cubemap texture (returns RHI texture with SRV)
    static RHI::ITexture* LoadCubemapFromKTX2(const std::string& filepath);

    // Load KTX2 2D texture (returns RHI texture with SRV)
    static RHI::ITexture* Load2DTextureFromKTX2(const std::string& filepath);

    // ============================================
    // CPU-side loading (for path tracing)
    // ============================================

    // Cubemap face data (6 faces, each is a vector of XMFLOAT4 pixels)
    struct SCubemapCPUData {
        std::vector<DirectX::XMFLOAT4> faces[6];  // +X, -X, +Y, -Y, +Z, -Z
        int size = 0;  // Width/height of each face
        bool valid = false;
    };

    // Load KTX2 cubemap to CPU memory (for path tracing skybox sampling)
    static bool LoadCubemapToCPU(const std::string& filepath, SCubemapCPUData& outData);
};
