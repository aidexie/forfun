#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>
#include <string>

struct SColorGradingSettings;

namespace RHI {
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

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
#ifndef FF_LEGACY_BINDING_DISABLED
    void createShaders();
    void createPipelineState();
#endif
    void createNeutralLUT();
    bool loadLUT(const std::string& cubePath);

private:
#ifndef FF_LEGACY_BINDING_DISABLED
    // Legacy Shaders (SM 5.0)
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;
#endif

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::BufferPtr m_constantBuffer;
    RHI::BufferPtr m_dummyExposureBuffer;  // Dummy buffer for t3 when no exposure buffer
    RHI::SamplerPtr m_sampler;
#ifndef FF_LEGACY_BINDING_DISABLED
    RHI::PipelineStatePtr m_pso;
#endif

    // Color Grading LUT
    RHI::TexturePtr m_neutralLUT;       // Identity LUT (32x32x32)
    RHI::TexturePtr m_customLUT;        // User-loaded LUT
    std::string m_cachedLUTPath;        // Track LUT changes

    bool m_initialized = false;

    // ============================================
    // Descriptor Set Resources (SM 5.1, DX12 only)
    // ============================================
    void initDescriptorSets();

    // SM 5.1 shaders
    RHI::ShaderPtr m_vs_ds;
    RHI::ShaderPtr m_ps_ds;

    // SM 5.1 PSO
    RHI::PipelineStatePtr m_pso_ds;

    // Descriptor set layout and set
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

#ifndef FF_LEGACY_BINDING_DISABLED
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_pso_ds != nullptr; }
#endif
};
