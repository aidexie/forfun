#pragma once
#include <d3d11.h>
#include <string>
#include <vector>
#include <DirectXMath.h>

class CKTXLoader {
public:
    // Load KTX2 cubemap texture
    static ID3D11Texture2D* LoadCubemapFromKTX2(const std::string& filepath);

    // Load KTX2 2D texture
    static ID3D11Texture2D* Load2DTextureFromKTX2(const std::string& filepath);

    // Load KTX2 and create SRV
    static ID3D11ShaderResourceView* LoadCubemapSRVFromKTX2(const std::string& filepath);
    static ID3D11ShaderResourceView* Load2DTextureSRVFromKTX2(const std::string& filepath);

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
