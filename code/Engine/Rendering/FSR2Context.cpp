#include "FSR2Context.h"
#include "RHI/RHIManager.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIResources.h"
#include "Core/FFLog.h"

#include <cstring>

using namespace DirectX;

// ============================================
// FSR2_AVAILABLE is defined by CMake based on whether
// FSR2 SDK shader permutation headers exist.
// ============================================
#if FSR2_AVAILABLE

// FSR2 SDK headers
#include "ffx_fsr2.h"
#include "ffx_fsr2_dx12.h"
#include <d3d12.h>

// ============================================
// Helper: Convert quality mode enum
// ============================================
static FfxFsr2QualityMode ToFfxQualityMode(EFSR2QualityMode mode) {
    switch (mode) {
        case EFSR2QualityMode::Quality:          return FFX_FSR2_QUALITY_MODE_QUALITY;
        case EFSR2QualityMode::Balanced:         return FFX_FSR2_QUALITY_MODE_BALANCED;
        case EFSR2QualityMode::Performance:      return FFX_FSR2_QUALITY_MODE_PERFORMANCE;
        case EFSR2QualityMode::UltraPerformance: return FFX_FSR2_QUALITY_MODE_ULTRA_PERFORMANCE;
        default:                                 return FFX_FSR2_QUALITY_MODE_QUALITY;
    }
}

// ============================================
// Constructor / Destructor
// ============================================
CFSR2Context::CFSR2Context() = default;

CFSR2Context::~CFSR2Context() {
    Shutdown();
}

// ============================================
// Static: Check if FSR2 is supported
// ============================================
bool CFSR2Context::IsSupported() {
    return RHI::CRHIManager::Instance().GetBackend() == RHI::EBackend::DX12;
}

// ============================================
// Initialize
// ============================================
bool CFSR2Context::Initialize(uint32_t displayWidth, uint32_t displayHeight, EFSR2QualityMode mode) {
    if (m_initialized) {
        Shutdown();
    }

    // Check DX12 backend
    if (!IsSupported()) {
        CFFLog::Warning("[FSR2] FSR 2.0 requires DX12 backend. Current backend: DX11");
        return false;
    }

    m_displayWidth = displayWidth;
    m_displayHeight = displayHeight;
    m_qualityMode = mode;

    // Calculate render resolution
    calculateRenderResolution();

    // Get DX12 device
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    ID3D12Device* device = static_cast<ID3D12Device*>(ctx->GetNativeDevice());
    if (!device) {
        CFFLog::Error("[FSR2] Failed to get DX12 device");
        return false;
    }

    // Allocate scratch buffer for FSR2 backend
    m_scratchBufferSize = ffxFsr2GetScratchMemorySizeDX12();
    m_scratchBuffer = malloc(m_scratchBufferSize);
    if (!m_scratchBuffer) {
        CFFLog::Error("[FSR2] Failed to allocate scratch buffer (%zu bytes)", m_scratchBufferSize);
        return false;
    }

    // Get FSR2 interface for DX12
    FfxFsr2Interface fsr2Interface = {};
    FfxErrorCode result = ffxFsr2GetInterfaceDX12(
        &fsr2Interface,
        device,
        m_scratchBuffer,
        m_scratchBufferSize
    );
    if (result != FFX_OK) {
        CFFLog::Error("[FSR2] ffxFsr2GetInterfaceDX12 failed: %d", result);
        free(m_scratchBuffer);
        m_scratchBuffer = nullptr;
        return false;
    }

    // Create FSR2 context
    FfxFsr2ContextDescription contextDesc = {};
    contextDesc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE |      // HDR input
                        FFX_FSR2_ENABLE_DEPTH_INVERTED |          // Reversed-Z
                        FFX_FSR2_ENABLE_AUTO_EXPOSURE;            // Let FSR2 compute exposure
    contextDesc.maxRenderSize.width = m_renderWidth;
    contextDesc.maxRenderSize.height = m_renderHeight;
    contextDesc.displaySize.width = m_displayWidth;
    contextDesc.displaySize.height = m_displayHeight;
    contextDesc.callbacks = fsr2Interface;
    contextDesc.device = ffxGetDeviceDX12(device);
    contextDesc.fpMessage = nullptr;  // No debug message callback

    // Allocate context
    m_context = new FfxFsr2Context();
    result = ffxFsr2ContextCreate(static_cast<FfxFsr2Context*>(m_context), &contextDesc);
    if (result != FFX_OK) {
        CFFLog::Error("[FSR2] ffxFsr2ContextCreate failed: %d", result);
        delete static_cast<FfxFsr2Context*>(m_context);
        m_context = nullptr;
        free(m_scratchBuffer);
        m_scratchBuffer = nullptr;
        return false;
    }

    m_initialized = true;
    CFFLog::Info("[FSR2] Initialized - Display: %ux%u, Render: %ux%u, Mode: %d",
                 m_displayWidth, m_displayHeight, m_renderWidth, m_renderHeight, static_cast<int>(mode));
    return true;
}

// ============================================
// Shutdown
// ============================================
void CFSR2Context::Shutdown() {
    if (!m_initialized) return;

    if (m_context) {
        ffxFsr2ContextDestroy(static_cast<FfxFsr2Context*>(m_context));
        delete static_cast<FfxFsr2Context*>(m_context);
        m_context = nullptr;
    }

    if (m_scratchBuffer) {
        free(m_scratchBuffer);
        m_scratchBuffer = nullptr;
    }

    m_initialized = false;
    CFFLog::Info("[FSR2] Shutdown complete");
}

// ============================================
// Resolution Management
// ============================================
void CFSR2Context::calculateRenderResolution() {
    if (m_qualityMode == EFSR2QualityMode::NativeAA) {
        // Native AA mode: render at display resolution
        m_renderWidth = m_displayWidth;
        m_renderHeight = m_displayHeight;
    } else {
        // Use FSR2's helper to calculate render resolution
        ffxFsr2GetRenderResolutionFromQualityMode(
            &m_renderWidth,
            &m_renderHeight,
            m_displayWidth,
            m_displayHeight,
            ToFfxQualityMode(m_qualityMode)
        );
    }
}

void CFSR2Context::GetRenderResolution(uint32_t& outWidth, uint32_t& outHeight) const {
    outWidth = m_renderWidth;
    outHeight = m_renderHeight;
}

float CFSR2Context::GetUpscaleFactor() const {
    if (m_qualityMode == EFSR2QualityMode::NativeAA) {
        return 1.0f;
    }
    return ffxFsr2GetUpscaleRatioFromQualityMode(ToFfxQualityMode(m_qualityMode));
}

// ============================================
// Jitter
// ============================================
DirectX::XMFLOAT2 CFSR2Context::GetJitterOffset(uint32_t frameIndex) const {
    if (!m_initialized) {
        return XMFLOAT2(0.0f, 0.0f);
    }

    int32_t phaseCount = GetJitterPhaseCount();
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    ffxFsr2GetJitterOffset(&jitterX, &jitterY, frameIndex, phaseCount);
    return XMFLOAT2(jitterX, jitterY);
}

int32_t CFSR2Context::GetJitterPhaseCount() const {
    if (!m_initialized) {
        return 1;
    }
    return ffxFsr2GetJitterPhaseCount(m_renderWidth, m_displayWidth);
}

// ============================================
// Configuration
// ============================================
void CFSR2Context::SetQualityMode(EFSR2QualityMode mode) {
    if (mode == m_qualityMode) return;

    // Quality mode change requires context recreation
    uint32_t displayW = m_displayWidth;
    uint32_t displayH = m_displayHeight;
    Shutdown();
    Initialize(displayW, displayH, mode);
}

// ============================================
// Execute
// ============================================
void CFSR2Context::Execute(
    RHI::ICommandList* cmdList,
    RHI::ITexture* colorInput,
    RHI::ITexture* depthInput,
    RHI::ITexture* velocityInput,
    RHI::ITexture* colorOutput,
    const DirectX::XMFLOAT2& jitterOffset,
    float deltaTimeMs,
    float cameraNear,
    float cameraFar,
    float cameraFovY,
    float sharpness,
    bool reset
) {
    if (!m_initialized || !m_context) {
        CFFLog::Warning("[FSR2] Execute called but context not initialized");
        return;
    }

    // Get native DX12 command list
    ID3D12GraphicsCommandList* dx12CmdList = static_cast<ID3D12GraphicsCommandList*>(cmdList->GetNativeContext());
    if (!dx12CmdList) {
        CFFLog::Error("[FSR2] Failed to get DX12 command list");
        return;
    }

    // Get native DX12 resources
    ID3D12Resource* colorRes = static_cast<ID3D12Resource*>(colorInput->GetNativeHandle());
    ID3D12Resource* depthRes = static_cast<ID3D12Resource*>(depthInput->GetNativeHandle());
    ID3D12Resource* velocityRes = static_cast<ID3D12Resource*>(velocityInput->GetNativeHandle());
    ID3D12Resource* outputRes = static_cast<ID3D12Resource*>(colorOutput->GetNativeHandle());

    // Get FSR2 context pointer
    FfxFsr2Context* fsr2Context = static_cast<FfxFsr2Context*>(m_context);

    // Prepare dispatch description
    FfxFsr2DispatchDescription dispatchDesc = {};
    dispatchDesc.commandList = ffxGetCommandListDX12(dx12CmdList);
    dispatchDesc.color = ffxGetResourceDX12(fsr2Context, colorRes, L"FSR2_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.depth = ffxGetResourceDX12(fsr2Context, depthRes, L"FSR2_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxGetResourceDX12(fsr2Context, velocityRes, L"FSR2_InputMotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.output = ffxGetResourceDX12(fsr2Context, outputRes, L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatchDesc.exposure = {};  // Let FSR2 auto-compute exposure
    dispatchDesc.reactive = {};  // No reactive mask
    dispatchDesc.transparencyAndComposition = {};  // No T&C mask

    dispatchDesc.jitterOffset.x = jitterOffset.x;
    dispatchDesc.jitterOffset.y = jitterOffset.y;

    // Motion vector scale: our motion vectors are in screen-space pixels
    // FSR2 expects: how to convert motion vector values to pixels
    // Since our MVs are already in pixels at render resolution, scale = 1.0
    dispatchDesc.motionVectorScale.x = 1.0f;
    dispatchDesc.motionVectorScale.y = 1.0f;

    dispatchDesc.renderSize.width = m_renderWidth;
    dispatchDesc.renderSize.height = m_renderHeight;
    dispatchDesc.enableSharpening = sharpness > 0.0f;
    dispatchDesc.sharpness = sharpness;
    dispatchDesc.frameTimeDelta = deltaTimeMs;
    dispatchDesc.preExposure = 1.0f;  // No pre-exposure applied
    dispatchDesc.reset = reset;
    dispatchDesc.cameraNear = cameraNear;
    dispatchDesc.cameraFar = cameraFar;
    dispatchDesc.cameraFovAngleVertical = cameraFovY;
    dispatchDesc.viewSpaceToMetersFactor = 1.0f;  // 1 unit = 1 meter

    // Execute FSR2
    FfxErrorCode result = ffxFsr2ContextDispatch(fsr2Context, &dispatchDesc);
    if (result != FFX_OK) {
        CFFLog::Error("[FSR2] ffxFsr2ContextDispatch failed: %d", result);
    }
}

#else  // FSR2_AVAILABLE == 0: Stub implementation

// ============================================
// Stub Implementation (FSR2 SDK not available)
// ============================================

CFSR2Context::CFSR2Context() = default;
CFSR2Context::~CFSR2Context() = default;

bool CFSR2Context::IsSupported() {
    // FSR2 SDK not built, always return false
    return false;
}

bool CFSR2Context::Initialize(uint32_t, uint32_t, EFSR2QualityMode) {
    CFFLog::Warning("[FSR2] FSR 2.0 SDK not available. Build FSR2 SDK first.");
    return false;
}

void CFSR2Context::Shutdown() {}

void CFSR2Context::calculateRenderResolution() {
    m_renderWidth = m_displayWidth;
    m_renderHeight = m_displayHeight;
}

void CFSR2Context::GetRenderResolution(uint32_t& outWidth, uint32_t& outHeight) const {
    outWidth = m_displayWidth;
    outHeight = m_displayHeight;
}

float CFSR2Context::GetUpscaleFactor() const {
    return 1.0f;
}

DirectX::XMFLOAT2 CFSR2Context::GetJitterOffset(uint32_t) const {
    return XMFLOAT2(0.0f, 0.0f);
}

int32_t CFSR2Context::GetJitterPhaseCount() const {
    return 1;
}

void CFSR2Context::SetQualityMode(EFSR2QualityMode) {}

void CFSR2Context::Execute(
    RHI::ICommandList*,
    RHI::ITexture*,
    RHI::ITexture*,
    RHI::ITexture*,
    RHI::ITexture*,
    const DirectX::XMFLOAT2&,
    float,
    float,
    float,
    float,
    float,
    bool
) {
    CFFLog::Warning("[FSR2] Execute called but FSR2 SDK not available");
}

#endif  // FSR2_AVAILABLE
