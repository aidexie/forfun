// RHI/CB_PerFrame.h
// Shared PerFrame constant buffer structure matching Common.hlsli
#pragma once

#include <DirectXMath.h>
#include <cstdint>

namespace RHI {

// CB_PerFrame (b0, space0) - Matches Common.hlsli
// Contains per-frame global data: camera, time, screen size
struct alignas(16) CB_PerFrame {
    // Camera matrices
    DirectX::XMMATRIX view;
    DirectX::XMMATRIX proj;
    DirectX::XMMATRIX viewProj;
    DirectX::XMMATRIX invView;
    DirectX::XMMATRIX invProj;
    DirectX::XMMATRIX invViewProj;

    // Camera position and time
    DirectX::XMFLOAT3 cameraPos;
    float time;

    // Screen size and near/far planes
    DirectX::XMFLOAT2 screenSize;
    float nearZ;
    float farZ;
};

// CB_DeferredLightingPerPass (b0, space1) - Pass-specific data
struct alignas(16) CB_DeferredLightingPerPass {
    // CSM parameters
    int cascadeCount;
    int enableSoftShadows;
    float cascadeBlendRange;
    float shadowBias;
    DirectX::XMFLOAT4 cascadeSplits;
    DirectX::XMMATRIX lightSpaceVPs[4];

    // Directional light
    DirectX::XMFLOAT3 lightDirWS;
    float _pad0;
    DirectX::XMFLOAT3 lightColor;
    float _pad1;

    // IBL settings
    float iblIntensity;
    int diffuseGIMode;
    int probeIndex;
    uint32_t useReversedZ;
};

} // namespace RHI
