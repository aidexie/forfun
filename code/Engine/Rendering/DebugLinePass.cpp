// Engine/Rendering/DebugLinePass.cpp
#include "DebugLinePass.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "Core/FFLog.h"
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

    m_initialized = true;
}

void CDebugLinePass::Shutdown() {
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
    psoDesc.depthStencil.depthFunc = EComparisonFunc::LessEqual;

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
    if (!m_initialized || m_dynamicLines.empty() || !m_pso) return;

    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    UpdateVertexBuffer();

    // Update VS constant buffer
    XMMATRIX viewProj = view * proj;
    CBPerFrameVS cbVS;
    cbVS.viewProj = XMMatrixTranspose(viewProj);

    // Update GS constant buffer
    CBPerFrameGS cbGS;
    cbGS.viewportSize = XMFLOAT2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
    cbGS.lineThickness = m_lineThickness;
    cbGS.padding = 0.0f;

    // Get command list and render
    ICommandList* cmdList = renderContext->GetCommandList();

    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::LineList);

    unsigned int stride = sizeof(LineVertex);
    unsigned int offset = 0;
    cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), stride, offset);

    // Use SetConstantBufferData for DX12 compatibility
    cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &cbVS, sizeof(CBPerFrameVS));
    cmdList->SetConstantBufferData(EShaderStage::Geometry, 0, &cbGS, sizeof(CBPerFrameGS));

    unsigned int vertexCount = static_cast<unsigned int>(m_dynamicLines.size());
    cmdList->Draw(vertexCount, 0);
}
