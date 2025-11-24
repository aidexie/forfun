#include "ShadowPass.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Scene.h"
#include "GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include <d3dcompiler.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

#pragma comment(lib, "d3dcompiler.lib")

struct alignas(16) CB_LightSpace {
    DirectX::XMMATRIX lightSpaceVP;
};

struct alignas(16) CB_Object {
    DirectX::XMMATRIX world;
};

bool CShadowPass::Initialize()
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return false;

    // Depth-only vertex shader
    static const char* kDepthVS = R"(
        cbuffer CB_LightSpace : register(b0) {
            float4x4 gLightSpaceVP;
        }
        cbuffer CB_Object : register(b1) {
            float4x4 gWorld;
        }

        struct VSIn {
            float3 pos : POSITION;
            float3 normal : NORMAL;
            float2 uv : TEXCOORD0;
            float4 tangent : TANGENT;
        };

        float4 main(VSIn i) : SV_Position {
            float4 posWS = mul(float4(i.pos, 1.0), gWorld);
            return mul(posWS, gLightSpaceVP);
        }
    )";

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    ComPtr<ID3DBlob> vsBlob, err;
    HRESULT hr = D3DCompile(kDepthVS, strlen(kDepthVS), nullptr, nullptr, nullptr,
        "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);

    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("Shadow depth VS compilation error: %s", (char*)err->GetBufferPointer());
        }
        return false;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, m_depthVS.GetAddressOf());

    // Input layout (same as MainPass for compatibility)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        m_inputLayout.GetAddressOf());

    // Constant buffers
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(CB_LightSpace);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&cbDesc, nullptr, m_cbLightSpace.GetAddressOf());

    cbDesc.ByteWidth = sizeof(CB_Object);
    device->CreateBuffer(&cbDesc, nullptr, m_cbObject.GetAddressOf());

    // --- Create default 1x1 white shadow map (depth=1.0, no shadow) ---
    {
        D3D11_TEXTURE2D_DESC shadowDesc{};
        shadowDesc.Width = 1;
        shadowDesc.Height = 1;
        shadowDesc.MipLevels = 1;
        shadowDesc.ArraySize = 1;
        shadowDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        shadowDesc.SampleDesc.Count = 1;
        shadowDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> defaultShadowTex;
        device->CreateTexture2D(&shadowDesc, nullptr, defaultShadowTex.GetAddressOf());

        // Create DSV to clear it
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> dsv;
        device->CreateDepthStencilView(defaultShadowTex.Get(), &dsvDesc, dsv.GetAddressOf());

        // Clear to 1.0 (no shadow)
        ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        // Create SRV for reading
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(defaultShadowTex.Get(), &srvDesc, m_defaultShadowMap.GetAddressOf());
    }

    // --- Create shadow sampler (Comparison sampler for PCF) ---
    {
        D3D11_SAMPLER_DESC sampDesc{};
        sampDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sampDesc.BorderColor[0] = 1.0f;  // Outside shadow map = no shadow
        sampDesc.BorderColor[1] = 1.0f;
        sampDesc.BorderColor[2] = 1.0f;
        sampDesc.BorderColor[3] = 1.0f;
        sampDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sampDesc.MinLOD = 0;
        sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sampDesc, m_shadowSampler.GetAddressOf());
    }

    // --- Create depth stencil state (depth test + write enabled) ---
    {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_LESS;
        dsd.StencilEnable = FALSE;
        device->CreateDepthStencilState(&dsd, m_depthState.GetAddressOf());
    }

    // --- Create rasterizer state (with depth bias to prevent shadow acne) ---
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthBias = 0;  // Will be set dynamically via DirectionalLight::ShadowBias
        rd.DepthBiasClamp = 0.0f;
        rd.SlopeScaledDepthBias = 0.0f;
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, m_rasterState.GetAddressOf());
    }

    // --- Initialize output bundle ---
    m_output.cascadeCount = 1;
    m_output.shadowMapArray = m_defaultShadowMap.Get();  // Default: no shadow
    m_output.shadowSampler = m_shadowSampler.Get();
    m_output.cascadeBlendRange = 0.0f;
    m_output.debugShowCascades = false;
    m_output.enableSoftShadows = true;  // Default to soft shadows enabled
    for (int i = 0; i < Output::MAX_CASCADES; ++i) {
        m_output.cascadeSplits[i] = 100.0f;
        m_output.lightSpaceVPs[i] = DirectX::XMMatrixIdentity();
    }

    return true;
}

void CShadowPass::Shutdown()
{
    m_shadowMapArray.Reset();
    m_shadowArraySRV.Reset();
    for (int i = 0; i < 4; ++i) {
        m_shadowDSVs[i].Reset();
    }
    m_defaultShadowMap.Reset();
    m_shadowSampler.Reset();
    m_depthVS.Reset();
    m_inputLayout.Reset();
    m_cbLightSpace.Reset();
    m_cbObject.Reset();
    m_depthState.Reset();
    m_rasterState.Reset();
}

void CShadowPass::ensureShadowMapArray(UINT size, int cascadeCount)
{
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return;

    if (size == 0) size = 2048;  // Default
    cascadeCount = std::min(std::max(cascadeCount, 1), 4);  // Clamp to [1, 4]

    // Already correct size and cascade count
    if (m_shadowMapArray && m_currentSize == size && m_currentCascadeCount == cascadeCount)
        return;

    // Reset all resources
    m_shadowMapArray.Reset();
    m_shadowArraySRV.Reset();
    for (int i = 0; i < 4; ++i) {
        m_shadowDSVs[i].Reset();
    }
    m_currentSize = size;
    m_currentCascadeCount = cascadeCount;

    // Create Texture2DArray (depth texture with multiple slices)
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = size;
    texDesc.Height = size;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = cascadeCount;  // Key: Array for CSM
    texDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    device->CreateTexture2D(&texDesc, nullptr, m_shadowMapArray.GetAddressOf());

    // Create per-slice DSVs (for rendering to each cascade)
    for (int i = 0; i < cascadeCount; ++i) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Texture2DArray.MipSlice = 0;
        device->CreateDepthStencilView(m_shadowMapArray.Get(), &dsvDesc,
                                       m_shadowDSVs[i].GetAddressOf());
    }

    // Create array SRV (for reading all cascades in MainPass)
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = cascadeCount;
    device->CreateShaderResourceView(m_shadowMapArray.Get(), &srvDesc,
                                     m_shadowArraySRV.GetAddressOf());
}

// ===========================
// CSM Helper Functions
// ===========================

std::vector<float> CShadowPass::calculateCascadeSplits(
    int cascadeCount,
    float nearPlane,
    float farPlane,
    float lambda) const
{
    std::vector<float> splits(cascadeCount + 1);
    splits[0] = nearPlane;
    splits[cascadeCount] = farPlane;

    // Practical Split Scheme (GPU Gems 3, Chapter 10)
    // Combines logarithmic and uniform distribution
    for (int i = 1; i < cascadeCount; ++i) {
        float p = (float)i / cascadeCount;

        // Logarithmic split (better for perspective aliasing)
        float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);

        // Uniform split (simpler, wastes resolution in distance)
        float uniformSplit = nearPlane + (farPlane - nearPlane) * p;

        // Blend between log and uniform using lambda
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }

    return splits;
}

std::array<XMFLOAT3, 8> CShadowPass::extractSubFrustum(
    const XMMATRIX& view,
    const XMMATRIX& proj,
    float nearDist,
    float farDist) const
{
    // Extract projection parameters from original projection matrix
    // Assuming perspective projection: proj._11 = 1/tan(fov/2)/aspect, proj._22 = 1/tan(fov/2)
    float fov = 2.0f * std::atan(1.0f / XMVectorGetY(proj.r[1]));
    float aspect = XMVectorGetX(proj.r[0]) / XMVectorGetY(proj.r[1]);

    // Create sub-frustum projection matrix with new near/far
    XMMATRIX subProj = XMMatrixPerspectiveFovLH(fov, aspect, nearDist, farDist);

    // Extract frustum corners using modified projection
    return extractFrustumCorners(view, subProj);
}

CShadowPass::BoundingSphere CShadowPass::calculateBoundingSphere(
    const std::array<XMFLOAT3, 8>& points) const
{
    // Simple bounding sphere: use centroid as center, max distance as radius
    // (Not optimal sphere, but good enough and fast)
    BoundingSphere sphere;

    // Calculate centroid
    XMVECTOR center = XMVectorZero();
    for (const auto& pt : points) {
        center = XMVectorAdd(center, XMLoadFloat3(&pt));
    }
    center = XMVectorScale(center, 1.0f / 8.0f);
    XMStoreFloat3(&sphere.center, center);

    // Calculate radius (max distance from center to any point)
    float maxDistSq = 0.0f;
    for (const auto& pt : points) {
        XMVECTOR p = XMLoadFloat3(&pt);
        XMVECTOR diff = XMVectorSubtract(p, center);
        float distSq = XMVectorGetX(XMVector3LengthSq(diff));
        maxDistSq = std::max(maxDistSq, distSq);
    }
    sphere.radius = std::sqrt(maxDistSq);

    return sphere;
}

// ===========================
// Tight Frustum Fitting
// ===========================

std::array<XMFLOAT3, 8> CShadowPass::extractFrustumCorners(
    const XMMATRIX& view,
    const XMMATRIX& proj) const
{
    // Inverse view-projection matrix: NDC -> World Space
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);

    // Frustum corners in NDC space ([-1, 1] range)
    XMFLOAT3 ndcCorners[8] = {
        {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
        {-1.0f,  1.0f, 0.0f}, {1.0f,  1.0f, 0.0f},
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
        {-1.0f,  1.0f, 1.0f}, {1.0f,  1.0f, 1.0f},
    };

    std::array<XMFLOAT3, 8> worldCorners;
    for (int i = 0; i < 8; ++i) {
        XMVECTOR ndc = XMLoadFloat3(&ndcCorners[i]);
        XMVECTOR world = XMVector3TransformCoord(ndc, invViewProj);
        XMStoreFloat3(&worldCorners[i], world);
    }

    return worldCorners;
}

XMMATRIX CShadowPass::calculateTightLightMatrix(
    const std::array<XMFLOAT3, 8>& frustumCornersWS,
    SDirectionalLight* light,
    float cascadeFarDist) const
{
    if (!light) return XMMatrixIdentity();

    XMFLOAT3 lightDir = light->GetDirection();
    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lightDir));

    // Step 1: Calculate bounding sphere for frustum corners
    // This sphere has FIXED radius for each cascade (since frustum is fixed)
    BoundingSphere sphere = calculateBoundingSphere(frustumCornersWS);
    // sphere.center = {0.0f, .0f, .0f};
    // sphere.radius = 16.0f; // Half diagonal of cube enclosing frustum
    XMVECTOR sphereCenter = XMLoadFloat3(&sphere.center);

    // Step 2: Build light space basis (fixed, only depends on light direction)
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMFLOAT3 lightDirF;
    XMStoreFloat3(&lightDirF, L);
    if (std::abs(lightDirF.y) > 0.99f) {
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    // Construct orthonormal basis for light space
    XMVECTOR lightRight = XMVector3Normalize(XMVector3Cross(up, L));
    XMVECTOR lightUp = XMVector3Cross(L, lightRight);

    // Step 3: Use sphere diameter as FIXED orthographic size
    float fixedOrthoSize = sphere.radius * 2.0f;

    // Step 4: TEXEL SNAPPING - Project sphere center onto light's perpendicular plane and align
    // Key insight: Align the sphere center in the plane perpendicular to light direction
    UINT shadowMapSize = (UINT)light->GetShadowMapResolution();
    float worldUnitsPerTexel = fixedOrthoSize / shadowMapSize;

    // Project sphere center onto light-right and light-up axes
    float centerRightComponent = XMVectorGetX(XMVector3Dot(sphereCenter, lightRight));
    float centerUpComponent = XMVectorGetX(XMVector3Dot(sphereCenter, lightUp));

    // Snap to texel grid in this perpendicular plane
    centerRightComponent = std::floor(centerRightComponent / worldUnitsPerTexel) * worldUnitsPerTexel;
    centerUpComponent = std::floor(centerUpComponent / worldUnitsPerTexel) * worldUnitsPerTexel;

    // Reconstruct aligned sphere center (preserve Z component along light direction)
    float centerForwardComponent = XMVectorGetX(XMVector3Dot(sphereCenter, L));
    XMVECTOR alignedSphereCenter = XMVectorAdd(
        XMVectorAdd(
            XMVectorScale(lightRight, centerRightComponent),
            XMVectorScale(lightUp, centerUpComponent)
        ),
        XMVectorScale(L, centerForwardComponent)
    );

    // Step 5: Build light view matrix with aligned center
    XMVECTOR lightPos = XMVectorSubtract(alignedSphereCenter, XMVectorScale(L, 100.0f));
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, alignedSphereCenter, lightUp);

    // Step 6: Build orthographic bounds (centered at origin in light space, since we use LookAt)
    float halfSize = fixedOrthoSize * 0.5f;
    float minX = -halfSize;
    float maxX = halfSize;
    float minY = -halfSize;
    float maxY = halfSize;

    // Step 7: Calculate Z bounds in light space (depth range doesn't need snapping)
    float minZ = FLT_MAX, maxZ = -FLT_MAX;
    for (const auto& corner : frustumCornersWS) {
        XMVECTOR lightSpacePos = XMVector3TransformCoord(XMLoadFloat3(&corner), lightView);
        float z = XMVectorGetZ(lightSpacePos);
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    }

    // Apply Shadow Near Plane Offset to capture tall objects
    float nearPlaneOffset = light->shadow_near_plane_offset;
    minZ -= nearPlaneOffset;
    maxZ += 10.0f;  // Small margin for far plane

    // Step 8: Build final orthographic projection with snapped XY bounds
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
        minX, maxX, minY, maxY, minZ, maxZ
    );

    return lightView * lightProj;
}

//DirectX::XMMATRIX ShadowPass::calculateLightSpaceMatrix(DirectionalLight* light, float shadowDistance, float shadowExtension) const
//{
//    if (!light) return XMMatrixIdentity();
//
//    // Get light direction from DirectionalLight component
//    XMFLOAT3 lightDir = light->GetDirection();
//    XMVECTOR L = XMLoadFloat3(&lightDir);
//    L = XMVector3Normalize(L);
//
//    // Position light far away in opposite direction of light
//    XMVECTOR lightPos = -L * std::max(shadowDistance,1.0f);
//
//    // Look at origin from light position
//    XMVECTOR target = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
//    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
//
//    // If light is pointing straight down/up, use different up vector
//    XMFLOAT3 lightDirF;
//    XMStoreFloat3(&lightDirF, L);
//    if (abs(lightDirF.y) > 0.99f) {
//        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
//    }
//
//    XMMATRIX view = XMMatrixLookAtLH(lightPos, target, up);
//
//    // Orthographic projection (directional light is parallel)
//    float orthoSize = shadowDistance * 0.5f;  // Half-size of frustum
//    XMMATRIX proj = XMMatrixOrthographicLH(
//        orthoSize * 2.0f,  // width
//        orthoSize * 2.0f,  // height
//        0.1f,              // near
//        shadowDistance * 2.0f  // far
//    );
//
//    return view * proj;
//}

void CShadowPass::Render(CScene& scene, SDirectionalLight* light,
                        const XMMATRIX& cameraView,
                        const XMMATRIX& cameraProj)
{
    ID3D11DeviceContext* context = CDX11Context::Instance().GetContext();
    if (!context || !light) return;

    // Get CSM parameters from light
    int cascadeCount = std::min(std::max(light->cascade_count, 1), 4);
    float shadowDistance = light->shadow_distance;
    UINT shadowMapSize = (UINT)light->GetShadowMapResolution();

    // Unbind resources before recreating shadow maps to avoid hazards
    ID3D11ShaderResourceView* nullSRV[8] = { nullptr };
    context->PSSetShaderResources(0, 8, nullSRV);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    // Ensure shadow map array resources
    ensureShadowMapArray(shadowMapSize, cascadeCount);

    // Calculate cascade split distances (in camera space)
    float cameraNear = 0.1f;  // TODO: Get from camera settings
    auto splits = calculateCascadeSplits(cascadeCount, cameraNear, shadowDistance,
                                         std::clamp(light->cascade_split_lambda, 0.0f, 1.0f));

    // Bind pipeline (shared across all cascades)
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_depthVS.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);  // Depth-only, no pixel shader
    context->VSSetConstantBuffers(0, 1, m_cbLightSpace.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, m_cbObject.GetAddressOf());

    // Set render states
    context->OMSetDepthStencilState(m_depthState.Get(), 0);
    context->RSSetState(m_rasterState.Get());

    // Set viewport (same for all cascades)
    D3D11_VIEWPORT vp{};
    vp.Width = (FLOAT)shadowMapSize;
    vp.Height = (FLOAT)shadowMapSize;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // Render each cascade
    for (int cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        // Extract sub-frustum for this cascade
        auto subFrustumCorners = extractSubFrustum(cameraView, cameraProj,
                                                    splits[cascadeIndex],
                                                    splits[cascadeIndex + 1]);

        // Calculate light space matrix with fixed square bounds for this cascade
        float cascadeFar = splits[cascadeIndex + 1];
        XMMATRIX lightSpaceVP = calculateTightLightMatrix(subFrustumCorners, light, cascadeFar);

        // Update light space constant buffer
        CB_LightSpace cbLight;
        cbLight.lightSpaceVP = XMMatrixTranspose(lightSpaceVP);
        context->UpdateSubresource(m_cbLightSpace.Get(), 0, nullptr, &cbLight, 0, 0);

        // Bind this cascade's DSV
        ID3D11RenderTargetView* nullRTV = nullptr;
        context->OMSetRenderTargets(0, &nullRTV, m_shadowDSVs[cascadeIndex].Get());

        // Clear depth for this cascade
        context->ClearDepthStencilView(m_shadowDSVs[cascadeIndex].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        // Render all objects to this cascade
        for (auto& objPtr : scene.GetWorld().Objects()) {
            auto* obj = objPtr.get();
            auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
            auto* transform = obj->GetComponent<STransform>();

            if (!meshRenderer || !transform) continue;

            // Ensure mesh is uploaded
            meshRenderer->EnsureUploaded();
            if (meshRenderer->meshes.empty()) continue;

            // Get world matrix
            XMMATRIX worldMatrix = transform->WorldMatrix();

            // Render all sub-meshes
            for (auto& gpuMesh : meshRenderer->meshes) {
                if (!gpuMesh) continue;

                // Update object constant buffer
                CB_Object cbObj;
                cbObj.world = XMMatrixTranspose(worldMatrix);
                context->UpdateSubresource(m_cbObject.Get(), 0, nullptr, &cbObj, 0, 0);

                // Bind vertex/index buffers
                UINT stride = sizeof(SVertexPNT), offset = 0;
                ID3D11Buffer* vbo = gpuMesh->vbo.Get();
                context->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
                context->IASetIndexBuffer(gpuMesh->ibo.Get(), DXGI_FORMAT_R32_UINT, 0);

                // Draw
                context->DrawIndexed(gpuMesh->indexCount, 0, 0);
            }
        }

        // Save cascade data to output
        m_output.lightSpaceVPs[cascadeIndex] = lightSpaceVP;
        m_output.cascadeSplits[cascadeIndex] = splits[cascadeIndex + 1];  // Far plane distance
    }

    // Unbind DSV to allow reading as SRV in MainPass
    context->OMSetRenderTargets(0, nullptr, nullptr);

    // Update output bundle
    m_output.cascadeCount = cascadeCount;
    m_output.shadowMapArray = m_shadowArraySRV.Get();
    m_output.shadowSampler = m_shadowSampler.Get();
    m_output.cascadeBlendRange = std::clamp(light->cascade_blend_range, 0.0f, 0.5f);
    m_output.debugShowCascades = light->debug_show_cascades;
    m_output.enableSoftShadows = light->enable_soft_shadows;
}
