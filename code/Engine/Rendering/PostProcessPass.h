#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// PostProcessPass: Handles tone mapping and gamma correction
// Converts HDR linear space to LDR sRGB space
class PostProcessPass {
public:
    PostProcessPass() = default;
    ~PostProcessPass() = default;

    // Initialize post-process resources
    bool Initialize();
    void Shutdown();

    // Render full-screen quad with tone mapping and gamma correction
    // hdrInput: HDR linear space texture (R16G16B16A16_FLOAT)
    // ldrOutput: LDR sRGB output render target (R8G8B8A8_UNORM_SRGB)
    void Render(ID3D11ShaderResourceView* hdrInput,
                ID3D11RenderTargetView* ldrOutput,
                UINT width, UINT height);

private:
    void createFullscreenQuad();
    void createShaders();

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
};
