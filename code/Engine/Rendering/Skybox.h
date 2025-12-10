#pragma once
#include "RHI/RHIResources.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>

// Skybox renderer using HDR cubemap
class CSkybox {
public:
    CSkybox() = default;
    ~CSkybox() = default;

    // Load HDR environment map and convert to cubemap
    bool Initialize(const std::string& hdrPath, int cubemapSize = 512);

    // Load pre-baked environment cubemap from KTX2
    bool InitializeFromKTX2(const std::string& ktx2Path);

    void Shutdown();

    // Render skybox
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

    // Get cubemap for IBL (future use)
    ID3D11ShaderResourceView* GetEnvironmentMap() const { return m_envCubemap; }
    ID3D11Texture2D* GetEnvironmentTexture() const { return m_envTexture; }

private:
    void convertEquirectToCubemap(const std::string& hdrPath, int size);
    void createCubeMesh();
    void createShaders();

private:
    // RHI texture (owns the D3D11 resources when loaded from KTX2)
    std::unique_ptr<RHI::ITexture> m_rhiEnvTexture;

    // Cubemap resources (raw pointers - owned by m_rhiEnvTexture or ComPtr below)
    ID3D11Texture2D* m_envTexture = nullptr;
    ID3D11ShaderResourceView* m_envCubemap = nullptr;

    // For HDR path (Initialize), we still use ComPtr since we create the texture ourselves
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ownedEnvTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ownedEnvCubemap;

    // Rendering resources
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbTransform;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;

    UINT m_indexCount = 0;
    std::string m_envPathKTX2 = "";
};
