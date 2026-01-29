#include "AutoExposurePass.h"
#include "Engine/SceneLightSettings.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/RHIHelpers.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <cmath>

using namespace DirectX;
using namespace RHI;

namespace {

// Helper to calculate dispatch group count
uint32_t calcDispatchGroups(uint32_t size, uint32_t groupSize) {
    return (size + groupSize - 1) / groupSize;
}

// Helper to compile a compute shader and create its PSO
bool createComputeShaderAndPSO(IRenderContext* ctx,
                                const std::string& shaderPath,
                                const char* entryPoint,
                                const char* shaderDebugName,
                                const char* psoDebugName,
                                bool debugShaders,
                                ShaderPtr& outShader,
                                PipelineStatePtr& outPSO) {
    SCompiledShader compiled = CompileShaderFromFile(shaderPath, entryPoint, "cs_5_0", nullptr, debugShaders);
    if (!compiled.success) {
        CFFLog::Error("[AutoExposurePass] %s compilation failed: %s", entryPoint, compiled.errorMessage.c_str());
        return false;
    }

    ShaderDesc shaderDesc;
    shaderDesc.type = EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();
    shaderDesc.debugName = shaderDebugName;
    outShader.reset(ctx->CreateShader(shaderDesc));

    ComputePipelineDesc psoDesc;
    psoDesc.computeShader = outShader.get();
    psoDesc.debugName = psoDebugName;
    outPSO.reset(ctx->CreateComputePipelineState(psoDesc));

    return true;
}

// Fullscreen quad vertex shader (embedded)
const char* g_debugVS = R"(
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput output;
    // Generate fullscreen triangle
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

// Debug histogram pixel shader (embedded)
const char* g_debugPS = R"(
cbuffer CB_HistogramDebug : register(b0) {
    float2 gScreenSize;
    float2 gHistogramPos;
    float2 gHistogramSize;
    float gCurrentExposure;
    float gTargetExposure;
    float gMinLogLuminance;
    float gMaxLogLuminance;
    float2 _pad;
};

StructuredBuffer<uint> gHistogram : register(t0);
StructuredBuffer<float> gExposureData : register(t1);  // [0]=current, [1]=target, [2]=maxHistogramValue

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float2 pixelPos = input.position.xy;

    // Check if pixel is within histogram area
    float2 histMin = gHistogramPos;
    float2 histMax = gHistogramPos + gHistogramSize;

    if (pixelPos.x < histMin.x || pixelPos.x > histMax.x ||
        pixelPos.y < histMin.y || pixelPos.y > histMax.y) {
        discard;
    }

    // Calculate which bin this pixel corresponds to
    float normalizedX = (pixelPos.x - histMin.x) / gHistogramSize.x;
    uint binIndex = uint(normalizedX * 256.0);
    binIndex = min(binIndex, 255u);

    // Get bin value (max is precomputed on GPU in exposure buffer)
    uint binValue = gHistogram[binIndex];
    float maxBinValue = max(gExposureData[2], 1.0);  // Read from GPU buffer

    // Calculate bar height
    float normalizedHeight = float(binValue) / maxBinValue;
    float barTop = histMax.y - normalizedHeight * gHistogramSize.y;

    // Draw bar
    if (pixelPos.y >= barTop) {
        // Color based on luminance range
        float logLum = lerp(gMinLogLuminance, gMaxLogLuminance, normalizedX);
        float3 barColor = float3(0.3, 0.6, 1.0);  // Blue bars

        // Highlight current exposure bin
        float currentLogLum = -log2(max(gCurrentExposure, 0.001));
        float currentBinNorm = saturate((currentLogLum - gMinLogLuminance) / (gMaxLogLuminance - gMinLogLuminance));
        if (abs(normalizedX - currentBinNorm) < 0.01) {
            barColor = float3(1.0, 0.8, 0.2);  // Yellow for current exposure
        }

        return float4(barColor, 0.8);
    }

    // Background
    return float4(0.1, 0.1, 0.1, 0.6);
}
)";

}  // namespace

// ============================================
// Lifecycle
// ============================================

bool CAutoExposurePass::Initialize() {
    if (m_initialized) return true;

    CFFLog::Info("[AutoExposurePass] Initializing...");

    if (!createShaders()) {
        CFFLog::Error("[AutoExposurePass] Failed to create shaders");
        return false;
    }
    createBuffers();
    createDebugResources();

    m_initialized = true;
    CFFLog::Info("[AutoExposurePass] Initialized successfully");
    return true;
}

void CAutoExposurePass::Shutdown() {
    m_histogramCS.reset();
    m_adaptationCS.reset();
    m_debugVS.reset();
    m_debugPS.reset();

    m_histogramPSO.reset();
    m_adaptationPSO.reset();
    m_debugPSO.reset();

    m_histogramBuffer.reset();
    m_exposureBuffer.reset();
    m_histogramReadback.reset();
    m_exposureReadback.reset();

    m_currentExposure = 1.0f;
    m_targetExposure = 1.0f;
    m_initialized = false;
    m_firstFrame = true;

    CFFLog::Info("[AutoExposurePass] Shutdown");
}

// ============================================
// Shader Creation
// ============================================

bool CAutoExposurePass::createShaders() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

#ifdef _DEBUG
    constexpr bool kDebugShaders = true;
#else
    constexpr bool kDebugShaders = false;
#endif

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/AutoExposure.cs.hlsl";

    // Histogram compute shader
    if (!createComputeShaderAndPSO(ctx, shaderPath, "CSBuildHistogram",
                              "AutoExposure_Histogram_CS", "AutoExposure_Histogram_PSO",
                              kDebugShaders, m_histogramCS, m_histogramPSO)) {
        return false;
    }

    // Adaptation compute shader
    if (!createComputeShaderAndPSO(ctx, shaderPath, "CSAdaptExposure",
                              "AutoExposure_Adaptation_CS", "AutoExposure_Adaptation_PSO",
                              kDebugShaders, m_adaptationCS, m_adaptationPSO)) {
        return false;
    }

    CFFLog::Info("[AutoExposurePass] Shaders compiled");
    return true;
}

void CAutoExposurePass::createBuffers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Histogram buffer (256 bins, UAV + Structured for SRV access)
    {
        BufferDesc desc;
        desc.size = AutoExposureConfig::HISTOGRAM_BINS * sizeof(uint32_t);
        desc.usage = EBufferUsage::UnorderedAccess | EBufferUsage::Structured;
        desc.structureByteStride = sizeof(uint32_t);
        desc.debugName = "AutoExposure_Histogram";
        m_histogramBuffer.reset(ctx->CreateBuffer(desc, nullptr));
    }

    // Exposure buffer (3 floats: current exposure, target exposure, max histogram value)
    // Initialize to 0.0 to trigger first-frame detection in shader
    {
        BufferDesc desc;
        desc.size = 3 * sizeof(float);
        desc.usage = EBufferUsage::UnorderedAccess | EBufferUsage::Structured;
        desc.structureByteStride = sizeof(float);
        desc.debugName = "AutoExposure_Exposure";
        float initialData[3] = { 0.0f, 0.0f, 1.0f };
        m_exposureBuffer.reset(ctx->CreateBuffer(desc, initialData));
    }

    // Histogram readback buffer (staging for CPU access)
    {
        BufferDesc desc;
        desc.size = AutoExposureConfig::HISTOGRAM_BINS * sizeof(uint32_t);
        desc.usage = EBufferUsage::Staging;
        desc.cpuAccess = ECPUAccess::Read;
        desc.debugName = "AutoExposure_Histogram_Readback";
        m_histogramReadback.reset(ctx->CreateBuffer(desc, nullptr));
    }

    // Exposure readback buffer (staging for CPU access, 1 frame behind)
    {
        BufferDesc desc;
        desc.size = 3 * sizeof(float);
        desc.usage = EBufferUsage::Staging;
        desc.cpuAccess = ECPUAccess::Read;
        desc.debugName = "AutoExposure_Exposure_Readback";
        m_exposureReadback.reset(ctx->CreateBuffer(desc, nullptr));
    }

    CFFLog::Info("[AutoExposurePass] Buffers created");
}

void CAutoExposurePass::createDebugResources() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

#ifdef _DEBUG
    constexpr bool kDebugShaders = true;
#else
    constexpr bool kDebugShaders = false;
#endif

    // Compile debug vertex shader
    {
        SCompiledShader compiled = CompileShaderFromSource(g_debugVS, "main", "vs_5_0", nullptr, kDebugShaders);
        if (compiled.success) {
            ShaderDesc desc;
            desc.type = EShaderType::Vertex;
            desc.bytecode = compiled.bytecode.data();
            desc.bytecodeSize = compiled.bytecode.size();
            desc.debugName = "AutoExposure_Debug_VS";
            m_debugVS.reset(ctx->CreateShader(desc));
        }
    }

    // Compile debug pixel shader
    {
        SCompiledShader compiled = CompileShaderFromSource(g_debugPS, "main", "ps_5_0", nullptr, kDebugShaders);
        if (compiled.success) {
            ShaderDesc desc;
            desc.type = EShaderType::Pixel;
            desc.bytecode = compiled.bytecode.data();
            desc.bytecodeSize = compiled.bytecode.size();
            desc.debugName = "AutoExposure_Debug_PS";
            m_debugPS.reset(ctx->CreateShader(desc));
        }
    }

    // Create debug PSO
    if (m_debugVS && m_debugPS) {
        PipelineStateDesc psoDesc;
        psoDesc.vertexShader = m_debugVS.get();
        psoDesc.pixelShader = m_debugPS.get();
        psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
        psoDesc.rasterizer.cullMode = ECullMode::None;
        psoDesc.depthStencil.depthEnable = false;
        psoDesc.depthStencil.depthWriteEnable = false;
        psoDesc.blend.blendEnable = true;
        psoDesc.blend.srcBlend = EBlendFactor::SrcAlpha;
        psoDesc.blend.dstBlend = EBlendFactor::InvSrcAlpha;
        psoDesc.renderTargetFormats = { ETextureFormat::R8G8B8A8_UNORM_SRGB };
        psoDesc.debugName = "AutoExposure_Debug_PSO";
        m_debugPSO.reset(ctx->CreatePipelineState(psoDesc));
    }

    CFFLog::Info("[AutoExposurePass] Debug resources created");
}

// ============================================
// Rendering
// ============================================

void CAutoExposurePass::Render(ICommandList* cmdList,
                                ITexture* hdrInput,
                                uint32_t width, uint32_t height,
                                float deltaTime,
                                const SAutoExposureSettings& settings) {
#ifndef FF_LEGACY_BINDING_DISABLED
    if (!m_initialized || !cmdList || !hdrInput || width == 0 || height == 0) {
        m_currentExposure = 1.0f;
        return;
    }

    // Guard against invalid state
    if (!m_histogramPSO || !m_adaptationPSO || !m_histogramBuffer || !m_exposureBuffer) {
        m_currentExposure = 1.0f;
        return;
    }

    // Note: CPU readback of exposure value is not yet implemented in this RHI.
    // The exposure is computed on GPU but we can't read it back to CPU yet.
    // For now, m_currentExposure stays at 1.0f.
    // TODO: Implement proper buffer mapping/readback in RHI layer.
    m_firstFrame = false;

    // Step 1: Build histogram
    dispatchHistogram(cmdList, hdrInput, width, height, settings);

    // Step 2: Calculate and adapt exposure
    dispatchAdaptation(cmdList, deltaTime, width * height, settings);

    // Step 3: Copy exposure to staging buffer (for future readback implementation)
    if (m_exposureReadback) {
        cmdList->CopyBuffer(m_exposureReadback.get(), 0, m_exposureBuffer.get(), 0, 3 * sizeof(float));
    }

    // Step 4: Copy histogram for debug UI (for future readback implementation)
    if (m_histogramReadback) {
        cmdList->CopyBuffer(m_histogramReadback.get(), 0, m_histogramBuffer.get(), 0,
                            AutoExposureConfig::HISTOGRAM_BINS * sizeof(uint32_t));
    }
#else
    CFFLog::Warning("[AutoExposurePass] Render() skipped - descriptor set path not implemented");
    m_currentExposure = 1.0f;
    (void)cmdList; (void)hdrInput; (void)width; (void)height; (void)deltaTime; (void)settings;
#endif
}

void CAutoExposurePass::dispatchHistogram(ICommandList* cmdList,
                                          ITexture* hdrInput,
                                          uint32_t width, uint32_t height,
                                          const SAutoExposureSettings& settings) {
    // Clear histogram buffer
    const uint32_t clearValues[4] = { 0, 0, 0, 0 };
    cmdList->ClearUnorderedAccessViewUint(m_histogramBuffer.get(), clearValues);

    // UAV barrier after clear
    cmdList->Barrier(m_histogramBuffer.get(), EResourceState::UnorderedAccess, EResourceState::UnorderedAccess);

    // Set PSO
    cmdList->SetPipelineState(m_histogramPSO.get());

    // Update constant buffer (unified structure for both passes)
    CB_AutoExposure cb{};
    cb.screenSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
    cb.rcpScreenSize = XMFLOAT2(1.0f / width, 1.0f / height);
    cb.minLogLuminance = AutoExposureConfig::MIN_LOG_LUMINANCE;
    cb.maxLogLuminance = AutoExposureConfig::MAX_LOG_LUMINANCE;
    cb.centerWeight = settings.centerWeight;
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

    // Bind resources
    cmdList->SetShaderResource(EShaderStage::Compute, 0, hdrInput);
    cmdList->SetUnorderedAccess(0, m_histogramBuffer.get());

    // Dispatch
    uint32_t groupsX = calcDispatchGroups(width, AutoExposureConfig::HISTOGRAM_THREAD_GROUP_SIZE);
    uint32_t groupsY = calcDispatchGroups(height, AutoExposureConfig::HISTOGRAM_THREAD_GROUP_SIZE);
    cmdList->Dispatch(groupsX, groupsY, 1);

    // Unbind UAV
    cmdList->SetUnorderedAccess(0, nullptr);

    // UAV barrier before adaptation pass reads histogram
    cmdList->Barrier(m_histogramBuffer.get(), EResourceState::UnorderedAccess, EResourceState::UnorderedAccess);
}

void CAutoExposurePass::dispatchAdaptation(ICommandList* cmdList,
                                           float deltaTime,
                                           uint32_t /*pixelCount*/,
                                           const SAutoExposureSettings& settings) {
    // Set PSO
    cmdList->SetPipelineState(m_adaptationPSO.get());

    // Update constant buffer (unified structure for both passes)
    CB_AutoExposure cb{};
    cb.minLogLuminance = AutoExposureConfig::MIN_LOG_LUMINANCE;
    cb.maxLogLuminance = AutoExposureConfig::MAX_LOG_LUMINANCE;
    cb.deltaTime = deltaTime;
    cb.minExposure = std::pow(2.0f, settings.minEV);
    cb.maxExposure = std::pow(2.0f, settings.maxEV);
    cb.adaptSpeedUp = settings.adaptSpeedUp;
    cb.adaptSpeedDown = settings.adaptSpeedDown;
    cb.exposureCompensation = settings.exposureCompensation;
    cmdList->SetConstantBufferData(EShaderStage::Compute, 0, &cb, sizeof(cb));

    // Bind resources
    cmdList->SetShaderResourceBuffer(EShaderStage::Compute, 0, m_histogramBuffer.get());
    cmdList->SetUnorderedAccess(0, m_exposureBuffer.get());

    // Dispatch single thread group (256 threads for parallel reduction)
    cmdList->Dispatch(1, 1, 1);

    // Unbind resources
    cmdList->SetUnorderedAccess(0, nullptr);
    cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 1);
}

void CAutoExposurePass::readbackHistogram(ICommandList* /*cmdList*/) {
    // Note: CPU readback of histogram is not yet implemented in this RHI.
    // The histogram data is copied to staging buffer but we can't map it yet.
    // TODO: Implement proper buffer mapping/readback in RHI layer.
}

void CAutoExposurePass::RenderDebugOverlay(ICommandList* cmdList,
                                           ITexture* renderTarget,
                                           uint32_t width, uint32_t height) {
#ifndef FF_LEGACY_BINDING_DISABLED
    if (!m_initialized || !m_debugPSO || !cmdList || !renderTarget) return;

    // Set render target
    cmdList->SetRenderTargets(1, &renderTarget, nullptr);
    cmdList->SetViewport(0, 0, static_cast<float>(width), static_cast<float>(height));
    cmdList->SetScissorRect(0, 0, width, height);

    // Set PSO
    cmdList->SetPipelineState(m_debugPSO.get());
    cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);

    // Update constant buffer
    CB_HistogramDebug cb{};
    cb.screenSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
    cb.histogramPos = XMFLOAT2(10.0f, static_cast<float>(height) - 140.0f);  // Bottom-left
    cb.histogramSize = XMFLOAT2(300.0f, 120.0f);
    cb.currentExposure = m_currentExposure;
    cb.targetExposure = m_targetExposure;
    cb.minLogLuminance = AutoExposureConfig::MIN_LOG_LUMINANCE;
    cb.maxLogLuminance = AutoExposureConfig::MAX_LOG_LUMINANCE;
    cmdList->SetConstantBufferData(EShaderStage::Pixel, 0, &cb, sizeof(cb));

    // Bind histogram buffer as SRV (t0)
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 0, m_histogramBuffer.get());
    // Bind exposure buffer as SRV (t1) - contains [current, target, maxHistogramValue]
    cmdList->SetShaderResourceBuffer(EShaderStage::Pixel, 1, m_exposureBuffer.get());

    // Draw fullscreen triangle (3 vertices, shader generates positions)
    cmdList->Draw(3, 0);

    // Unbind
    cmdList->UnbindShaderResources(EShaderStage::Pixel, 0, 2);
#else
    CFFLog::Warning("[AutoExposurePass] RenderDebugOverlay() skipped - descriptor set path not implemented");
    (void)cmdList; (void)renderTarget; (void)width; (void)height;
#endif
}
