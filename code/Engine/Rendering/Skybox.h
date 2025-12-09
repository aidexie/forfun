#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

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
    ID3D11ShaderResourceView* GetEnvironmentMap() const { return m_envCubemap.Get(); }
    ID3D11Texture2D* GetEnvironmentTexture() const { return m_envTexture.Get(); }

private:
    void convertEquirectToCubemap(const std::string& hdrPath, int size);
    void createCubeMesh();
    void createShaders();

private:
    // Cubemap resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_envTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_envCubemap;

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
