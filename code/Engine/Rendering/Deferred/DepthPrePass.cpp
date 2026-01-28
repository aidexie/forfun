#include "DepthPrePass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerDrawSlots.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Core/PathManager.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Core/MaterialManager.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

// ============================================
// Constant Buffer Structures
// ============================================
namespace {

struct alignas(16) CB_DepthFrame {
    XMMATRIX viewProj;
};

struct alignas(16) CB_DepthObject {
    XMMATRIX world;
};

// CB_DepthPrePass for descriptor set path (Set 1, space1)
struct alignas(16) CB_DepthPrePass {
    XMMATRIX viewProj;
};

namespace {
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

// Depth-only vertex shader source
static const char* kDepthPrePassVS = R"(
    cbuffer CB_Frame : register(b0) {
        float4x4 gViewProj;
    }
    cbuffer CB_Object : register(b1) {
        float4x4 gWorld;
    }

    struct VSIn {
        float3 pos : POSITION;
        float3 normal : NORMAL;
        float2 uv : TEXCOORD0;
        float4 tangent : TANGENT;
        float4 color : COLOR;
        float2 uv2 : TEXCOORD1;
    };

    float4 main(VSIn i) : SV_Position {
        float4 posWS = mul(float4(i.pos, 1.0), gWorld);
        return mul(posWS, gViewProj);
    }
)";

} // anonymous namespace

bool CDepthPrePass::Initialize()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile depth-only vertex shader
    SCompiledShader vsCompiled = CompileShaderFromSource(kDepthPrePassVS, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("DepthPrePass VS compilation error: %s", vsCompiled.errorMessage.c_str());
        return false;
    }

    // Create vertex shader
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "DepthPrePass_VS";
    m_depthVS.reset(ctx->CreateShader(vsDesc));

    // Create pipeline state (depth-only, no pixel shader)
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_depthVS.get();
    psoDesc.pixelShader = nullptr;  // No pixel shader - depth only

    // Input layout (must match SVertexPNT)
    // Position: offset 0, float3
    // Normal: offset 12, float3
    // UV: offset 24, float2
    // Tangent: offset 32, float4
    // Color: offset 48, float4
    // UV2: offset 64, float2
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

    // Depth stencil state: test, write ON
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = true;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(false);  // Less or Greater

    // No blending (no color output)
    psoDesc.blend.blendEnable = false;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    // Depth-only pass: no render targets
    psoDesc.renderTargetFormats = {};
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;

    psoDesc.debugName = "DepthPrePass_PSO";
    m_pso.reset(ctx->CreatePipelineState(psoDesc));

    if (!m_pso) {
        CFFLog::Error("Failed to create DepthPrePass PSO");
        return false;
    }

    // Create constant buffers
    BufferDesc cbFrameDesc;
    cbFrameDesc.size = sizeof(CB_DepthFrame);
    cbFrameDesc.usage = EBufferUsage::Constant;
    cbFrameDesc.cpuAccess = ECPUAccess::Write;
    cbFrameDesc.debugName = "DepthPrePass_CB_Frame";
    m_cbFrame.reset(ctx->CreateBuffer(cbFrameDesc, nullptr));

    BufferDesc cbObjDesc;
    cbObjDesc.size = sizeof(CB_DepthObject);
    cbObjDesc.usage = EBufferUsage::Constant;
    cbObjDesc.cpuAccess = ECPUAccess::Write;
    cbObjDesc.debugName = "DepthPrePass_CB_Object";
    m_cbObject.reset(ctx->CreateBuffer(cbObjDesc, nullptr));

    // Initialize descriptor set resources (DX12 only)
    initDescriptorSets();

    CFFLog::Info("DepthPrePass initialized");
    return true;
}

void CDepthPrePass::Shutdown()
{
    m_pso.reset();
    m_depthVS.reset();
    m_cbFrame.reset();
    m_cbObject.reset();

    // Cleanup descriptor set resources
    m_pso_ds.reset();
    m_depthVS_ds.reset();

    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perPassSet) {
            ctx->FreeDescriptorSet(m_perPassSet);
            m_perPassSet = nullptr;
        }
        if (m_perDrawSet) {
            ctx->FreeDescriptorSet(m_perDrawSet);
            m_perDrawSet = nullptr;
        }
        if (m_perPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_perPassLayout);
            m_perPassLayout = nullptr;
        }
        if (m_perDrawLayout) {
            ctx->DestroyDescriptorSetLayout(m_perDrawLayout);
            m_perDrawLayout = nullptr;
        }
    }
}

void CDepthPrePass::Render(
    const CCamera& camera,
    CScene& scene,
    RHI::ITexture* depthTarget,
    uint32_t width,
    uint32_t height)
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    ICommandList* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    RHI::CScopedDebugEvent evt(cmdList, L"Depth Pre-Pass");

    // Set render target (depth only, no color)
    cmdList->SetRenderTargets(0, nullptr, depthTarget);

    // Set viewport and scissor
    cmdList->SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    cmdList->SetScissorRect(0, 0, width, height);

    // Clear depth buffer
    float clearDepth = UseReversedZ() ? 0.0f : 1.0f;
    cmdList->ClearDepthStencil(depthTarget, true, clearDepth, false, 0);

    // Choose DS or legacy path
    const bool useDescriptorSets = IsDescriptorSetModeAvailable();

    // Set pipeline state
    cmdList->SetPipelineState(useDescriptorSets ? m_pso_ds.get() : m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Update frame constants (ViewProj matrix)
    XMMATRIX view = camera.GetViewMatrix();
    // Use jittered projection for TAA (returns normal projection if TAA disabled)
    XMMATRIX proj = camera.GetJitteredProjectionMatrix(width, height);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    if (useDescriptorSets) {
        // === Descriptor Set Path ===
        // Bind PerPass set (Set 1) with viewProj matrix
        CB_DepthPrePass passCB;
        passCB.viewProj = XMMatrixTranspose(viewProj);

        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &passCB, sizeof(passCB)));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        // Render all opaque objects
        for (auto& objPtr : scene.GetWorld().Objects()) {
            auto* obj = objPtr.get();
            auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
            auto* transform = obj->GetComponent<STransform>();

            if (!meshRenderer || !transform) continue;

            meshRenderer->EnsureUploaded();
            if (meshRenderer->meshes.empty()) continue;

            // Get material for alpha mode check
            CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
            if (!meshRenderer->materialPath.empty()) {
                material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
            }

            // Skip transparent and alpha-tested objects
            if (material && (material->alphaMode == EAlphaMode::Blend ||
                             material->alphaMode == EAlphaMode::Mask)) {
                continue;
            }

            XMMATRIX worldMatrix = transform->WorldMatrix();

            // Bind PerDraw set (Set 3) with world matrix
            PerDrawSlots::CB_PerDraw perDraw;
            XMStoreFloat4x4(&perDraw.World, XMMatrixTranspose(worldMatrix));
            XMStoreFloat4x4(&perDraw.WorldPrev, XMMatrixTranspose(worldMatrix));
            perDraw.lightmapIndex = -1;  // Not used in depth pre-pass
            perDraw.objectID = 0;

            m_perDrawSet->Bind(BindingSetItem::VolatileCBV(0, &perDraw, sizeof(perDraw)));
            cmdList->BindDescriptorSet(3, m_perDrawSet);

            // Draw all meshes
            for (auto& gpuMesh : meshRenderer->meshes) {
                if (!gpuMesh) continue;

                cmdList->SetVertexBuffer(0, gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
                cmdList->SetIndexBuffer(gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
                cmdList->DrawIndexed(gpuMesh->indexCount, 0, 0);
            }
        }
    } else {
        // === Legacy Path ===
        CB_DepthFrame frameData;
        frameData.viewProj = XMMatrixTranspose(viewProj);
        cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &frameData, sizeof(frameData));

        // Render all opaque objects
        for (auto& objPtr : scene.GetWorld().Objects()) {
            auto* obj = objPtr.get();
            auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
            auto* transform = obj->GetComponent<STransform>();

            if (!meshRenderer || !transform) continue;

            meshRenderer->EnsureUploaded();
            if (meshRenderer->meshes.empty()) continue;

            // Get material for alpha mode check
            CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
            if (!meshRenderer->materialPath.empty()) {
                material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
            }

            // Skip transparent and alpha-tested objects
            if (material && (material->alphaMode == EAlphaMode::Blend ||
                             material->alphaMode == EAlphaMode::Mask)) {
                continue;
            }

            // Update object constants (World matrix)
            XMMATRIX worldMatrix = transform->WorldMatrix();
            CB_DepthObject objData;
            objData.world = XMMatrixTranspose(worldMatrix);
            cmdList->SetConstantBufferData(EShaderStage::Vertex, 1, &objData, sizeof(objData));

            // Draw all meshes
            for (auto& gpuMesh : meshRenderer->meshes) {
                if (!gpuMesh) continue;

                cmdList->SetVertexBuffer(0, gpuMesh->vbo.get(), sizeof(SVertexPNT), 0);
                cmdList->SetIndexBuffer(gpuMesh->ibo.get(), EIndexFormat::UInt32, 0);
                cmdList->DrawIndexed(gpuMesh->indexCount, 0, 0);
            }
        }
    }
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CDepthPrePass::initDescriptorSets()
{
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[DepthPrePass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile SM 5.1 vertex shader
    std::string vsSource = LoadShaderSource(shaderDir + "DepthPrePass_DS.vs.hlsl");
    if (vsSource.empty()) {
        CFFLog::Warning("[DepthPrePass] Failed to load DepthPrePass_DS.vs.hlsl");
        return;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("[DepthPrePass] DepthPrePass_DS.vs.hlsl compile error: %s", vsCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    vsDesc.debugName = "DepthPrePass_DS_VS";
    m_depthVS_ds.reset(ctx->CreateShader(vsDesc));

    if (!m_depthVS_ds) {
        CFFLog::Error("[DepthPrePass] Failed to create SM 5.1 shader");
        return;
    }

    // Create PerPass layout (Set 1, space1)
    // CB_DepthPrePass (b0)
    BindingLayoutDesc perPassLayoutDesc("DepthPrePass_PerPass");
    perPassLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_DepthPrePass)));

    m_perPassLayout = ctx->CreateDescriptorSetLayout(perPassLayoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[DepthPrePass] Failed to create PerPass layout");
        return;
    }

    // Create PerDraw layout (Set 3, space3)
    // CB_PerDraw (b0)
    BindingLayoutDesc perDrawLayoutDesc("DepthPrePass_PerDraw");
    perDrawLayoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(PerDrawSlots::CB_PerDraw)));

    m_perDrawLayout = ctx->CreateDescriptorSetLayout(perDrawLayoutDesc);
    if (!m_perDrawLayout) {
        CFFLog::Error("[DepthPrePass] Failed to create PerDraw layout");
        return;
    }

    // Allocate descriptor sets
    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    m_perDrawSet = ctx->AllocateDescriptorSet(m_perDrawLayout);

    if (!m_perPassSet || !m_perDrawSet) {
        CFFLog::Error("[DepthPrePass] Failed to allocate descriptor sets");
        return;
    }

    CFFLog::Info("[DepthPrePass] Descriptor set resources initialized");
}

void CDepthPrePass::CreatePSOWithLayouts(IDescriptorSetLayout* perFrameLayout)
{
    if (!m_perPassLayout || !m_depthVS_ds) {
        CFFLog::Warning("[DepthPrePass] Cannot create PSO with layouts - missing resources");
        return;
    }

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_depthVS_ds.get();
    psoDesc.pixelShader = nullptr;  // Depth-only, no pixel shader

    // Input layout (same as legacy PSO)
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

    // Depth stencil state
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = true;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(false);  // Less or Greater

    // No blending
    psoDesc.blend.blendEnable = false;

    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;

    // Depth-only pass: no render targets
    psoDesc.renderTargetFormats = {};
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;

    // Set descriptor set layouts
    // DepthPrePass doesn't use Set 0 (PerFrame) or Set 2 (PerMaterial)
    psoDesc.setLayouts[0] = nullptr;             // Set 0: Not used
    psoDesc.setLayouts[1] = m_perPassLayout;     // Set 1: PerPass (space1)
    psoDesc.setLayouts[2] = nullptr;             // Set 2: Not used
    psoDesc.setLayouts[3] = m_perDrawLayout;     // Set 3: PerDraw (space3)

    psoDesc.debugName = "DepthPrePass_DS_PSO";
    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));

    if (m_pso_ds) {
        CFFLog::Info("[DepthPrePass] PSO with descriptor set layouts created");
    } else {
        CFFLog::Error("[DepthPrePass] Failed to create PSO with descriptor set layouts");
    }
}
