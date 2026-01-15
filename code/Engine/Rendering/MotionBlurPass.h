#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>

struct SMotionBlurSettings;

// ============================================
// CMotionBlurPass - Camera Motion Blur
// ============================================
// Post-processing effect that blurs pixels along their velocity direction.
//
// Algorithm:
//   1. Read velocity from G-Buffer RT4 (UV-space motion vectors)
//   2. Sample HDR input along velocity direction (linear blur)
//   3. Weight samples by distance from center (tent filter)
//
// Input:
//   - HDR color buffer (R16G16B16A16_FLOAT)
//   - Velocity buffer (R16G16_FLOAT, UV-space motion vectors)
//
// Output:
//   - Motion-blurred HDR texture (full resolution)
// ============================================
class CMotionBlurPass {
public:
    CMotionBlurPass() = default;
    ~CMotionBlurPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Returns motion-blurred texture, or hdrInput unchanged if disabled/error
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          RHI::ITexture* velocityBuffer,
                          uint32_t width, uint32_t height,
                          const SMotionBlurSettings& settings);

    // ============================================
    // Output
    // ============================================
    RHI::ITexture* GetOutputTexture() const { return m_outputHDR.get(); }

private:
    // ============================================
    // Resources
    // ============================================
    RHI::TexturePtr m_outputHDR;        // Output (R16G16B16A16_FLOAT)
    RHI::BufferPtr m_vertexBuffer;      // Fullscreen quad
    RHI::SamplerPtr m_linearSampler;    // For HDR input
    RHI::SamplerPtr m_pointSampler;     // For velocity buffer

    // ============================================
    // Shaders & Pipeline
    // ============================================
    RHI::ShaderPtr m_fullscreenVS;
    RHI::ShaderPtr m_motionBlurPS;
    RHI::PipelineStatePtr m_pso;

    // ============================================
    // State
    // ============================================
    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;
    bool m_initialized = false;

    // ============================================
    // Internal Methods
    // ============================================
    void ensureOutputTexture(uint32_t width, uint32_t height);
    void createFullscreenQuad();
    void createShaders();
    void createPSO();
};
