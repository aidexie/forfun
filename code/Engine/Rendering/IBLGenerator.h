#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

// IBL (Image-Based Lighting) Generator
// Generates diffuse irradiance map and specular prefiltered environment map
class CIBLGenerator {
public:
    CIBLGenerator() = default;
    ~CIBLGenerator() = default;

    // Initialize generator resources
    bool Initialize();
    void Shutdown();

    // Generate diffuse irradiance map from environment cubemap
    // Returns the generated irradiance map SRV
    ID3D11ShaderResourceView* GenerateIrradianceMap(
        ID3D11ShaderResourceView* envMap,
        int outputSize = 32
    );

    // Generate specular pre-filtered environment map
    // Returns the generated pre-filtered map SRV (cubemap with mipmaps)
    // numMipLevels: Number of roughness levels (1 = single roughness level for testing)
    ID3D11ShaderResourceView* GeneratePreFilteredMap(
        ID3D11ShaderResourceView* envMap,
        int outputSize = 128,
        int numMipLevels = 1
    );

    // Generate BRDF LUT for Split Sum Approximation
    // Returns the generated BRDF LUT SRV (2D texture, RG channels)
    // resolution: LUT resolution (typically 512x512)
    ID3D11ShaderResourceView* GenerateBrdfLut(int resolution = 512);

    // Get rendering resources (for PBR shader usage)
    ID3D11ShaderResourceView* GetIrradianceMapSRV() const { return m_irradianceSRV.Get(); }
    ID3D11ShaderResourceView* GetPreFilteredMapSRV() const { return m_preFilteredSRV.Get(); }
    ID3D11ShaderResourceView* GetBrdfLutSRV() const { return m_brdfLutSRV.Get(); }

    // Get the number of mip levels in the pre-filtered map (0 if not generated)
    int GetPreFilteredMipLevels() const { return m_preFilteredMipLevels; }

    // Save the generated irradiance map to DDS file
    bool SaveIrradianceMapToDDS(const std::string& filepath);

    // Debug: Get individual face SRVs for visualization (creates on-demand)
    ID3D11ShaderResourceView* GetIrradianceFaceSRV(int faceIndex);

    // Debug: Get pre-filtered map face SRV for specific mip level
    ID3D11ShaderResourceView* GetPreFilteredFaceSRV(int faceIndex, int mipLevel);

private:
    void createFullscreenQuad();
    void createIrradianceShader();
    void createPreFilterShader();
    void createBrdfLutShader();

private:
    // Rendering resources
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_fullscreenVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_irradiancePS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_prefilterPS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_brdfLutPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fullscreenVB;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbFaceIndex;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbRoughness;  // For pre-filter roughness parameter

    // Generated irradiance map
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_irradianceTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_irradianceSRV;

    // Generated pre-filtered map
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_preFilteredTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_preFilteredSRV;
    int m_preFilteredMipLevels = 0;

    // Generated BRDF LUT
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_brdfLutTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_brdfLutSRV;

    // Debug: Individual face SRVs
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_debugFaceSRVs[6];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_debugPreFilteredFaceSRVs[6 * 10];  // 6 faces × max 5 mip levels
};
