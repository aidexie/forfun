#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>

// Forward declarations
struct Scene;
class DirectionalLight;

// ShadowPass: Renders shadow map from light's perspective
// Used by MainPass to apply shadows in final rendering
class ShadowPass
{
public:
    // Output bundle containing all shadow resources needed by MainPass
    struct Output {
        ID3D11ShaderResourceView* shadowMap;      // Shadow map texture (or default if no light)
        ID3D11SamplerState*       shadowSampler;  // Comparison sampler for PCF
        DirectX::XMMATRIX         lightSpaceVP;   // Light space view-projection matrix
    };

    ShadowPass() = default;
    ~ShadowPass() = default;

    // Initialize shadow pass resources
    bool Initialize();
    void Shutdown();

    // Render shadow map from directional light's perspective
    // Updates internal output bundle for MainPass to use
    void Render(Scene& scene, DirectionalLight* light);

    // Get complete shadow output bundle for MainPass
    const Output& GetOutput() const { return m_output; }

private:
    void ensureShadowMap(UINT size);
    DirectX::XMMATRIX calculateLightSpaceMatrix(DirectionalLight* light, float shadowDistance);

private:
    // Shadow map resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         m_shadowMap;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>  m_shadowDSV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shadowSRV;
    UINT m_currentSize = 0;
    DirectX::XMMATRIX                               m_lightSpaceVP;
    // Default shadow map (1x1 white, depth=1.0, no shadow)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_defaultShadowMap;

    // Shadow sampler (comparison sampler for PCF)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowSampler;

    // Depth-only rendering pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_depthVS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbLightSpace;  // Light space VP matrix
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbObject;      // Object world matrix

    // Output bundle for MainPass
    Output m_output;
};
