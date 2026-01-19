#pragma once
#include "FSR2Context.h"
#include "RHI/RHIPointers.h"
#include <DirectXMath.h>
#include <cstdint>

namespace RHI {
    class ICommandList;
    class ITexture;
}

class CCamera;
struct SFSR2Settings;  // Forward declaration from SceneLightSettings.h

// ============================================
// CFSR2Pass - FSR 2.0 Render Pass
// ============================================
// Orchestrates FSR 2.0 upscaling within the render pipeline.
// Replaces TAA pass - provides both temporal anti-aliasing and upscaling.
//
// Pipeline Position: After SSR, before Auto Exposure (HDR space)
//
// Usage:
//   m_fsr2Pass.Initialize();
//   // In Render():
//   m_fsr2Pass.EnsureResources(displayWidth, displayHeight, settings);
//   m_fsr2Pass.Render(cmd, colorHDR, depth, velocity, outputHDR, camera, deltaTime, settings);
class CFSR2Pass {
public:
    CFSR2Pass() = default;
    ~CFSR2Pass() = default;

    // ============================================
    // Lifecycle
    // ============================================

    // Initialize FSR2 pass (call once at startup)
    bool Initialize();

    // Shutdown and release resources
    void Shutdown();

    // ============================================
    // Resource Management
    // ============================================

    // Ensure FSR2 context and resources are ready for given display resolution
    // Call this before Render() when resolution may have changed
    void EnsureResources(uint32_t displayWidth, uint32_t displayHeight, const SFSR2Settings& settings);

    // Get render resolution for current quality mode
    // Use this to determine G-Buffer and intermediate buffer sizes
    void GetRenderResolution(uint32_t displayWidth, uint32_t displayHeight,
                            const SFSR2Settings& settings,
                            uint32_t& outRenderWidth, uint32_t& outRenderHeight) const;

    // ============================================
    // Rendering
    // ============================================

    // Execute FSR 2.0 upscaling
    // colorInput: HDR color buffer at render resolution
    // depthInput: Depth buffer at render resolution (reversed-Z)
    // velocityInput: Motion vectors at render resolution (screen-space pixels)
    // colorOutput: Output HDR buffer at display resolution
    // camera: Camera for jitter and projection info
    // deltaTimeMs: Frame time in milliseconds
    // frameIndex: Current frame index (for jitter sequence)
    // settings: FSR2 configuration from scene settings
    void Render(
        RHI::ICommandList* cmdList,
        RHI::ITexture* colorInput,
        RHI::ITexture* depthInput,
        RHI::ITexture* velocityInput,
        RHI::ITexture* colorOutput,
        const CCamera& camera,
        float deltaTimeMs,
        uint32_t frameIndex,
        const SFSR2Settings& settings
    );

    // ============================================
    // Jitter
    // ============================================

    // Get jitter offset for current frame (in pixels)
    // Apply this to projection matrix before rendering
    DirectX::XMFLOAT2 GetJitterOffset(uint32_t frameIndex) const;

    // Get jitter offset in NDC space (for applying to projection matrix)
    // jitterNDC.x = 2.0 * jitterPixels.x / renderWidth
    // jitterNDC.y = -2.0 * jitterPixels.y / renderHeight (negative for DX)
    DirectX::XMFLOAT2 GetJitterOffsetNDC(uint32_t frameIndex, uint32_t renderWidth, uint32_t renderHeight) const;

    // Get jitter phase count (number of samples in sequence)
    int32_t GetJitterPhaseCount() const;

    // ============================================
    // Configuration
    // ============================================

    // Check if FSR2 is supported (DX12 only)
    bool IsSupported() const { return CFSR2Context::IsSupported(); }

    // Check if FSR2 is ready to render
    bool IsReady() const { return m_context.IsReady(); }

    // Invalidate temporal history (call on camera cut, scene change)
    void InvalidateHistory() { m_resetHistory = true; }

private:
    CFSR2Context m_context;

    uint32_t m_displayWidth = 0;
    uint32_t m_displayHeight = 0;
    EFSR2QualityMode m_currentQualityMode = EFSR2QualityMode::Quality;
    bool m_initialized = false;
    bool m_resetHistory = false;  // Transient flag, cleared after use
};
