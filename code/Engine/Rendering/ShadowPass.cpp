#include "ShadowPass.h"
#include "Core/DX11Context.h"
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

bool ShadowPass::Initialize()
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
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
            OutputDebugStringA((char*)err->GetBufferPointer());
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
        ID3D11DeviceContext* context = DX11Context::Instance().GetContext();
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

    // --- Initialize output bundle ---
    m_output.cascadeCount = 1;
    m_output.shadowMapArray = m_defaultShadowMap.Get();  // Default: no shadow
    m_output.shadowSampler = m_shadowSampler.Get();
    m_output.debugShowCascades = false;
    for (int i = 0; i < Output::MAX_CASCADES; ++i) {
        m_output.cascadeSplits[i] = 100.0f;
        m_output.lightSpaceVPs[i] = DirectX::XMMatrixIdentity();
    }

    return true;
}

void ShadowPass::Shutdown()
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
}

void ShadowPass::ensureShadowMapArray(UINT size, int cascadeCount)
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
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

std::vector<float> ShadowPass::calculateCascadeSplits(
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

std::array<XMFLOAT3, 8> ShadowPass::extractSubFrustum(
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

// ===========================
// Tight Frustum Fitting
// ===========================

std::array<XMFLOAT3, 8> ShadowPass::extractFrustumCorners(
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

XMMATRIX ShadowPass::calculateTightLightMatrix(
    const std::array<XMFLOAT3, 8>& frustumCornersWS,
    DirectionalLight* light,
    float shadowExtension) const
{
    if (!light) return XMMatrixIdentity();

    XMFLOAT3 lightDir = light->GetDirection();
    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lightDir));

    // // Extend far plane corners
    std::array<XMFLOAT3, 8> extendedCorners = frustumCornersWS;
    //XMVECTOR offset = XMVectorScale(L, -shadowExtension);
    //for (int i = 4; i < 8; ++i) {
    //    XMVECTOR corner = XMLoadFloat3(&extendedCorners[i]);
    //    corner = XMVectorAdd(corner, offset);
    //    XMStoreFloat3(&extendedCorners[i], corner);
    //}

    // Compute center
    XMVECTOR center = XMVectorZero();
    for (const auto& corner : extendedCorners) {
        center = XMVectorAdd(center, XMLoadFloat3(&corner));
    }
    center = XMVectorScale(center, 1.0f / 8.0f);

    // Build light view matrix
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMFLOAT3 lightDirF;
    XMStoreFloat3(&lightDirF, L);
    if (std::abs(lightDirF.y) > 0.99f) {
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    XMVECTOR lightPos = XMVectorSubtract(center, XMVectorScale(L, std::max(shadowExtension, 1.0f) * 2.0f));
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, center, up);

    // Compute AABB in light space
    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    float minZ = FLT_MAX, maxZ = -FLT_MAX;

    for (const auto& corner : extendedCorners) {
        XMVECTOR lightSpacePos = XMVector3TransformCoord(XMLoadFloat3(&corner), lightView);
        float x = XMVectorGetX(lightSpacePos);
        float y = XMVectorGetY(lightSpacePos);
        float z = XMVectorGetZ(lightSpacePos);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    }

    const float margin = 10.0f;
    minX -= margin; maxX += margin;
    minY -= margin; maxY += margin;
    minZ -= margin; maxZ += margin;

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

void ShadowPass::Render(Scene& scene, DirectionalLight* light,
                        const XMMATRIX& cameraView,
                        const XMMATRIX& cameraProj)
{
    ID3D11DeviceContext* context = DX11Context::Instance().GetContext();
    if (!context || !light) return;

    // Get CSM parameters from light
    int cascadeCount = std::min(std::max(light->CascadeCount, 1), 4);
    float shadowDistance = light->ShadowDistance;
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
                                         std::clamp(light->CascadeSplitLambda, 0.0f, 1.0f));

    // Bind pipeline (shared across all cascades)
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_depthVS.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);  // Depth-only, no pixel shader
    context->VSSetConstantBuffers(0, 1, m_cbLightSpace.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, m_cbObject.GetAddressOf());

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

        // Calculate tight-fitting light space matrix for this cascade
        XMMATRIX lightSpaceVP = calculateTightLightMatrix(subFrustumCorners, light,
                                                           light->ShadowDistance);

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
        for (auto& objPtr : scene.world.Objects()) {
            auto* obj = objPtr.get();
            auto* meshRenderer = obj->GetComponent<MeshRenderer>();
            auto* transform = obj->GetComponent<Transform>();

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
                UINT stride = sizeof(VertexPNT), offset = 0;
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
    m_output.debugShowCascades = light->DebugShowCascades;
}
