#include "DepthPrePass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "Core/GpuMeshResource.h"
#include "Core/Mesh.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Core/MaterialManager.h"

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

    CFFLog::Info("DepthPrePass initialized");
    return true;
}

void CDepthPrePass::Shutdown()
{
    m_pso.reset();
    m_depthVS.reset();
    m_cbFrame.reset();
    m_cbObject.reset();
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

    // Set pipeline state
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Update frame constants (ViewProj matrix)
    XMMATRIX view = camera.GetViewMatrix();
    // Use jittered projection for TAA (returns normal projection if TAA disabled)
    XMMATRIX proj = camera.GetJitteredProjectionMatrix(width, height);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    CB_DepthFrame frameData;
    frameData.viewProj = XMMatrixTranspose(viewProj);
    cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &frameData, sizeof(frameData));

    // Render all opaque objects
    for (auto& objPtr : scene.GetWorld().Objects()) {
        auto* obj = objPtr.get();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
        auto* transform = obj->GetComponent<STransform>();

        if (!meshRenderer || !transform) continue;

        // Skip transparent objects (they don't write to depth pre-pass)
        // Check material alpha mode
        meshRenderer->EnsureUploaded();
        if (meshRenderer->meshes.empty()) continue;

        // Get material for alpha mode check
        CMaterialAsset* material = CMaterialManager::Instance().GetDefault();
        if (!meshRenderer->materialPath.empty()) {
            material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
        }

        // Skip transparent and alpha-tested objects
        // - Blend: Cannot write depth (blending requires sorted rendering)
        // - Mask: Cannot test alpha without pixel shader (would cause black holes)
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
