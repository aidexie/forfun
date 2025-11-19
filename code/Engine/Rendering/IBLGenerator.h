#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

// IBL (Image-Based Lighting) Generator
// Generates diffuse irradiance map and specular prefiltered environment map
class IBLGenerator {
public:
    IBLGenerator() = default;
    ~IBLGenerator() = default;

    // Initialize generator resources
    bool Initialize();
    void Shutdown();

    // Generate diffuse irradiance map from environment cubemap
    // Returns the generated irradiance map SRV
    ID3D11ShaderResourceView* GenerateIrradianceMap(
        ID3D11ShaderResourceView* envMap,
        int outputSize = 32
    );

    // Save the generated irradiance map to DDS file
    bool SaveIrradianceMapToDDS(const std::string& filepath);

    // Debug: Get individual face SRVs for visualization (creates on-demand)
    ID3D11ShaderResourceView* GetIrradianceFaceSRV(int faceIndex);

private:
    void createFullscreenQuad();
    void createIrradianceShader();

private:
    // Rendering resources
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_fullscreenVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_irradiancePS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fullscreenVB;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbFaceIndex;

    // Generated irradiance map
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_irradianceTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_irradianceSRV;

    // Debug: Individual face SRVs
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_debugFaceSRVs[6];
};
