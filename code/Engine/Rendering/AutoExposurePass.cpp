#include "AutoExposurePass.h"
#include "ComputePassLayout.h"
#include "Engine/SceneLightSettings.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
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

    createBuffers();
    createSamplers();

    initDescriptorSets();

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

    // Cleanup DS resources
    m_histogramCS_ds.reset();
    m_adaptationCS_ds.reset();
    m_histogramPSO_ds.reset();
    m_adaptationPSO_ds.reset();

    m_pointSampler.reset();
    m_linearSampler.reset();

    auto* ctx = CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_perPassSet) {
            ctx->FreeDescriptorSet(m_perPassSet);
            m_perPassSet = nullptr;
        }
        if (m_computePerPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_computePerPassLayout);
            m_computePerPassLayout = nullptr;
        }
    }

    m_currentExposure = 1.0f;
    m_targetExposure = 1.0f;
    m_initialized = false;
    m_firstFrame = true;

    CFFLog::Info("[AutoExposurePass] Shutdown");
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

void CAutoExposurePass::createSamplers() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    auto createClampSampler = [ctx](EFilter filter) {
        SamplerDesc desc;
        desc.filter = filter;
        desc.addressU = ETextureAddressMode::Clamp;
        desc.addressV = ETextureAddressMode::Clamp;
        desc.addressW = ETextureAddressMode::Clamp;
        return SamplerPtr(ctx->CreateSampler(desc));
    };

    m_pointSampler = createClampSampler(EFilter::MinMagMipPoint);
    m_linearSampler = createClampSampler(EFilter::MinMagMipLinear);
}

// ============================================
// Rendering
// ============================================

void CAutoExposurePass::Render(ICommandList* cmdList,
                                ITexture* hdrInput,
                                uint32_t width, uint32_t height,
                                float deltaTime,
                                const SAutoExposureSettings& settings) {
    if (!m_initialized || !cmdList || !hdrInput || width == 0 || height == 0) {
        m_currentExposure = 1.0f;
        return;
    }

    // Guard against invalid state
    if (!m_histogramBuffer || !m_exposureBuffer) {
        m_currentExposure = 1.0f;
        return;
    }

    // Note: CPU readback of exposure value is not yet implemented in this RHI.
    // The exposure is computed on GPU but we can't read it back to CPU yet.
    // For now, m_currentExposure stays at 1.0f.
    // TODO: Implement proper buffer mapping/readback in RHI layer.
    m_firstFrame = false;

    // Use descriptor set path if available (DX12)
    if (IsDescriptorSetModeAvailable()) {
        // Step 1: Build histogram
        {
            CScopedDebugEvent evt(cmdList, L"AutoExposure Histogram (DS)");
            dispatchHistogram_DS(cmdList, hdrInput, width, height, settings);
        }

        // Step 2: Calculate and adapt exposure
        {
            CScopedDebugEvent evt(cmdList, L"AutoExposure Adaptation (DS)");
            dispatchAdaptation_DS(cmdList, deltaTime, width * height, settings);
        }
    } else {
        CFFLog::Warning("[AutoExposurePass] Legacy binding disabled and descriptor sets not available");
        m_currentExposure = 1.0f;
        return;
    }

    // Step 3: Copy exposure to staging buffer (for future readback implementation)
    if (m_exposureReadback) {
        cmdList->CopyBuffer(m_exposureReadback.get(), 0, m_exposureBuffer.get(), 0, 3 * sizeof(float));
    }

    // Step 4: Copy histogram for debug UI (for future readback implementation)
    if (m_histogramReadback) {
        cmdList->CopyBuffer(m_histogramReadback.get(), 0, m_histogramBuffer.get(), 0,
                            AutoExposureConfig::HISTOGRAM_BINS * sizeof(uint32_t));
    }
}

void CAutoExposurePass::readbackHistogram(ICommandList* /*cmdList*/) {
    // Note: CPU readback of histogram is not yet implemented in this RHI.
    // The histogram data is copied to staging buffer but we can't map it yet.
    // TODO: Implement proper buffer mapping/readback in RHI layer.
}

void CAutoExposurePass::RenderDebugOverlay(ICommandList* /*cmdList*/,
                                           ITexture* /*renderTarget*/,
                                           uint32_t /*width*/, uint32_t /*height*/) {
    CFFLog::Warning("[AutoExposurePass] RenderDebugOverlay() skipped - descriptor set path not implemented");
}

// ============================================
// Descriptor Set Dispatch Helpers
// ============================================

void CAutoExposurePass::dispatchHistogram_DS(ICommandList* cmdList,
                                              ITexture* hdrInput,
                                              uint32_t width, uint32_t height,
                                              const SAutoExposureSettings& settings) {
    if (!m_histogramPSO_ds || !m_perPassSet) return;

    // Clear histogram buffer
    const uint32_t clearValues[4] = { 0, 0, 0, 0 };
    cmdList->ClearUnorderedAccessViewUint(m_histogramBuffer.get(), clearValues);

    // UAV barrier after clear
    cmdList->Barrier(m_histogramBuffer.get(), EResourceState::UnorderedAccess, EResourceState::UnorderedAccess);

    // Update constant buffer (unified structure for both passes)
    CB_AutoExposure cb{};
    cb.screenSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
    cb.rcpScreenSize = XMFLOAT2(1.0f / width, 1.0f / height);
    cb.minLogLuminance = AutoExposureConfig::MIN_LOG_LUMINANCE;
    cb.maxLogLuminance = AutoExposureConfig::MAX_LOG_LUMINANCE;
    cb.centerWeight = settings.centerWeight;

    // Bind resources to descriptor set
    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cb, sizeof(cb)),
        BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input0, hdrInput),
        BindingSetItem::Buffer_UAV(ComputePassLayout::Slots::UAV_Output0, m_histogramBuffer.get())
    });

    cmdList->SetPipelineState(m_histogramPSO_ds.get());
    cmdList->BindDescriptorSet(1, m_perPassSet);

    // Dispatch
    uint32_t groupsX = calcDispatchGroups(width, AutoExposureConfig::HISTOGRAM_THREAD_GROUP_SIZE);
    uint32_t groupsY = calcDispatchGroups(height, AutoExposureConfig::HISTOGRAM_THREAD_GROUP_SIZE);
    cmdList->Dispatch(groupsX, groupsY, 1);

    // UAV barrier before adaptation pass reads histogram
    cmdList->Barrier(m_histogramBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

void CAutoExposurePass::dispatchAdaptation_DS(ICommandList* cmdList,
                                               float deltaTime,
                                               uint32_t /*pixelCount*/,
                                               const SAutoExposureSettings& settings) {
    if (!m_adaptationPSO_ds || !m_perPassSet) return;

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

    // Bind resources to descriptor set
    // Note: Histogram is read as SRV (t1), exposure is written as UAV (u1)
    m_perPassSet->Bind({
        BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cb, sizeof(cb)),
        BindingSetItem::Buffer_SRV(ComputePassLayout::Slots::Tex_Input1, m_histogramBuffer.get()),
        BindingSetItem::Buffer_UAV(ComputePassLayout::Slots::UAV_Output1, m_exposureBuffer.get())
    });

    cmdList->SetPipelineState(m_adaptationPSO_ds.get());
    cmdList->BindDescriptorSet(1, m_perPassSet);

    // Dispatch single thread group (256 threads for parallel reduction)
    cmdList->Dispatch(1, 1, 1);

    // UAV barrier for exposure buffer
    cmdList->Barrier(m_exposureBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
}

// ============================================
// Descriptor Set Initialization (DX12 only)
// ============================================
void CAutoExposurePass::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Check if descriptor sets are supported (DX12 only)
    if (ctx->GetBackend() != EBackend::DX12) {
        CFFLog::Info("[AutoExposurePass] DX11 mode - descriptor sets not supported");
        return;
    }

    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/AutoExposure_DS.cs.hlsl";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Create unified compute layout
    m_computePerPassLayout = ComputePassLayout::CreateComputePerPassLayout(ctx);
    if (!m_computePerPassLayout) {
        CFFLog::Error("[AutoExposurePass] Failed to create compute PerPass layout");
        return;
    }

    // Allocate descriptor set
    m_perPassSet = ctx->AllocateDescriptorSet(m_computePerPassLayout);
    if (!m_perPassSet) {
        CFFLog::Error("[AutoExposurePass] Failed to allocate PerPass descriptor set");
        return;
    }

    // Bind static samplers (not used by AutoExposure but required for layout compatibility)
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Point, m_pointSampler.get()));
    m_perPassSet->Bind(BindingSetItem::Sampler(ComputePassLayout::Slots::Samp_Linear, m_linearSampler.get()));

    // Compile SM 5.1 shaders
    // Histogram compute shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSBuildHistogram", "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[AutoExposurePass] CSBuildHistogram (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = "AutoExposure_DS_Histogram_CS";
        m_histogramCS_ds.reset(ctx->CreateShader(shaderDesc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_histogramCS_ds.get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = "AutoExposure_DS_Histogram_PSO";
        m_histogramPSO_ds.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    // Adaptation compute shader
    {
        SCompiledShader compiled = CompileShaderFromFile(shaderPath, "CSAdaptExposure", "cs_5_1", nullptr, debugShaders);
        if (!compiled.success) {
            CFFLog::Error("[AutoExposurePass] CSAdaptExposure (SM 5.1) compilation failed: %s", compiled.errorMessage.c_str());
            return;
        }

        ShaderDesc shaderDesc;
        shaderDesc.type = EShaderType::Compute;
        shaderDesc.bytecode = compiled.bytecode.data();
        shaderDesc.bytecodeSize = compiled.bytecode.size();
        shaderDesc.debugName = "AutoExposure_DS_Adaptation_CS";
        m_adaptationCS_ds.reset(ctx->CreateShader(shaderDesc));

        ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_adaptationCS_ds.get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = "AutoExposure_DS_Adaptation_PSO";
        m_adaptationPSO_ds.reset(ctx->CreateComputePipelineState(psoDesc));
    }

    CFFLog::Info("[AutoExposurePass] Descriptor set resources initialized");
}
