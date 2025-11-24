#pragma once
#include <d3d11.h>
#include <string>

class CKTXLoader {
public:
    // Load KTX2 cubemap texture
    static ID3D11Texture2D* LoadCubemapFromKTX2(const std::string& filepath);

    // Load KTX2 2D texture
    static ID3D11Texture2D* Load2DTextureFromKTX2(const std::string& filepath);

    // Load KTX2 and create SRV
    static ID3D11ShaderResourceView* LoadCubemapSRVFromKTX2(const std::string& filepath);
    static ID3D11ShaderResourceView* Load2DTextureSRVFromKTX2(const std::string& filepath);
};
