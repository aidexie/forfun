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
    m_output.shadowMap = m_defaultShadowMap.Get();  // Default: no shadow
    m_output.shadowSampler = m_shadowSampler.Get();
    m_output.lightSpaceVP = DirectX::XMMatrixIdentity();

    return true;
}

void ShadowPass::Shutdown()
{
    m_shadowMap.Reset();
    m_shadowDSV.Reset();
    m_shadowSRV.Reset();
    m_defaultShadowMap.Reset();
    m_shadowSampler.Reset();
    m_depthVS.Reset();
    m_inputLayout.Reset();
    m_cbLightSpace.Reset();
    m_cbObject.Reset();
}

void ShadowPass::ensureShadowMap(UINT size)
{
    ID3D11Device* device = DX11Context::Instance().GetDevice();
    if (!device) return;

    if (size == 0) size = 2048;  // Default

    // Already correct size
    if (m_shadowMap && m_currentSize == size) return;

    m_shadowMap.Reset();
    m_shadowDSV.Reset();
    m_shadowSRV.Reset();
    m_currentSize = size;

    // Create depth texture (24-bit depth, 8-bit stencil)
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = size;
    texDesc.Height = size;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;  // Typeless for flexible views
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    device->CreateTexture2D(&texDesc, nullptr, m_shadowMap.GetAddressOf());

    // DSV (for writing depth)
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, m_shadowDSV.GetAddressOf());

    // SRV (for reading in MainPass)
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_shadowMap.Get(), &srvDesc, m_shadowSRV.GetAddressOf());
}
#include <algorithm>
XMMATRIX ShadowPass::calculateLightSpaceMatrix(DirectionalLight* light, float shadowDistance)
{
    if (!light) return XMMatrixIdentity();

    // Get light direction from DirectionalLight component
    XMFLOAT3 lightDir = light->GetDirection();
    XMVECTOR L = XMLoadFloat3(&lightDir);
    L = XMVector3Normalize(L);

    // Position light far away in opposite direction of light
    XMVECTOR lightPos = -L * std::max(shadowDistance,1.0f);

    // Look at origin from light position
    XMVECTOR target = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    // If light is pointing straight down/up, use different up vector
    XMFLOAT3 lightDirF;
    XMStoreFloat3(&lightDirF, L);
    if (abs(lightDirF.y) > 0.99f) {
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    XMMATRIX view = XMMatrixLookAtLH(lightPos, target, up);

    // Orthographic projection (directional light is parallel)
    float orthoSize = shadowDistance * 0.5f;  // Half-size of frustum
    XMMATRIX proj = XMMatrixOrthographicLH(
        orthoSize * 2.0f,  // width
        orthoSize * 2.0f,  // height
        0.1f,              // near
        shadowDistance * 2.0f  // far
    );

    return view * proj;
}

void ShadowPass::Render(Scene& scene, DirectionalLight* light)
{
    ID3D11DeviceContext* context = DX11Context::Instance().GetContext();
    if (!context || !light) return;

    // Get shadow map size from light settings
    UINT shadowMapSize = (UINT)light->GetShadowMapResolution();
    ensureShadowMap(shadowMapSize);

    // Calculate light space matrix
    m_lightSpaceVP = calculateLightSpaceMatrix(light, light->ShadowDistance);

    // Update constant buffer
    CB_LightSpace cbLight;
    cbLight.lightSpaceVP = XMMatrixTranspose(m_lightSpaceVP);
    context->UpdateSubresource(m_cbLightSpace.Get(), 0, nullptr, &cbLight, 0, 0);

    // Bind shadow map DSV (no color target needed for depth-only)
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(0, &nullRTV, m_shadowDSV.Get());

    // Clear depth
    context->ClearDepthStencilView(m_shadowDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Set viewport
    D3D11_VIEWPORT vp{};
    vp.Width = (FLOAT)m_currentSize;
    vp.Height = (FLOAT)m_currentSize;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // Bind pipeline
    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_depthVS.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);  // No pixel shader needed for depth-only
    context->VSSetConstantBuffers(0, 1, m_cbLightSpace.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, m_cbObject.GetAddressOf());

    // Render all objects with MeshRenderer
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

    // Unbind DSV to allow reading as SRV in MainPass
    context->OMSetRenderTargets(0, nullptr, nullptr);

    // Update output bundle with rendered shadow map
    m_output.shadowMap = m_shadowSRV.Get();
    m_output.shadowSampler = m_shadowSampler.Get();
    m_output.lightSpaceVP = m_lightSpaceVP;
}
