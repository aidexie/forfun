// Engine/Rendering/GridPass.cpp
#include "GridPass.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/RenderConfig.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/RHIManager.h"
#include "RHI/IDescriptorSet.h"
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
    initDescriptorSets();

    m_initialized = true;
}

void CGridPass::Shutdown() {
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
    m_ps_ds.reset();

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
    if (!m_initialized || !m_enabled) return;

    IRenderContext* renderContext = CRHIManager::Instance().GetRenderContext();
    if (!renderContext) return;

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        // Update constant buffer data
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
        cmdList->SetPipelineState(m_pso_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleStrip);

        // Bind descriptor set with volatile CBV
        m_perPassSet->Bind(BindingSetItem::VolatileCBV(0, &cb, sizeof(cb)));
        cmdList->BindDescriptorSet(1, m_perPassSet);

        // Draw full-screen quad (4 vertices, triangle strip)
        cmdList->Draw(4, 0);
        return;
    }

#ifndef FF_LEGACY_BINDING_DISABLED
    if (!m_pso) return;

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
#else
    CFFLog::Warning("GridPass::Render() - Legacy binding disabled, descriptor set path not available");
#endif
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CGridPass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[GridPass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile SM 5.1 shaders
    std::string vsSource = LoadShaderSource(shaderDir + "Grid_DS.vs.hlsl");
    std::string psSource = LoadShaderSource(shaderDir + "Grid_DS.ps.hlsl");
    if (vsSource.empty() || psSource.empty()) {
        CFFLog::Warning("[GridPass] Failed to load DS shaders");
        return;
    }

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_1", nullptr, debugShaders);
    if (!vsCompiled.success || !psCompiled.success) {
        CFFLog::Error("[GridPass] DS shader compile error: %s %s",
                      vsCompiled.errorMessage.c_str(), psCompiled.errorMessage.c_str());
        return;
    }

    ShaderDesc vsDesc(EShaderType::Vertex, vsCompiled.bytecode.data(), vsCompiled.bytecode.size());
    vsDesc.debugName = "Grid_DS_VS";
    ShaderDesc psDesc(EShaderType::Pixel, psCompiled.bytecode.data(), psCompiled.bytecode.size());
    psDesc.debugName = "Grid_DS_PS";
    m_vs_ds.reset(ctx->CreateShader(vsDesc));
    m_ps_ds.reset(ctx->CreateShader(psDesc));

    if (!m_vs_ds || !m_ps_ds) {
        CFFLog::Error("[GridPass] Failed to create DS shaders");
        return;
    }

    // Create PerPass layout (Set 1): VolatileCBV only
    BindingLayoutDesc layoutDesc("Grid_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CBPerFrame)));

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) {
        CFFLog::Error("[GridPass] Failed to create descriptor set layout");
        return;
    }

    m_perPassSet = ctx->AllocateDescriptorSet(m_perPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[GridPass] Failed to allocate descriptor set");
        return;
    }

    // Create PSO with descriptor set layout
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs_ds.get();
    psoDesc.pixelShader = m_ps_ds.get();
    psoDesc.inputLayout.clear();  // No input layout (procedural quad)
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.rasterizer.fillMode = EFillMode::Solid;
    psoDesc.rasterizer.frontCounterClockwise = false;
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = GetDepthComparisonFunc(true);
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;
    psoDesc.blend.blendEnable = true;
    psoDesc.blend.srcBlend = EBlendFactor::SrcAlpha;
    psoDesc.blend.dstBlend = EBlendFactor::InvSrcAlpha;
    psoDesc.blend.blendOp = EBlendOp::Add;
    psoDesc.blend.srcBlendAlpha = EBlendFactor::One;
    psoDesc.blend.dstBlendAlpha = EBlendFactor::Zero;
    psoDesc.blend.blendOpAlpha = EBlendOp::Add;
    psoDesc.blend.renderTargetWriteMask = 0x07;  // RGB only
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleStrip;
    psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
    psoDesc.setLayouts[1] = m_perPassLayout;  // Set 1: PerPass (space1)
    psoDesc.debugName = "Grid_DS_PSO";

    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));
    if (!m_pso_ds) {
        CFFLog::Error("[GridPass] Failed to create DS PSO");
        return;
    }

    CFFLog::Info("[GridPass] Descriptor set path initialized");
}
