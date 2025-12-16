#include "ShadowPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Scene.h"
#include "GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include <algorithm>
#include <cstring>

using namespace DirectX;
using namespace RHI;

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

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader vsCompiled = CompileShaderFromSource(kDepthVS, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("Shadow depth VS compilation error: %s", vsCompiled.errorMessage.c_str());
        return false;
    }

    // Create vertex shader using RHI
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    m_depthVS.reset(ctx->CreateShader(vsDesc));

    // Create pipeline state
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_depthVS.get();
    psoDesc.pixelShader = nullptr;  // Depth-only, no pixel shader

    // Input layout (same as MainPass for compatibility)
    // VertexElement: { semantic, semanticIndex, format, offset, inputSlot }
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 }
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

    // Depth-only pass: no render targets, only depth stencil
    psoDesc.renderTargetFormats = {};  // Empty - no color outputs
    psoDesc.depthStencilFormat = ETextureFormat::D24_UNORM_S8_UINT;

    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    // Constant buffers (CPU-writable for Map/Unmap pattern)
    BufferDesc cbLightDesc;
    cbLightDesc.size = sizeof(CB_LightSpace);
    cbLightDesc.usage = EBufferUsage::Constant;
    cbLightDesc.cpuAccess = ECPUAccess::Write;
    m_cbLightSpace.reset(ctx->CreateBuffer(cbLightDesc, nullptr));

    BufferDesc cbObjDesc;
    cbObjDesc.size = sizeof(CB_Object);
    cbObjDesc.usage = EBufferUsage::Constant;
    cbObjDesc.cpuAccess = ECPUAccess::Write;
    m_cbObject.reset(ctx->CreateBuffer(cbObjDesc, nullptr));

    // Create default 1x1 white shadow map (depth=1.0, no shadow)
    {
        TextureDesc desc = TextureDesc::DepthStencilWithSRV(1, 1);
        desc.debugName = "Default_ShadowMap";
        m_defaultShadowMap.reset(ctx->CreateTexture(desc, nullptr));

        // Clear to 1.0 (no shadow) via RHI
        ICommandList* cmdList = ctx->GetCommandList();
        cmdList->ClearDepthStencil(m_defaultShadowMap.get(), true, 1.0f, false, 0);
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
    if (!ctx) return;

    if (size == 0) size = 2048;  // Default
    cascadeCount = std::min(std::max(cascadeCount, 1), 4);  // Clamp to [1, 4]

    // Already correct size and cascade count
    if (m_shadowMapArray && m_currentSize == size && m_currentCascadeCount == cascadeCount)
        return;

    // Reset resources
    m_shadowMapArray.reset();
    m_currentSize = size;
    m_currentCascadeCount = cascadeCount;

    // Create Texture2DArray via RHI (depth texture with array slices and SRV)
    // RHI now automatically creates per-slice DSVs for array textures
    TextureDesc desc = TextureDesc::DepthStencilArrayWithSRV(size, size, cascadeCount);
    desc.debugName = "ShadowMapArray";
    m_shadowMapArray.reset(ctx->CreateTexture(desc, nullptr));
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

    if (!cmdList || !light) return;

    // Get CSM parameters from light
    int cascadeCount = std::min(std::max(light->cascade_count, 1), 4);
    float shadowDistance = light->shadow_distance;
    uint32_t shadowMapSize = (uint32_t)light->GetShadowMapResolution();

    // Unbind render targets to avoid hazards
    cmdList->SetRenderTargets(0, nullptr, nullptr);

    // Ensure shadow map array resources
    ensureShadowMapArray(shadowMapSize, cascadeCount);

    // Calculate cascade split distances (in camera space)
    float cameraNear = 0.1f;  // TODO: Get from camera settings
    auto splits = calculateCascadeSplits(cascadeCount, cameraNear, shadowDistance,
                                         std::clamp(light->cascade_split_lambda, 0.0f, 1.0f));

    // Set pipeline state via RHI
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Note: Constant buffers are bound per-draw using SetConstantBufferData
    // This allocates from a dynamic ring buffer, giving each draw its own memory

    // Set viewport and scissor rect (DX12 requires both)
    cmdList->SetViewport(0.0f, 0.0f, (float)shadowMapSize, (float)shadowMapSize, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, shadowMapSize, shadowMapSize);

    // Render each cascade
    for (int cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        // Extract sub-frustum for this cascade
        auto subFrustumCorners = extractSubFrustum(cameraView, cameraProj,
                                                    splits[cascadeIndex],
                                                    splits[cascadeIndex + 1]);

        // Calculate light space matrix with fixed square bounds for this cascade
        float cascadeFar = splits[cascadeIndex + 1];
        XMMATRIX lightSpaceVP = calculateTightLightMatrix(subFrustumCorners, light, cascadeFar);

        // Update light space constant buffer using dynamic allocation (DX12 compatible)
        CB_LightSpace cbLight;
        cbLight.lightSpaceVP = XMMatrixTranspose(lightSpaceVP);
        cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &cbLight, sizeof(CB_LightSpace));

        // Bind this cascade's DSV via RHI
        cmdList->SetDepthStencilOnly(m_shadowMapArray.get(), cascadeIndex);

        // Clear depth for this cascade via RHI
        cmdList->ClearDepthStencilSlice(m_shadowMapArray.get(), cascadeIndex, true, 1.0f, false, 0);

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

                // Update object constant buffer using dynamic allocation (DX12 compatible)
                CB_Object cbObj;
                cbObj.world = XMMatrixTranspose(worldMatrix);
                cmdList->SetConstantBufferData(EShaderStage::Vertex, 1, &cbObj, sizeof(CB_Object));

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
    cmdList->SetRenderTargets(0, nullptr, nullptr);

    // Update output bundle
    m_output.cascadeCount = cascadeCount;
    m_output.shadowMapArray = m_shadowMapArray.get();
    m_output.shadowSampler = m_shadowSampler.get();
    m_output.cascadeBlendRange = std::clamp(light->cascade_blend_range, 0.0f, 0.5f);
    m_output.debugShowCascades = light->debug_show_cascades;
    m_output.enableSoftShadows = light->enable_soft_shadows;
}
