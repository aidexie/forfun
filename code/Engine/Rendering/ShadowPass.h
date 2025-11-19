#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <vector>

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
        static const int MAX_CASCADES = 4;

        int cascadeCount;                                   // Actual number of cascades (1-4)
        float cascadeSplits[MAX_CASCADES];                  // Split distances in camera space
        ID3D11ShaderResourceView* shadowMapArray;           // Texture2DArray SRV (all cascades)
        DirectX::XMMATRIX lightSpaceVPs[MAX_CASCADES];      // Light space VP matrix per cascade
        ID3D11SamplerState* shadowSampler;                  // Comparison sampler for PCF
        float cascadeBlendRange;                            // Blend range at cascade boundaries (0-1)
        bool debugShowCascades;                             // Debug: visualize cascade colors
        bool enableSoftShadows;                             // Enable PCF for soft shadows (3x3 sampling)
    };

    ShadowPass() = default;
    ~ShadowPass() = default;

    // Initialize shadow pass resources
    bool Initialize();
    void Shutdown();

    // Render shadow map from directional light's perspective
    // Uses tight frustum fitting based on camera view frustum
    // cameraView, cameraProj: Camera matrices for frustum extraction
    void Render(Scene& scene, DirectionalLight* light,
                const DirectX::XMMATRIX& cameraView,
                const DirectX::XMMATRIX& cameraProj);

    // Get complete shadow output bundle for MainPass
    const Output& GetOutput() const { return m_output; }

private:
    void ensureShadowMapArray(UINT size, int cascadeCount);

    // Tight frustum fitting helpers
    std::array<DirectX::XMFLOAT3, 8> extractFrustumCorners(
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& proj) const;

    // Extract sub-frustum for a specific cascade
    std::array<DirectX::XMFLOAT3, 8> extractSubFrustum(
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& proj,
        float nearDist,
        float farDist) const;

    DirectX::XMMATRIX calculateTightLightMatrix(
        const std::array<DirectX::XMFLOAT3, 8>& frustumCornersWS,
        DirectionalLight* light,
        float cascadeFarDist) const;

    // Calculate cascade split distances using Practical Split Scheme
    std::vector<float> calculateCascadeSplits(
        int cascadeCount,
        float nearPlane,
        float farPlane,
        float lambda) const;

    // Bounding sphere for stabilizing shadow projection
    struct BoundingSphere {
        DirectX::XMFLOAT3 center;
        float radius;
    };

    // Calculate minimum bounding sphere for a set of points
    BoundingSphere calculateBoundingSphere(
        const std::array<DirectX::XMFLOAT3, 8>& points) const;

private:
    // Shadow map resources (Texture2DArray for CSM)
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_shadowMapArray;     // Texture2D with ArraySize=cascadeCount
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_shadowDSVs[4];      // Per-slice DSV
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shadowArraySRV;     // Array SRV
    UINT m_currentSize = 0;
    int m_currentCascadeCount = 0;

    // Default shadow map (1x1 white, depth=1.0, no shadow)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_defaultShadowMap;

    // Shadow sampler (comparison sampler for PCF)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowSampler;

    // Depth-only rendering pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_depthVS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbLightSpace;  // Light space VP matrix
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cbObject;      // Object world matrix

    // Render states for shadow rendering
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rasterState;

    // Output bundle for MainPass
    Output m_output;
};
