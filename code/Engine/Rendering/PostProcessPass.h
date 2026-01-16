#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>
#include <string>

struct SColorGradingSettings;

// CPostProcessPass: Handles tone mapping, color grading, and gamma correction
// Converts HDR linear space to LDR sRGB space
class CPostProcessPass {
public:
    CPostProcessPass() = default;
    ~CPostProcessPass() = default;

    // Initialize post-process resources
    bool Initialize();
    void Shutdown();

    // Render full-screen quad with tone mapping, color grading, and gamma correction
    // hdrInput: HDR linear space texture (R16G16B16A16_FLOAT)
    // bloomTexture: Bloom texture (half resolution, can be nullptr)
    // ldrOutput: LDR sRGB output render target (R8G8B8A8_UNORM_SRGB)
    // exposure: Exposure adjustment (0.5 = darker, 1.0 = neutral, 2.0 = brighter)
    // exposureBuffer: GPU buffer containing exposure value (overrides exposure param if provided)
    // bloomIntensity: Bloom contribution multiplier (0 = no bloom)
    // colorGrading: Color grading settings (can be nullptr to disable)
    // colorGradingEnabled: Whether color grading is enabled (from ShowFlags)
    void Render(RHI::ITexture* hdrInput,
                RHI::ITexture* bloomTexture,
                RHI::ITexture* ldrOutput,
                uint32_t width, uint32_t height,
                float exposure = 1.0f,
                RHI::IBuffer* exposureBuffer = nullptr,
                float bloomIntensity = 1.0f,
                const SColorGradingSettings* colorGrading = nullptr,
                bool colorGradingEnabled = false);

private:
    void createFullscreenQuad();
    void createShaders();
    void createPipelineState();
    void createNeutralLUT();
    bool loadLUT(const std::string& cubePath);

private:
    // Shaders
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::BufferPtr m_constantBuffer;
    RHI::BufferPtr m_dummyExposureBuffer;  // Dummy buffer for t3 when no exposure buffer
    RHI::SamplerPtr m_sampler;
    RHI::PipelineStatePtr m_pso;

    // Color Grading LUT
    RHI::TexturePtr m_neutralLUT;       // Identity LUT (32x32x32)
    RHI::TexturePtr m_customLUT;        // User-loaded LUT
    std::string m_cachedLUTPath;        // Track LUT changes

    bool m_initialized = false;
};
