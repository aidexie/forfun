#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>

// CPostProcessPass: Handles tone mapping and gamma correction
// Converts HDR linear space to LDR sRGB space
class CPostProcessPass {
public:
    CPostProcessPass() = default;
    ~CPostProcessPass() = default;

    // Initialize post-process resources
    bool Initialize();
    void Shutdown();

    // Render full-screen quad with tone mapping and gamma correction
    // hdrInput: HDR linear space texture (R16G16B16A16_FLOAT)
    // ldrOutput: LDR sRGB output render target (R8G8B8A8_UNORM_SRGB)
    // exposure: Exposure adjustment (0.5 = darker, 1.0 = neutral, 2.0 = brighter)
    void Render(RHI::ITexture* hdrInput,
                RHI::ITexture* ldrOutput,
                uint32_t width, uint32_t height,
                float exposure = 1.0f);

private:
    void createFullscreenQuad();
    void createShaders();
    void createPipelineState();

private:
    // Shaders
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::BufferPtr m_constantBuffer;
    RHI::SamplerPtr m_sampler;
    RHI::PipelineStatePtr m_pso;

    bool m_initialized = false;
};
