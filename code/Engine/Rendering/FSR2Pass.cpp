#include "FSR2Pass.h"
#include "Engine/Camera.h"
#include "Engine/SceneLightSettings.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIResources.h"
#include "Core/FFLog.h"

using namespace DirectX;

// ============================================
// Initialize
// ============================================
bool CFSR2Pass::Initialize() {
    if (!IsSupported()) {
        CFFLog::Warning("[FSR2Pass] FSR 2.0 requires DX12 backend");
        return false;
    }

    m_initialized = true;
    CFFLog::Info("[FSR2Pass] Initialized (context will be created on first use)");
    return true;
}

// ============================================
// Shutdown
// ============================================
void CFSR2Pass::Shutdown() {
    m_context.Shutdown();
    m_initialized = false;
    m_displayWidth = 0;
    m_displayHeight = 0;
    m_resetHistory = false;
}

// ============================================
// EnsureResources
// ============================================
void CFSR2Pass::EnsureResources(uint32_t displayWidth, uint32_t displayHeight, const SFSR2Settings& settings) {
    if (!m_initialized || !IsSupported()) return;

    // Check if we need to (re)create the context
    bool needsRecreate = !m_context.IsReady() ||
                         m_displayWidth != displayWidth ||
                         m_displayHeight != displayHeight ||
                         m_currentQualityMode != settings.qualityMode;

    if (needsRecreate) {
        m_displayWidth = displayWidth;
        m_displayHeight = displayHeight;
        m_currentQualityMode = settings.qualityMode;

        if (!m_context.Initialize(displayWidth, displayHeight, settings.qualityMode)) {
            CFFLog::Error("[FSR2Pass] Failed to initialize FSR2 context");
            return;
        }
    }
}

// ============================================
// GetRenderResolution
// ============================================
void CFSR2Pass::GetRenderResolution(uint32_t displayWidth, uint32_t displayHeight,
                                     const SFSR2Settings& settings,
                                     uint32_t& outRenderWidth, uint32_t& outRenderHeight) const {
    if (m_context.IsReady() && m_currentQualityMode == settings.qualityMode) {
        m_context.GetRenderResolution(outRenderWidth, outRenderHeight);
    } else {
        // Context not ready or quality mode changed, calculate based on quality mode
        if (settings.qualityMode == EFSR2QualityMode::NativeAA) {
            outRenderWidth = displayWidth;
            outRenderHeight = displayHeight;
        } else {
            // Use scale factors
            float scale = 1.0f;
            switch (settings.qualityMode) {
                case EFSR2QualityMode::Quality:          scale = 1.5f; break;
                case EFSR2QualityMode::Balanced:         scale = 1.7f; break;
                case EFSR2QualityMode::Performance:      scale = 2.0f; break;
                case EFSR2QualityMode::UltraPerformance: scale = 3.0f; break;
                default: scale = 1.5f; break;
            }
            outRenderWidth = static_cast<uint32_t>(displayWidth / scale);
            outRenderHeight = static_cast<uint32_t>(displayHeight / scale);
        }
    }
}

// ============================================
// Render
// ============================================
void CFSR2Pass::Render(
    RHI::ICommandList* cmdList,
    RHI::ITexture* colorInput,
    RHI::ITexture* depthInput,
    RHI::ITexture* velocityInput,
    RHI::ITexture* colorOutput,
    const CCamera& camera,
    float deltaTimeMs,
    uint32_t frameIndex,
    const SFSR2Settings& settings
) {
    if (!m_context.IsReady()) {
        CFFLog::Warning("[FSR2Pass] Render called but context not ready");
        return;
    }

    // Get jitter offset (in pixels)
    XMFLOAT2 jitterOffset = GetJitterOffset(frameIndex);

    // Execute FSR2
    m_context.Execute(
        cmdList,
        colorInput,
        depthInput,
        velocityInput,
        colorOutput,
        jitterOffset,
        deltaTimeMs,
        camera.nearZ,
        camera.farZ,
        camera.fovY,
        settings.sharpness,
        m_resetHistory
    );

    // Clear reset flag after use
    m_resetHistory = false;
}

// ============================================
// Jitter
// ============================================
DirectX::XMFLOAT2 CFSR2Pass::GetJitterOffset(uint32_t frameIndex) const {
    if (!m_context.IsReady()) {
        return XMFLOAT2(0.0f, 0.0f);
    }
    return m_context.GetJitterOffset(frameIndex);
}

DirectX::XMFLOAT2 CFSR2Pass::GetJitterOffsetNDC(uint32_t frameIndex, uint32_t renderWidth, uint32_t renderHeight) const {
    XMFLOAT2 jitterPixels = GetJitterOffset(frameIndex);

    // Convert to NDC space
    // NDC range is [-1, 1], so we need to scale by 2/resolution
    // Negative Y because DirectX has Y down in screen space but up in NDC
    return XMFLOAT2(
        2.0f * jitterPixels.x / static_cast<float>(renderWidth),
        -2.0f * jitterPixels.y / static_cast<float>(renderHeight)
    );
}

int32_t CFSR2Pass::GetJitterPhaseCount() const {
    if (!m_context.IsReady()) {
        return 1;
    }
    return m_context.GetJitterPhaseCount();
}
