#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <vector>
#include <memory>

// Forward declarations
class CScene;
struct SDirectionalLight;

// CShadowPass: Renders shadow map from light's perspective
// Used by CMainPass to apply shadows in final rendering
class CShadowPass
{
public:
    // Output bundle containing all shadow resources needed by CMainPass
    struct Output {
        static const int MAX_CASCADES = 4;

        int cascadeCount;                                   // Actual number of cascades (1-4)
        float cascadeSplits[MAX_CASCADES];                  // Split distances in camera space
        RHI::ITexture* shadowMapArray;                      // Texture2DArray (all cascades)
        DirectX::XMMATRIX lightSpaceVPs[MAX_CASCADES];      // Light space VP matrix per cascade
        RHI::ISampler* shadowSampler;                       // Comparison sampler for PCF
        float cascadeBlendRange;                            // Blend range at cascade boundaries (0-1)
        bool debugShowCascades;                             // Debug: visualize cascade colors
        bool enableSoftShadows;                             // Enable PCF for soft shadows (3x3 sampling)
    };

    CShadowPass() = default;
    ~CShadowPass() = default;

    // Initialize shadow pass resources
    bool Initialize();
    void Shutdown();

    // Render shadow map from directional light's perspective
    // Uses tight frustum fitting based on camera view frustum
    // cameraView, cameraProj: Camera matrices for frustum extraction
    void Render(CScene& scene, SDirectionalLight* light,
                const DirectX::XMMATRIX& cameraView,
                const DirectX::XMMATRIX& cameraProj);

    // Get complete shadow output bundle for MainPass
    const Output& GetOutput() const { return m_output; }

private:
    void ensureShadowMapArray(uint32_t size, int cascadeCount);

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
        SDirectionalLight* light,
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
    RHI::TexturePtr m_shadowMapArray;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_shadowDSVs[4];  // Per-slice DSV (Phase 6: add to RHI)
    uint32_t m_currentSize = 0;
    int m_currentCascadeCount = 0;

    // Default shadow map (1x1 white, depth=1.0, no shadow)
    RHI::TexturePtr m_defaultShadowMap;

    // Shadow sampler (comparison sampler for PCF)
    RHI::SamplerPtr m_shadowSampler;

    // Depth-only rendering pipeline
    RHI::ShaderPtr m_depthVS;
    RHI::PipelineStatePtr m_pso;
    RHI::BufferPtr m_cbLightSpace;   // Light space VP matrix
    RHI::BufferPtr m_cbObject;       // Object world matrix

    // Output bundle for MainPass
    Output m_output;
};
