// Engine/Rendering/GridPass.cpp
#include "GridPass.h"
#include "Core/FFLog.h"
#include "Core/RenderConfig.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/RHIManager.h"
#include "RHI/ShaderCompiler.h"
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

CGridPass& CGridPass::Instance() {
    static CGridPass instance;
    return instance;
}

void CGridPass::Initialize() {
    if (m_initialized) return;

    CreateShaders();
    CreateBuffers();
    CreatePipelineState();

    m_initialized = true;
}

void CGridPass::Shutdown() {
    m_pso.reset();
    m_vs.reset();
    m_ps.reset();
    m_cbPerFrame.reset();
    m_initialized = false;
}

void CGridPass::CreateShaders() {
    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) {
        CFFLog::Error("RHIManager not initialized!");
        return;
    }

    // Load shader source files
    std::string vsSource = LoadShaderSource("../source/code/Shader/Grid.vs.hlsl");
    std::string psSource = LoadShaderSource("../source/code/Shader/Grid.ps.hlsl");

    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Error("Failed to load Grid shader files!");
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
        CFFLog::Error("=== GRID VERTEX SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", vsCompiled.errorMessage.c_str());
        return;
    }

    // Compile Pixel Shader
    SCompiledShader psCompiled = CompileShaderFromSource(psSource, "main", "ps_5_0", nullptr, debugShaders);
    if (!psCompiled.success) {
        CFFLog::Error("=== GRID PIXEL SHADER COMPILATION ERROR ===");
        CFFLog::Error("%s", psCompiled.errorMessage.c_str());
        return;
    }

    // Create shader objects using RHI
    ShaderDesc vsDesc;
    vsDesc.type = EShaderType::Vertex;
    vsDesc.bytecode = vsCompiled.bytecode.data();
    vsDesc.bytecodeSize = vsCompiled.bytecode.size();
    m_vs.reset(renderContext->CreateShader(vsDesc));

    ShaderDesc psDesc;
    psDesc.type = EShaderType::Pixel;
    psDesc.bytecode = psCompiled.bytecode.data();
    psDesc.bytecodeSize = psCompiled.bytecode.size();
    m_ps.reset(renderContext->CreateShader(psDesc));
}

void CGridPass::CreateBuffers() {
    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    // Create constant buffer
    BufferDesc cbDesc;
    cbDesc.size = sizeof(CBPerFrame);
    cbDesc.usage = EBufferUsage::Constant;
    cbDesc.cpuAccess = ECPUAccess::Write;  // Need write access for Map/Unmap
    m_cbPerFrame.reset(renderContext->CreateBuffer(cbDesc, nullptr));
}

void CGridPass::CreatePipelineState() {
    if (!m_vs || !m_ps) return;

    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs.get();
    psoDesc.pixelShader = m_ps.get();

    // No input layout (vertex shader generates quad procedurally)
    psoDesc.inputLayout.clear();

    // Rasterizer state: default
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.frontCounterClockwise = false;

    // Depth stencil state: Read depth but don't write
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);  // LessEqual or GreaterEqual
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;  // Match GBuffer depth

    // Blend state: Alpha blending for RGB, preserve destination alpha
    psoDesc.blend.blendEnable = true;
    psoDesc.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoDesc.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoDesc.blend.blendOp = EBlendOp::Add;
    psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
    psoDesc.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoDesc.blend.blendOpAlpha = EBlendOp::Add;
    psoDesc.blend.renderTargetWriteMask = 0x07; // RGB only

    // Primitive topology
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;

    // Render target format: LDR uses R8G8B8A8_UNORM_SRGB
    psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    psoDesc.debugName = "Grid_PSO";

    m_pso.reset(renderContext->CreatePipelineState(psoDesc));
}

void CGridPass::Render(XMMATRIX view, XMMATRIX proj, XMFLOAT3 cameraPos) {
    if (!m_initialized || !m_enabled || !m_pso) return;

    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    // Update constant buffer
    XMMATRIX viewProj = view * proj;
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    CBPerFrame cb;
    cb.viewProj = XMMatrixTranspose(viewProj);       // HLSL expects column-major
    cb.invViewProj = XMMatrixTranspose(invViewProj);
    cb.cameraPos = cameraPos;
    cb.fadeStart = m_fadeStart;
    cb.fadeEnd = m_fadeEnd;
    cb.padding = XMFLOAT3(0, 0, 0);

    // Get command list
    ICommandList* cmdList = renderContext->GetCommandList();

    // Set pipeline state
    cmdList->SetPipelineState(m_pso.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);

    // Bind constant buffer (use SetConstantBufferData for DX12 compatibility)
    cmdList->SetConstantBufferData(EShaderStage::Vertex, 0, &cb, sizeof(CBPerFrame));
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(CBPerFrame));

    // Draw full-screen quad (4 vertices, triangle strip)
    cmdList->Draw(4, 0);
}
