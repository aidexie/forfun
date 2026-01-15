#include "GBufferPass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Core/PathManager.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Core/MaterialManager.h"
#include "Core/TextureManager.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

// ============================================
// Constant Buffer Structures
// ============================================
namespace {

struct alignas(16) CB_GBufferFrame {
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX viewProjPrev;
    XMFLOAT3 camPosWS;
    float _pad0;
};

struct alignas(16) CB_GBufferObject {
    XMMATRIX world;
    XMMATRIX worldPrev;
    XMFLOAT3 albedo; float metallic;
    XMFLOAT3 emissive; float roughness;
    float emissiveStrength;
    int hasMetallicRoughnessTexture;
    int hasEmissiveMap;
    int alphaMode;
    float alphaCutoff;
    int lightmapIndex;
    float materialID;
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

bool CGBufferPass::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Shader directory
    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

    // Compile vertex shader
    std::string vsSource = LoadShaderSource(shaderDir + "GBuffer.vs.hlsl");
    if (vsSource.empty()) {
        CFFLog::Error("GBufferPass: Failed to load vertex shader");
        return false;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("GBufferPass VS compilation error: %s", vsCompiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "GBuffer_VS";
    m_vs.reset(ctx->CreateShader(vsDesc));

    // Compile pixel shader with include path for Lightmap2D.hlsl
    std::string psSource = LoadShaderSource(shaderDir + "GBuffer.ps.hlsl");
    if (psSource.empty()) {
        CFFLog::Error("GBufferPass: Failed to load pixel shader");
        return false;
    }

    // Use include handler for Lightmap2D.hlsl
    CDefaultShaderIncludeHandler includeHandler(shaderDir);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_0", &includeHandler, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("GBufferPass PS compilation error: %s", psCompiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    psDesc.debugName = "GBuffer_PS";
    m_ps.reset(ctx->CreateShader(psDesc));

    // Create pipeline state
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();

    // Input layout (matches SVertexPNT)
    psoDesc.inputLayout = {
        { EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 },
        { EVertexSemantic::Normal,   0, EVertexFormat::Float3, 12, 0 },
        { EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 24, 0 },
        { EVertexSemantic::Tangent,  0, EVertexFormat::Float4, 32, 0 },
        { EVertexSemantic::Color,    0, EVertexFormat::Float4, 48, 0 },
        { EVertexSemantic::Texcoord, 1, EVertexFormat::Float2, 64, 0 }
    };

    // Rasterizer state
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.cullMode = ECullMode::Back;
    psoDesc.rasterizer.depthClipEnable = true;

    // Depth bias: compensate for FP precision difference between passes
    // DepthPrePass uses: posWS * ViewProj (single matrix multiply)
    // GBuffer uses: (posWS * View) * Proj (two matrix multiplies)
    // Matrix multiplication is NOT associative in FP: (A*B)*C ≠ A*(B*C)
    // The extra multiply causes GBuffer depth to be slightly different than pre-pass
    // Depth bias pushes GBuffer depth to match pre-pass value
    // For reversed-Z, we need positive bias (opposite direction)
    int depthBias = UseReversedZ() ? 1 : -1;
    float slopeScaledBias = UseReversedZ() ? 1.0f : -1.0f;
    psoDesc.rasterizer.depthBias = depthBias;
    psoDesc.rasterizer.slopeScaledDepthBias = slopeScaledBias;

    // Depth stencil state: test (with bias, effectively matches pre-pass depth)
    // Write OFF since depth was already written by DepthPrePass
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual

    // No blending
    psoDesc.blend.blendEnable = false;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    // 5 render targets (G-Buffer layout)
    psoDesc.renderTargetFormats = {
        ETextureFormat::R16G16B16A16_FLOAT,   // RT0: WorldPosMetallic
        ETextureFormat::R16G16B16A16_FLOAT,   // RT1: NormalRoughness
        ETextureFormat::R8G8B8A8_UNORM_SRGB,  // RT2: AlbedoAO
        ETextureFormat::R16G16B16A16_FLOAT,   // RT3: EmissiveMaterialID
        ETextureFormat::R16G16_FLOAT          // RT4: Velocity
    };
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;

    psoDesc.debugName = "GBufferPass_PSO";
    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    if (!m_pso) {
        CFFLog::Error("Failed to create GBufferPass PSO");
        return false;
    }

    // Create sampler
    SamplerDesc sampDesc;
    sampDesc.filter = EFilter::MinMagMipLinear;
    sampDesc.addressU = ETextureAddressMode::Wrap;
    sampDesc.addressV = ETextureAddressMode::Wrap;
    sampDesc.addressW = ETextureAddressMode::Wrap;
    m_sampler.reset(ctx->CreateSampler(sampDesc));

    CFFLog::Info("GBufferPass initialized");
    return true;
}

void CGBufferPass::Shutdown()
{
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_sampler.reset();
}

void CGBufferPass::Render(
    const CCamera& camera,
    CScene& scene,
    CGBuffer& gbuffer,
    const DirectX::XMMATRIX& viewProjPrev,
    uint32_t width,
    uint32_t height)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    RHI::CScopedDebugEvent evt(cmdList, L"G-Buffer Pass");

    // Get G-Buffer render targets
    RHI::ITexture* rts[CGBuffer::RT_Count];
    uint32_t rtCount;
    gbuffer.GetRenderTargets(rts, rtCount);

    // Set render targets
    cmdList->SetRenderTargets(rtCount, rts, gbuffer.GetDepthBuffer());

    // Set viewport and scissor
    cmdList->SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    // Clear G-Buffer render targets (not depth - already populated)
    const float clearBlack[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0; i < rtCount; ++i) {
        cmdList->ClearRenderTarget(rts[i], clearBlack);
    }

    // Set pipeline state
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Bind sampler
    cmdList->SetSampler(EShaderStage::Pixel, 0, m_sampler.get());

    // Bind 2D Lightmap resources
    scene.GetLightmap2D().Bind(cmdList);

    // Update frame constants
    XMMATRIX view = camera.GetViewMatrix();
    // Use jittered projection for TAA (returns normal projection if TAA disabled)
    XMMATRIX proj = camera.GetJitteredProjectionMatrix(width, height);

    CB_GBufferFrame frameData;
    frameData.view = XMMatrixTranspose(view);
    frameData.proj = XMMatrixTranspose(proj);
    frameData.viewProjPrev = XMMatrixTranspose(viewProjPrev);
    frameData.camPosWS = camera.position;
    cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &frameData, sizeof(frameData));
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &frameData, sizeof(frameData));

    // Render all opaque objects
    CTextureManager& texMgr = CTextureManager::Instance();

    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) continue;

        meshRenderer->EnsureUploaded();
        if (meshRenderer->meshes.empty()) continue;

        // Get material
        CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
        if (!meshRenderer->materialPath.empty()) {
            material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
        }

        // Skip transparent objects
        if (material && material->alphaMode == EAlphaMode::Blend) {
            continue;
        }

        // Get textures (async loading - returns placeholder until ready)
        RHI::ITexture* albedoTex = material->albedoTexture.empty() ?
            texMgr.GetDefaultWhite().get() : texMgr.LoadAsync(material->albedoTexture, true)->GetTexture();
        RHI::ITexture* normalTex = material->normalMap.empty() ?
            texMgr.GetDefaultNormal().get() : texMgr.LoadAsync(material->normalMap, false)->GetTexture();
        RHI::ITexture* metallicRoughnessTex = material->metallicRoughnessMap.empty() ?
            texMgr.GetDefaultWhite().get() : texMgr.LoadAsync(material->metallicRoughnessMap, false)->GetTexture();
        RHI::ITexture* emissiveTex = material->emissiveMap.empty() ?
            texMgr.GetDefaultBlack().get() : texMgr.LoadAsync(material->emissiveMap, true)->GetTexture();

        bool hasRealMetallicRoughnessTexture = !material->metallicRoughnessMap.empty();
        bool hasRealEmissiveMap = !material->emissiveMap.empty();

        // Update object constants
        XMMATRIX worldMatrix = transform->WorldMatrix();

        CB_GBufferObject objData;
        objData.world = XMMatrixTranspose(worldMatrix);
        objData.worldPrev = XMMatrixTranspose(worldMatrix);  // TODO: Track previous frame's world matrix
        objData.albedo = material->albedo;
        objData.metallic = material->metallic;
        objData.emissive = material->emissive;
        objData.roughness = material->roughness;
        objData.emissiveStrength = material->emissiveStrength;
        objData.hasMetallicRoughnessTexture = hasRealMetallicRoughnessTexture ? 1 : 0;
        objData.hasEmissiveMap = hasRealEmissiveMap ? 1 : 0;
        objData.alphaMode = static_cast<int>(material->alphaMode);
        objData.alphaCutoff = material->alphaCutoff;
        objData.lightmapIndex = meshRenderer->lightmapInfosIndex;
        objData.materialID = static_cast<float>(material->materialType);  // EMaterialType → MaterialID

        cmdList->SetConstantBufferData(EShaderStage::Vertex, 1, &objData, sizeof(objData));
        cmdList->SetConstantBufferData(EShaderStage::Pixel, 1, &objData, sizeof(objData));

        // Bind textures
        cmdList->SetShaderResource(EShaderStage::Pixel, 0, albedoTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 1, normalTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 2, metallicRoughnessTex);
        cmdList->SetShaderResource(EShaderStage::Pixel, 3, emissiveTex);

        // Draw all meshes
        for (auto& gpuMesh : meshRenderer->meshes) {
            if (!gpuMesh) continue;

            cmdList->SetVertexBuffer(0, gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
            cmdList->SetIndexBuffer(gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
            cmdList->DrawIndexed(gpuMesh->indexCount, 0, 0);
        }
    }
}
