#include "TransparentForwardPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/GpuMeshResource.h"
#include "Core/MaterialManager.h"
#include "Core/TextureManager.h"
#include "Core/Mesh.h"
#include "Engine/Scene.h"
#include "Engine/Camera.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Rendering/ClusteredLightingPass.h"
#include "Engine/Rendering/ReflectionProbeManager.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

namespace {

// RenderItem for transparent objects
struct TransparentItem {
    CGameObject* obj;
    SMeshRenderer* meshRenderer;
    STransform* transform;
    CMaterialAsset* material;
    XMMATRIX worldMatrix;
    float distanceToCamera;
    GpuMeshResource* gpuMesh;
    ITexture* albedoTex;
    ITexture* normalTex;
    ITexture* metallicRoughnessTex;
    ITexture* emissiveTex;
    bool hasRealMetallicRoughnessTexture;
    bool hasRealEmissiveMap;
    int probeIndex;
    int lightmapIndex;
};

// CB_Frame structure (must match MainPass.ps.hlsl)
struct alignas(16) CB_Frame {
    XMMATRIX view;
    XMMATRIX proj;
    int cascadeCount;
    int debugShowCascades;
    int enableSoftShadows;
    float cascadeBlendRange;
    XMFLOAT4 cascadeSplits;
    XMMATRIX lightSpaceVPs[4];
    XMFLOAT3 lightDirWS; float _pad1;
    XMFLOAT3 lightColor; float _pad2;
    XMFLOAT3 camPosWS; float _pad3;
    float shadowBias;
    float iblIntensity;
    int diffuseGIMode;
    float _pad4;
};

// CB_Object structure (must match MainPass shaders)
struct alignas(16) CB_Object {
    XMMATRIX world;
    XMFLOAT3 albedo; float metallic;
    XMFLOAT3 emissive; float roughness;
    float emissiveStrength;
    int hasMetallicRoughnessTexture;
    int hasEmissiveMap;
    int alphaMode;
    float alphaCutoff;
    int probeIndex;
    int lightmapIndex;
    float _padObj;
};

std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // anonymous namespace

bool CTransparentForwardPass::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    createPipeline();

    CFFLog::Info("TransparentForwardPass initialized");
    return true;
}

void CTransparentForwardPass::Shutdown()
{
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_cbFrame.reset();
    m_cbObject.reset();
    m_linearSampler.reset();
    m_shadowSampler.reset();
}

void CTransparentForwardPass::createPipeline()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";
    std::string vsSource = LoadShaderSource(shaderDir + "MainPass.vs.hlsl");
    std::string psSource = LoadShaderSource(shaderDir + "MainPass.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("TransparentForwardPass: Failed to load MainPass shaders");
        return;
    }

    CDefaultShaderIncludeHandler includeHandler(shaderDir);

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource, "main", "vs_5_0", &includeHandler, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("TransparentForwardPass VS error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    SCompiledShader psCompiled = CompileShaderFromSource(psSource, "main", "ps_5_0", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("TransparentForwardPass PS error: %s", psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "TransparentForward_VS";
    m_vs.reset(ctx->CreateShader(vsDesc));

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "TransparentForward_PS";
    m_ps.reset(ctx->CreateShader(psDesc));

    // Input layout (matches SVertexPNT)
    std::vector<VertexElement> inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 },
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 },
        { EVertexSemantic::Texcoord, 1, EVertexFormat::Float2, 64, 0 }  // UV2 for lightmap
    };

    // Transparent PSO: depth read-only, alpha blending
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();
    psoDesc.inputLayout = inputLayout;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.cullMode = ECullMode::Back;
    psoDesc.rasterizer.frontCounterClockwise = false;
    psoDesc.rasterizer.depthClipEnable = true;

    // Depth: read-only (no write) with LessEqual
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = EComparisonFunc::LessEqual;

    // Alpha blending: SrcAlpha * Src + InvSrcAlpha * Dst
    psoDesc.blend.blendEnable = true;
    psoDesc.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoDesc.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoDesc.blend.blendOp = EBlendOp::Add;
    psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
    psoDesc.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoDesc.blend.blendOpAlpha = EBlendOp::Add;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;  // Match G-Buffer depth format
    psoDesc.debugName = "TransparentForward_PSO";

    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    // Constant buffers
    BufferDesc cbFrameDesc;
    cbFrameDesc.size = sizeof(CB_Frame);
    cbFrameDesc.usage = EBufferUsage::Constant;
    cbFrameDesc.cpuAccess = ECPUAccess::Write;
    m_cbFrame.reset(ctx->CreateBuffer(cbFrameDesc, nullptr));

    BufferDesc cbObjDesc;
    cbObjDesc.size = sizeof(CB_Object);
    cbObjDesc.usage = EBufferUsage::Constant;
    cbObjDesc.cpuAccess = ECPUAccess::Write;
    m_cbObject.reset(ctx->CreateBuffer(cbObjDesc, nullptr));

    // Samplers
    SamplerDesc linearSampDesc;
    linearSampDesc.filter = EFilter::MinMagMipLinear;
    linearSampDesc.addressU = ETextureAddressMode::Wrap;
    linearSampDesc.addressV = ETextureAddressMode::Wrap;
    linearSampDesc.addressW = ETextureAddressMode::Wrap;
    m_linearSampler.reset(ctx->CreateSampler(linearSampDesc));

    SamplerDesc shadowSampDesc;
    shadowSampDesc.filter = EFilter::ComparisonMinMagMipLinear;
    shadowSampDesc.addressU = ETextureAddressMode::Border;
    shadowSampDesc.addressV = ETextureAddressMode::Border;
    shadowSampDesc.addressW = ETextureAddressMode::Border;
    shadowSampDesc.borderColor[0] = 1.0f;
    shadowSampDesc.borderColor[1] = 1.0f;
    shadowSampDesc.borderColor[2] = 1.0f;
    shadowSampDesc.borderColor[3] = 1.0f;
    shadowSampDesc.comparisonFunc = EComparisonFunc::LessEqual;
    m_shadowSampler.reset(ctx->CreateSampler(shadowSampDesc));
}

void CTransparentForwardPass::Render(
    const CCamera& camera,
    CScene& scene,
    ITexture* hdrRT,
    ITexture* depthRT,
    uint32_t width,
    uint32_t height,
    const CShadowPass::Output* shadowData,
    CClusteredLightingPass* clusteredLighting)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList || !m_pso) return;

    // ============================================
    // Collect transparent objects
    // ============================================
    std::vector<TransparentItem> transparentItems;
    XMVECTOR eye = XMLoadFloat3(&camera.position);

    auto& probeManager = scene.GetProbeManager();
    CTextureManager& texMgr = CTextureManager::Instance();
    ITexture* defaultWhite = texMgr.GetDefaultWhite().get();
    ITexture* defaultNormal = texMgr.GetDefaultNormal().get();
    ITexture* defaultBlack = texMgr.GetDefaultBlack().get();

    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) continue;

        meshRenderer->EnsureUploaded();

        // Get material via MaterialManager
        CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
        if (!meshRenderer->materialPath.empty()) {
            material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
        }
        if (!material) continue;

        // Only collect transparent objects
        if (material->alphaMode != EAlphaMode::Blend) continue;

        XMMATRIX worldMatrix = transform->WorldMatrix();
        XMVECTOR objPos = worldMatrix.r[3];
        float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(objPos, eye)));

        // Get textures
        ITexture* albedoTex = material->albedoTexture.empty() ?
            defaultWhite : texMgr.Load(material->albedoTexture, true).get();
        ITexture* normalTex = material->normalMap.empty() ?
            defaultNormal : texMgr.Load(material->normalMap, false).get();
        ITexture* metallicRoughnessTex = material->metallicRoughnessMap.empty() ?
            defaultWhite : texMgr.Load(material->metallicRoughnessMap, false).get();
        ITexture* emissiveTex = material->emissiveMap.empty() ?
            defaultBlack : texMgr.Load(material->emissiveMap, true).get();

        bool hasRealMetallicRoughnessTexture = !material->metallicRoughnessMap.empty();
        bool hasRealEmissiveMap = !material->emissiveMap.empty();

        // Probe selection
        XMFLOAT3 worldPos;
        XMStoreFloat3(&worldPos, objPos);
        int probeIndex = probeManager.SelectProbeForPosition(worldPos);

        // Collect each mesh
        for (auto& gpuMesh : meshRenderer->meshes) {
            if (!gpuMesh) continue;

            TransparentItem item;
            item.obj = obj;
            item.meshRenderer = meshRenderer;
            item.transform = transform;
            item.material = material;
            item.worldMatrix = worldMatrix;
            item.distanceToCamera = distance;
            item.gpuMesh = gpuMesh.get();
            item.albedoTex = albedoTex ? albedoTex : defaultWhite;
            item.normalTex = normalTex ? normalTex : defaultNormal;
            item.metallicRoughnessTex = metallicRoughnessTex ? metallicRoughnessTex : defaultWhite;
            item.emissiveTex = emissiveTex ? emissiveTex : defaultBlack;
            item.hasRealMetallicRoughnessTexture = hasRealMetallicRoughnessTexture;
            item.hasRealEmissiveMap = hasRealEmissiveMap;
            item.probeIndex = probeIndex;
            item.lightmapIndex = meshRenderer->lightmapInfosIndex;

            transparentItems.push_back(item);
        }
    }

    // Skip if no transparent objects
    if (transparentItems.empty()) return;

    // Sort back-to-front for proper blending
    std::sort(transparentItems.begin(), transparentItems.end(),
        [](const TransparentItem& a, const TransparentItem& b) {
            return a.distanceToCamera > b.distanceToCamera;
        });

    CScopedDebugEvent evt(cmdList, L"Transparent Forward Pass");

    // ============================================
    // Set render target (HDR + depth read-only)
    // ============================================
    cmdList->SetRenderTargets(1, &hdrRT, depthRT);
    cmdList->SetViewport(0, 0, (float)width, (float)height);
    cmdList->SetScissorRect(0, 0, width, height);

    // ============================================
    // Set pipeline state
    // ============================================
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // ============================================
    // Update frame constants
    // ============================================
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = camera.GetProjectionMatrix();

    SDirectionalLight* dirLight = nullptr;
    for (auto& objPtr : scene.GetWorld().Objects()) {
        dirLight = objPtr->GetComponent<SDirectionalLight>();
        if (dirLight) break;
    }

    CB_Frame cf{};
    cf.view = XMMatrixTranspose(view);
    cf.proj = XMMatrixTranspose(proj);

    if (shadowData) {
        cf.cascadeCount = shadowData->cascadeCount;
        cf.debugShowCascades = shadowData->debugShowCascades ? 1 : 0;
        cf.enableSoftShadows = shadowData->enableSoftShadows ? 1 : 0;
        cf.cascadeBlendRange = shadowData->cascadeBlendRange;
        cf.cascadeSplits = XMFLOAT4(
            (0 < shadowData->cascadeCount) ? shadowData->cascadeSplits[0] : 100.0f,
            (1 < shadowData->cascadeCount) ? shadowData->cascadeSplits[1] : 100.0f,
            (2 < shadowData->cascadeCount) ? shadowData->cascadeSplits[2] : 100.0f,
            (3 < shadowData->cascadeCount) ? shadowData->cascadeSplits[3] : 100.0f
        );
        for (int i = 0; i < 4; ++i) {
            cf.lightSpaceVPs[i] = XMMatrixTranspose(shadowData->lightSpaceVPs[i]);
        }
    } else {
        cf.cascadeCount = 1;
        cf.enableSoftShadows = 1;
        for (int i = 0; i < 4; ++i) {
            cf.lightSpaceVPs[i] = XMMatrixTranspose(XMMatrixIdentity());
        }
    }

    if (dirLight) {
        cf.lightDirWS = dirLight->GetDirection();
        cf.lightColor = XMFLOAT3(
            dirLight->color.x * dirLight->intensity,
            dirLight->color.y * dirLight->intensity,
            dirLight->color.z * dirLight->intensity
        );
        cf.shadowBias = dirLight->shadow_bias;
        cf.iblIntensity = dirLight->ibl_intensity;
    } else {
        cf.lightDirWS = XMFLOAT3(0.4f, -1.0f, 0.2f);
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&cf.lightDirWS));
        XMStoreFloat3(&cf.lightDirWS, L);
        cf.lightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        cf.shadowBias = 0.005f;
        cf.iblIntensity = 1.0f;
    }

    cf.camPosWS = camera.position;
    cf.diffuseGIMode = static_cast<int>(scene.GetLightSettings().diffuseGIMode);

    cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &cf, sizeof(cf));
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cf, sizeof(cf));

    // ============================================
    // Bind shadow maps (t4)
    // ============================================
    if (shadowData && shadowData->shadowMapArray) {
        cmdList->SetShaderResource(EShaderStage::Pixel, 4, shadowData->shadowMapArray);
        if (shadowData->shadowSampler) {
            cmdList->SetSampler(EShaderStage::Pixel, 1, shadowData->shadowSampler);
        } else {
            cmdList->SetSampler(EShaderStage::Pixel, 1, m_shadowSampler.get());
        }
    }

    // ============================================
    // Bind IBL textures (t5-t7)
    // ============================================
    probeManager.Bind(cmdList);

    // ============================================
    // Bind Clustered Lighting data (t8-t10, b3)
    // ============================================
    if (clusteredLighting) {
        clusteredLighting->BindToMainPass(cmdList);
    }

    // ============================================
    // Bind Volumetric Lightmap
    // ============================================
    scene.GetVolumetricLightmap().Bind(cmdList);

    // ============================================
    // Bind sampler
    // ============================================
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_linearSampler.get());

    // ============================================
    // Render each transparent item
    // ============================================
    for (const auto& item : transparentItems) {
        // Update per-object constants
        CB_Object co{};
        co.world = XMMatrixTranspose(item.worldMatrix);
        co.albedo = item.material->albedo;
        co.metallic = item.material->metallic;
        co.roughness = item.material->roughness;
        co.hasMetallicRoughnessTexture = item.hasRealMetallicRoughnessTexture ? 1 : 0;
        co.emissive = item.material->emissive;
        co.emissiveStrength = item.material->emissiveStrength;
        co.hasEmissiveMap = item.hasRealEmissiveMap ? 1 : 0;
        co.alphaMode = static_cast<int>(item.material->alphaMode);
        co.alphaCutoff = item.material->alphaCutoff;
        co.probeIndex = item.probeIndex;
        co.lightmapIndex = item.lightmapIndex;

        cmdList->SetConstantBufferData(EShaderStage::Vertex, 1, &co, sizeof(co));
        cmdList->SetConstantBufferData(EShaderStage::Pixel, 1, &co, sizeof(co));

        // Bind textures
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, item.albedoTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, item.normalTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 2, item.metallicRoughnessTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 3, item.emissiveTex);

        // Bind vertex/index buffers and draw
        cmdList->SetVertexBuffer(0, item.gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
        cmdList->SetIndexBuffer(item.gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
        cmdList->DrawIndexed(item.gpuMesh->indexCount, 0, 0);
    }
}
