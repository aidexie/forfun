// Engine/Rendering/DebugLinePass.cpp
#include "DebugLinePass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/RenderConfig.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace RHI;

// Helper function to load shader source from file
static std::string LoadShaderSource(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        CFFLog::Error("Failed to open shader file: %s", filepath.c_str());
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void CDebugLinePass::Initialize() {
    if (m_initialized) return;

    CreateShaders();
    CreateBuffers();
    CreatePipelineState();
    initDescriptorSets();

    m_initialized = true;
}

void CDebugLinePass::Shutdown() {
    // Cleanup descriptor set resources
    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perPassSet) {
            ctx->FreeDescriptorSet(m_perPassSet);
            m_perPassSet = nullptr;
        }
        if (m_perPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_perPassLayout);
            m_perPassLayout = nullptr;
        }
    }

    m_pso_ds.reset();
    m_vs_ds.reset();
    m_gs_ds.reset();
    m_ps_ds.reset();

    m_pso.reset();
    m_vertexBuffer.reset();
    m_cbPerFrameVS.reset();
    m_cbPerFrameGS.reset();
    m_vs.reset();
    m_gs.reset();
    m_ps.reset();
    m_dynamicLines.clear();
    m_initialized = false;
}

void CDebugLinePass::CreateShaders() {
    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) {
        CFFLog::Error("RHIManager not initialized!");
        return;
    }

    // Load shader source files
    std::string vsSource = LoadShaderSource("../source/code/Shader/DebugLine.vs.hlsl");
    std::string gsSource = LoadShaderSource("../source/code/Shader/DebugLine.gs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/DebugLine.ps.hlsl");

    if (vsSource.empty() || gsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load DebugLine shader files!");
        return;
    }

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile Vertex Shader
    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource, "main", "vs_5_0", nullptr, debugShaders);
    if (!vsCompiled.success) {
        CFFLog::Error("=== DEBUGLINE VERTEX SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", vsCompiled.errorMessage.c_str());
        return;
    }

    // Compile Geometry Shader
    SCompiledShader gsCompiled = CompileShaderFromSource(gsSource, "main", "gs_5_0", nullptr, debugShaders);
    if (!gsCompiled.success) {
        CFFLog::Error("=== DEBUGLINE GEOMETRY SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", gsCompiled.errorMessage.c_str());
        return;
    }

    // Compile Pixel Shader
    SCompiledShader psCompiled = CompileShaderFromSource(psSource, "main", "ps_5_0", nullptr, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("=== DEBUGLINE PIXEL SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", psCompiled.errorMessage.c_str());
        return;
    }

    // Create shader objects using RHI
    ShaderDesc vsDesc(EShaderType::Vertex, vsCompiled.bytecode.data(), vsCompiled.bytecode.size());
    m_vs.reset(renderContext->CreateShader(vsDesc));

    ShaderDesc gsDesc(EShaderType::Geometry, gsCompiled.bytecode.data(), gsCompiled.bytecode.size());
    m_gs.reset(renderContext->CreateShader(gsDesc));

    ShaderDesc psDesc(EShaderType::Pixel, psCompiled.bytecode.data(), psCompiled.bytecode.size());
    m_ps.reset(renderContext->CreateShader(psDesc));
}

void CDebugLinePass::CreateBuffers() {
    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    // Create dynamic vertex buffer
    BufferDesc vbDesc;
    vbDesc.size = sizeof(LineVertex) * m_maxVertices;
    vbDesc.usage = EBufferUsage::Vertex;
    vbDesc.cpuAccess = ECPUAccess::Write;
    m_vertexBuffer.reset(renderContext->CreateBuffer(vbDesc, nullptr));

    // Create constant buffers
    BufferDesc cbDescVS;
    cbDescVS.size = sizeof(CBPerFrameVS);
    cbDescVS.usage = EBufferUsage::Constant;
    cbDescVS.cpuAccess = ECPUAccess::Write;
    m_cbPerFrameVS.reset(renderContext->CreateBuffer(cbDescVS, nullptr));

    BufferDesc cbDescGS;
    cbDescGS.size = sizeof(CBPerFrameGS);
    cbDescGS.usage = EBufferUsage::Constant;
    cbDescGS.cpuAccess = ECPUAccess::Write;
    m_cbPerFrameGS.reset(renderContext->CreateBuffer(cbDescGS, nullptr));
}

void CDebugLinePass::CreatePipelineState() {
    if (!m_vs || !m_gs || !m_ps) return;

    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.geometryShader = m_gs.get();
    psoDesc.pixelShader = m_ps.get();

    // Input layout
    psoDesc.inputLayout = {
        VertexElement(EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0, false),
        VertexElement(EVertexSemantic::Color, 0, EVertexFormat::Float4, 12, 0, false)
    };

    // Rasterizer state
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;

    // Depth stencil state: test but no write
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;  // Match GBuffer depth

    // Blend state: alpha blending
    psoDesc.blend.blendEnable = true;
    psoDesc.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoDesc.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoDesc.blend.blendOp = EBlendOp::Add;
    psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
    psoDesc.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoDesc.blend.blendOpAlpha = EBlendOp::Add;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::LineList;

    // Render target format: LDR uses R8G8B8A8_UNORM_SRGB
    psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    psoDesc.debugName = "DebugLine_PSO";

    m_pso.reset(renderContext->CreatePipelineState(psoDesc));
}

void CDebugLinePass::BeginFrame() {
    m_dynamicLines.clear();
}

void CDebugLinePass::AddLine(XMFLOAT3 from, XMFLOAT3 to, XMFLOAT4 color) {
    if (m_dynamicLines.size() + 2 > m_maxVertices) {
        CFFLog::Warning("DebugLinePass vertex buffer overflow!");
        return;
    }
    m_dynamicLines.push_back({ from, color });
    m_dynamicLines.push_back({ to, color });
}

void CDebugLinePass::AddAABB(XMFLOAT3 localMin, XMFLOAT3 localMax,
                             XMMATRIX worldMatrix, XMFLOAT4 color) {
    XMFLOAT3 corners[8] = {
        XMFLOAT3(localMin.x, localMin.y, localMin.z),
        XMFLOAT3(localMax.x, localMin.y, localMin.z),
        XMFLOAT3(localMax.x, localMax.y, localMin.z),
        XMFLOAT3(localMin.x, localMax.y, localMin.z),
        XMFLOAT3(localMin.x, localMin.y, localMax.z),
        XMFLOAT3(localMax.x, localMin.y, localMax.z),
        XMFLOAT3(localMax.x, localMax.y, localMax.z),
        XMFLOAT3(localMin.x, localMax.y, localMax.z),
    };

    XMFLOAT3 worldCorners[8];
    for (int i = 0; i < 8; i++) {
        XMVECTOR localCorner = XMLoadFloat3(&corners[i]);
        XMVECTOR worldCorner = XMVector3TransformCoord(localCorner, worldMatrix);
        XMStoreFloat3(&worldCorners[i], worldCorner);
    }

    int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    for (int i = 0; i < 12; i++) {
        AddLine(worldCorners[edges[i][0]], worldCorners[edges[i][1]], color);
    }
}

void CDebugLinePass::UpdateVertexBuffer() {
    if (m_dynamicLines.empty() || !m_vertexBuffer) return;

    void* mappedData = m_vertexBuffer->Map();
    if (mappedData) {
        size_t dataSize = sizeof(LineVertex) * m_dynamicLines.size();
        memcpy(mappedData, m_dynamicLines.data(), dataSize);
        m_vertexBuffer->Unmap();
    }
}

void CDebugLinePass::Render(XMMATRIX view, XMMATRIX proj,
                            unsigned int viewportWidth, unsigned int viewportHeight) {
    if (!m_initialized || m_dynamicLines.empty()) return;

    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    UpdateVertexBuffer();

    // Update constant buffer data
    XMMATRIX viewProj = view * proj;
    CBPerFrameVS cbVS;
    cbVS.viewProj = XMMatrixTranspose(viewProj);

    CBPerFrameGS cbGS;
    cbGS.viewportSize = XMFLOAT2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
    cbGS.lineThickness = m_lineThickness;
    cbGS.padding = 0.0f;

    // Get command list
    ICommandList* cmdList = renderContext->GetCommandList();

    unsigned int stride = sizeof(LineVertex);
    unsigned int offset = 0;
    unsigned int vertexCount = static_cast<unsigned int>(m_dynamicLines.size());

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        cmdList->SetPipelineState(m_pso_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::LineList);
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), stride, offset);

        // Bind descriptor set with volatile CBVs
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cbVS, sizeof(cbVS)));
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(1, &cbGS, sizeof(cbGS)));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        cmdList->Draw(vertexCount, 0);
        return;
    }

    CFFLog::Warning("DebugLinePass::Render() - Legacy binding disabled, descriptor set path not available");
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CDebugLinePass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[DebugLinePass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile SM 5.1 shaders
    std::string vsSource = LoadShaderSource(shaderDir + "DebugLine_DS.vs.hlsl");
    std::string gsSource = LoadShaderSource(shaderDir + "DebugLine_DS.gs.hlsl");
    std::string psSource = LoadShaderSource(shaderDir + "DebugLine_DS.ps.hlsl");
    if (vsSource.empty() || gsSource.empty() || psSource.empty()) {
        CFFLog::Warning("[DebugLinePass] Failed to load DS shaders");
        return;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
    SCompiledShader gsCompiled = CompileShaderFromSource(gsSource.c_str(), "main", "gs_5_1", nullptr, debugShaders);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_1", nullptr, debugShaders);
    if (!vsCompiled.success || !gsCompiled.success || !psCompiled.success) {
        CFFLog::Error("[DebugLinePass] DS shader compile error: %s %s %s",
                      vsCompiled.errorMessage.c_str(), gsCompiled.errorMessage.c_str(), psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc(EShaderType::Vertex, vsCompiled.bytecode.data(), vsCompiled.bytecode.size());
    vsDesc.debugName = "DebugLine_DS_VS";
    ShaderDesc gsDesc(EShaderType::Geometry, gsCompiled.bytecode.data(), gsCompiled.bytecode.size());
    gsDesc.debugName = "DebugLine_DS_GS";
    ShaderDesc psDesc(EShaderType::Pixel, psCompiled.bytecode.data(), psCompiled.bytecode.size());
    psDesc.debugName = "DebugLine_DS_PS";
    m_vs_ds.reset(ctx->CreateShader(vsDesc));
    m_gs_ds.reset(ctx->CreateShader(gsDesc));
    m_ps_ds.reset(ctx->CreateShader(psDesc));

    if (!m_vs_ds || !m_gs_ds || !m_ps_ds) {
        CFFLog::Error("[DebugLinePass] Failed to create DS shaders");
        return;
    }

    // Create PerPass layout (Set 1): Two VolatileCBVs (b0 for VS, b1 for GS)
    BindingLayoutDesc layoutDesc("DebugLine_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CBPerFrameVS)));  // VS constant buffer
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(1, sizeof(CBPerFrameGS)));  // GS constant buffer

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[DebugLinePass] Failed to create descriptor set layout");
        return;
    }

    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[DebugLinePass] Failed to allocate descriptor set");
        return;
    }

    // Create PSO with descriptor set layout
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs_ds.get();
    psoDesc.geometryShader = m_gs_ds.get();
    psoDesc.pixelShader = m_ps_ds.get();

    // Input layout
    psoDesc.inputLayout = {
        VertexElement(EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0, false),
        VertexElement(EVertexSemantic::Color, 0, EVertexFormat::Float4, 12, 0, false)
    };

    // Rasterizer state
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;

    // Depth stencil state: test but no write
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;

    // Blend state: alpha blending
    psoDesc.blend.blendEnable = true;
    psoDesc.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoDesc.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoDesc.blend.blendOp = EBlendOp::Add;
    psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
    psoDesc.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoDesc.blend.blendOpAlpha = EBlendOp::Add;

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::LineList;

    // Render target format: LDR uses R8G8B8A8_UNORM_SRGB
    psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    psoDesc.setLayouts[1] = m_perPassLayout;  // Set 1: PerPass (space1)
    psoDesc.debugName = "DebugLine_DS_PSO";

    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));
    if (!m_pso_ds) {
        CFFLog::Error("[DebugLinePass] Failed to create DS PSO");
        return;
    }

    CFFLog::Info("[DebugLinePass] Descriptor set path initialized");
}
