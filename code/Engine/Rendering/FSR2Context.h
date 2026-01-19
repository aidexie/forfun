#pragma once
#include <cstdint>
#include <DirectXMath.h>
#include "Engine/SceneLightSettings.h"  // For EFSR2QualityMode

// Forward declarations - avoid exposing FSR2 SDK types in public header
struct FfxFsr2Context;

namespace RHI {
    class ICommandList;
    class ITexture;
}

// ============================================
// CFSR2Context - FSR 2.0 SDK Wrapper
// ============================================
// Encapsulates AMD FidelityFX Super Resolution 2 SDK.
// Follows the same pattern as CLightmapDenoiser (opaque pointer).
//
// Usage:
//   CFSR2Context fsr2;
//   if (fsr2.Initialize(1920, 1080, EFSR2QualityMode::Quality)) {
//       fsr2.Execute(cmdList, color, depth, velocity, output, ...);
//   }
//   fsr2.Shutdown();
class CFSR2Context {
public:
    CFSR2Context();
    ~CFSR2Context();

    // Non-copyable
    CFSR2Context(const CFSR2Context&) = delete;
    CFSR2Context& operator=(const CFSR2Context&) = delete;

    // ============================================
    // Lifecycle
    // ============================================

    // Initialize FSR2 context for given display resolution and quality mode
    // Returns false if FSR2 initialization fails (e.g., DX11 backend)
    bool Initialize(uint32_t displayWidth, uint32_t displayHeight, EFSR2QualityMode mode);

    // Shutdown and release FSR2 resources
    void Shutdown();

    // Check if FSR2 is initialized and ready
    bool IsReady() const { return m_initialized; }

    // Check if FSR2 is supported (DX12 only)
    static bool IsSupported();

    // ============================================
    // Resolution Management
    // ============================================

    // Get render resolution for current quality mode
    void GetRenderResolution(uint32_t& outWidth, uint32_t& outHeight) const;

    // Get upscale factor (e.g., 1.5 for Quality mode)
    float GetUpscaleFactor() const;

    // Get display resolution
    uint32_t GetDisplayWidth() const { return m_displayWidth; }
    uint32_t GetDisplayHeight() const { return m_displayHeight; }

    // Get render resolution
    uint32_t GetRenderWidth() const { return m_renderWidth; }
    uint32_t GetRenderHeight() const { return m_renderHeight; }

    // ============================================
    // Jitter
    // ============================================

    // Get jitter offset for current frame (in pixels, relative to render resolution)
    DirectX::XMFLOAT2 GetJitterOffset(uint32_t frameIndex) const;

    // Get jitter phase count (number of samples in jitter sequence)
    int32_t GetJitterPhaseCount() const;

    // ============================================
    // Execution
    // ============================================

    // Execute FSR 2.0 upscaling
    // colorInput: HDR color buffer at render resolution
    // depthInput: Depth buffer at render resolution (reversed-Z: near=1, far=0)
    // velocityInput: Motion vectors at render resolution (screen-space pixels)
    // colorOutput: Output HDR buffer at display resolution
    // jitterOffset: Sub-pixel jitter applied to projection matrix (in pixels)
    // deltaTimeMs: Frame time in milliseconds
    // cameraNear/Far: Camera near/far planes
    // cameraFovY: Vertical field of view in radians
    // reset: Set true to invalidate history (camera cut, scene change)
    void Execute(
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
    );

    // ============================================
    // Configuration
    // ============================================

    // Change quality mode (requires context recreation)
    void SetQualityMode(EFSR2QualityMode mode);
    EFSR2QualityMode GetQualityMode() const { return m_qualityMode; }

private:
    void calculateRenderResolution();

    // FSR2 context (opaque pointer to avoid SDK header in public interface)
    void* m_context = nullptr;          // FfxFsr2Context*
    void* m_scratchBuffer = nullptr;    // Scratch memory for FSR2 backend
    size_t m_scratchBufferSize = 0;

    // Resolution state
    uint32_t m_displayWidth = 0;
    uint32_t m_displayHeight = 0;
    uint32_t m_renderWidth = 0;
    uint32_t m_renderHeight = 0;
    EFSR2QualityMode m_qualityMode = EFSR2QualityMode::Quality;

    bool m_initialized = false;
};
