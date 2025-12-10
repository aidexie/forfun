#include "ShadowPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
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
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace RHI;

#pragma comment(lib, "d3dcompiler.lib")

struct alignas(16) CB_LightSpace {
    DirectX::XMMATRIX lightSpaceVP;
};

struct alignas(16) CB_Object {
    DirectX::XMMATRIX world;
};

bool CShadowPass::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

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

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kDepthVS, strlen(kDepthVS), nullptr, nullptr, nullptr,
        "main", "vs_5_0", compileFlags, 0, &vsBlob, &err);

    if (FAILED(hr)) {
        if (err) {
            CFFLog::Error("Shadow depth VS compilation error: %s", (char*)err->GetBufferPointer());
            err->Release();
        }
        return false;
    }

    // Create vertex shader using RHI
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsBlob->GetBufferPointer();
    vsDesc.bytecodeSize = vsBlob->GetBufferSize();
    m_depthVS.reset(ctx->CreateShader(vsDesc));

    // Create pipeline state
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_depthVS.get();
    psoDesc.pixelShader = nullptr;  // Depth-only, no pixel shader

    // Input layout (same as MainPass for compatibility)
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 0, 12 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 0, 24 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 0, 32 }
    };

    // Rasterizer state (with depth bias to prevent shadow acne)
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.cullMode = ECullMode::Back;
    psoDesc.rasterizer.depthClipEnable = true;

    // Depth stencil state (depth test + write enabled)
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = true;
    psoDesc.depthStencil.depthFunc = EComparisonFunc::Less;

    // No blending
    psoDesc.blend.blendEnable = false;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    vsBlob->Release();

    // Constant buffers (GPU-writable for UpdateSubresource pattern)
    BufferDesc cbLightDesc;
    cbLightDesc.size = sizeof(CB_LightSpace);
    cbLightDesc.usage = EBufferUsage::Constant;
    cbLightDesc.cpuAccess = ECPUAccess::None;  // GPU-writable via UpdateSubresource
    m_cbLightSpace.reset(ctx->CreateBuffer(cbLightDesc, nullptr));

    BufferDesc cbObjDesc;
    cbObjDesc.size = sizeof(CB_Object);
    cbObjDesc.usage = EBufferUsage::Constant;
    cbObjDesc.cpuAccess = ECPUAccess::None;
    m_cbObject.reset(ctx->CreateBuffer(cbObjDesc, nullptr));

    // Create default 1x1 white shadow map (depth=1.0, no shadow)
    {
        TextureDesc desc = TextureDesc::DepthStencilWithSRV(1, 1);
        desc.debugName = "Default_ShadowMap";
        m_defaultShadowMap.reset(ctx->CreateTexture(desc, nullptr));

        // Clear to 1.0 (no shadow) via D3D11 context
        ID3D11DeviceContext* d3dCtx = static_cast<ID3D11DeviceContext*>(ctx->GetNativeContext());
        ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(m_defaultShadowMap->GetDSV());
        if (dsv && d3dCtx) {
            d3dCtx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        }
    }

    // Create shadow sampler (Comparison sampler for PCF)
    SamplerDesc samplerDesc;
    samplerDesc.filter = EFilter::ComparisonMinMagMipLinear;
    samplerDesc.addressU = ETextureAddressMode::Border;
    samplerDesc.addressV = ETextureAddressMode::Border;
    samplerDesc.addressW = ETextureAddressMode::Border;
    samplerDesc.borderColor[0] = 1.0f;  // Outside shadow map = no shadow
    samplerDesc.borderColor[1] = 1.0f;
    samplerDesc.borderColor[2] = 1.0f;
    samplerDesc.borderColor[3] = 1.0f;
    samplerDesc.comparisonFunc = EComparisonFunc::LessEqual;
    m_shadowSampler.reset(ctx->CreateSampler(samplerDesc));

    // Initialize output bundle
    m_output.cascadeCount = 1;
    m_output.shadowMapArray = m_defaultShadowMap.get();
    m_output.shadowSampler = m_shadowSampler.get();
    m_output.cascadeBlendRange = 0.0f;
    m_output.debugShowCascades = false;
    m_output.enableSoftShadows = true;
    for (int i = 0; i < Output::MAX_CASCADES; ++i) {
        m_output.cascadeSplits[i] = 100.0f;
        m_output.lightSpaceVPs[i] = DirectX::XMMatrixIdentity();
    }

    return true;
}

void CShadowPass::Shutdown()
{
    m_shadowMapArray.reset();
    for (int i = 0; i < 4; ++i) {
        m_shadowDSVs[i].Reset();
    }
    m_defaultShadowMap.reset();
    m_shadowSampler.reset();
    m_depthVS.reset();
    m_pso.reset();
    m_cbLightSpace.reset();
    m_cbObject.reset();
}

void CShadowPass::ensureShadowMapArray(uint32_t size, int cascadeCount)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ID3D11Device* device = static_cast<ID3D11Device*>(ctx->GetNativeDevice());
    if (!ctx || !device) return;

    if (size == 0) size = 2048;  // Default
    cascadeCount = std::min(std::max(cascadeCount, 1), 4);  // Clamp to [1, 4]

    // Already correct size and cascade count
    if (m_shadowMapArray && m_currentSize == size && m_currentCascadeCount == cascadeCount)
        return;

    // Reset all resources
    m_shadowMapArray.reset();
    for (int i = 0; i < 4; ++i) {
        m_shadowDSVs[i].Reset();
    }
    m_currentSize = size;
    m_currentCascadeCount = cascadeCount;

    // Create Texture2DArray via RHI (depth texture with array slices and SRV)
    TextureDesc desc = TextureDesc::DepthStencilArrayWithSRV(size, size, cascadeCount);
    desc.debugName = "ShadowMapArray";
    m_shadowMapArray.reset(ctx->CreateTexture(desc, nullptr));

    // Get native texture handle for creating per-slice DSVs
    // Note: Per-slice DSV creation is D3D11-specific until Phase 6 RHI extension
    ID3D11Texture2D* nativeTexture = static_cast<ID3D11Texture2D*>(m_shadowMapArray->GetNativeHandle());
    if (!nativeTexture) return;

    // Create per-slice DSVs (for rendering to each cascade)
    for (int i = 0; i < cascadeCount; ++i) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Texture2DArray.MipSlice = 0;
        device->CreateDepthStencilView(nativeTexture, &dsvDesc,
                                       m_shadowDSVs[i].GetAddressOf());
    }
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
    BoundingSphere sphere = calculateBoundingSphere(frustumCornersWS);
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
    uint32_t shadowMapSize = (uint32_t)light->GetShadowMapResolution();
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

void CShadowPass::Render(CScene& scene, SDirectionalLight* light,
                        const XMMATRIX& cameraView,
                        const XMMATRIX& cameraProj)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();
    ID3D11DeviceContext* d3dCtx = static_cast<ID3D11DeviceContext*>(ctx->GetNativeContext());

    if (!d3dCtx || !light) return;

    // Get CSM parameters from light
    int cascadeCount = std::min(std::max(light->cascade_count, 1), 4);
    float shadowDistance = light->shadow_distance;
    uint32_t shadowMapSize = (uint32_t)light->GetShadowMapResolution();

    // Unbind resources before recreating shadow maps to avoid hazards
    ID3D11ShaderResourceView* nullSRV[8] = { nullptr };
    d3dCtx->PSSetShaderResources(0, 8, nullSRV);
    d3dCtx->OMSetRenderTargets(0, nullptr, nullptr);

    // Ensure shadow map array resources
    ensureShadowMapArray(shadowMapSize, cascadeCount);

    // Calculate cascade split distances (in camera space)
    float cameraNear = 0.1f;  // TODO: Get from camera settings
    auto splits = calculateCascadeSplits(cascadeCount, cameraNear, shadowDistance,
                                         std::clamp(light->cascade_split_lambda, 0.0f, 1.0f));

    // Set pipeline state via RHI
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Bind constant buffers via RHI
    cmdList->SetConstantBuffer(EShaderStage::Vertex, 0, m_cbLightSpace.get());
    cmdList->SetConstantBuffer(EShaderStage::Vertex, 1, m_cbObject.get());

    // Set viewport
    cmdList->SetViewport(0.0f, 0.0f, (float)shadowMapSize, (float)shadowMapSize, 0.0f, 1.0f);

    // Render each cascade
    for (int cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        // Extract sub-frustum for this cascade
        auto subFrustumCorners = extractSubFrustum(cameraView, cameraProj,
                                                    splits[cascadeIndex],
                                                    splits[cascadeIndex + 1]);

        // Calculate light space matrix with fixed square bounds for this cascade
        float cascadeFar = splits[cascadeIndex + 1];
        XMMATRIX lightSpaceVP = calculateTightLightMatrix(subFrustumCorners, light, cascadeFar);

        // Update light space constant buffer via D3D11 (UpdateSubresource)
        CB_LightSpace cbLight;
        cbLight.lightSpaceVP = XMMatrixTranspose(lightSpaceVP);
        d3dCtx->UpdateSubresource(static_cast<ID3D11Buffer*>(m_cbLightSpace->GetNativeHandle()),
                                  0, nullptr, &cbLight, 0, 0);

        // Bind this cascade's DSV (D3D11-specific until Phase 6 RHI extension)
        ID3D11RenderTargetView* nullRTV = nullptr;
        d3dCtx->OMSetRenderTargets(0, &nullRTV, m_shadowDSVs[cascadeIndex].Get());

        // Clear depth for this cascade
        d3dCtx->ClearDepthStencilView(m_shadowDSVs[cascadeIndex].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

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

                // Update object constant buffer via D3D11
                CB_Object cbObj;
                cbObj.world = XMMatrixTranspose(worldMatrix);
                d3dCtx->UpdateSubresource(static_cast<ID3D11Buffer*>(m_cbObject->GetNativeHandle()),
                                          0, nullptr, &cbObj, 0, 0);

                // Bind vertex/index buffers via RHI
                cmdList->SetVertexBuffer(0, gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
                cmdList->SetIndexBuffer(gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);

                // Draw via RHI
                cmdList->DrawIndexed(gpuMesh->indexCount, 0, 0);
            }
        }

        // Save cascade data to output
        m_output.lightSpaceVPs[cascadeIndex] = lightSpaceVP;
        m_output.cascadeSplits[cascadeIndex] = splits[cascadeIndex + 1];  // Far plane distance
    }

    // Unbind DSV to allow reading as SRV in MainPass
    d3dCtx->OMSetRenderTargets(0, nullptr, nullptr);

    // Update output bundle
    m_output.cascadeCount = cascadeCount;
    m_output.shadowMapArray = m_shadowMapArray.get();
    m_output.shadowSampler = m_shadowSampler.get();
    m_output.cascadeBlendRange = std::clamp(light->cascade_blend_range, 0.0f, 0.5f);
    m_output.debugShowCascades = light->debug_show_cascades;
    m_output.enableSoftShadows = light->enable_soft_shadows;
}
