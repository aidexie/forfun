#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>

struct SDepthOfFieldSettings;

namespace RHI {
    class ICommandList;
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// ============================================
// CDepthOfFieldPass - Cinematic Depth of Field
// ============================================
// Post-processing effect that simulates camera lens focus behavior.
// Objects at the focus distance appear sharp, while near/far objects blur.
//
// Algorithm (Two-Pass Separated Near/Far):
//   Pass 1: Compute CoC (Circle of Confusion) from depth buffer
//   Pass 2: Downsample + split into near/far layers (half-res)
//   Pass 3: Horizontal separable blur (near + far)
//   Pass 4: Vertical separable blur (near + far)
//   Pass 5: Bilateral upsample + composite
//
// Input:
//   - HDR color buffer (R16G16B16A16_FLOAT)
//   - Depth buffer (D32_FLOAT)
//
// Output:
//   - Focus-blurred HDR texture (full resolution)
//
// CoC Model (Artist-Friendly):
//   - focusDistance: Distance to focal plane (world units)
//   - focalRange: Depth range that remains in focus
//   - aperture: f-stop value (lower = more blur)
//   - maxBlurRadius: Maximum blur radius in pixels
// ============================================
class CDepthOfFieldPass {
public:
    CDepthOfFieldPass() = default;
    ~CDepthOfFieldPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Returns DoF-processed texture, or hdrInput unchanged if disabled/error
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          RHI::ITexture* depthBuffer,
                          float cameraNearZ, float cameraFarZ,
                          uint32_t width, uint32_t height,
                          const SDepthOfFieldSettings& settings);

    // ============================================
    // Output
    // ============================================
    RHI::ITexture* GetOutputTexture() const { return m_outputHDR.get(); }
    RHI::ITexture* GetCoCTexture() const { return m_cocBuffer.get(); }

private:
    // ============================================
    // Resources - Full Resolution
    // ============================================
    RHI::TexturePtr m_cocBuffer;        // CoC buffer (R32_FLOAT, signed: -near/+far)
    RHI::TexturePtr m_outputHDR;        // Final output (R16G16B16A16_FLOAT)

    // ============================================
    // Resources - Half Resolution
    // ============================================
    RHI::TexturePtr m_nearColor;        // Near layer color (R16G16B16A16_FLOAT)
    RHI::TexturePtr m_farColor;         // Far layer color (R16G16B16A16_FLOAT)
    RHI::TexturePtr m_nearCoc;          // Near layer CoC (R32_FLOAT)
    RHI::TexturePtr m_farCoc;           // Far layer CoC (R32_FLOAT)
    RHI::TexturePtr m_blurTempNear;     // Blur temp near (R16G16B16A16_FLOAT)
    RHI::TexturePtr m_blurTempFar;      // Blur temp far (R16G16B16A16_FLOAT)

    // ============================================
    // Samplers
    // ============================================
    RHI::BufferPtr m_vertexBuffer;      // Fullscreen quad
    RHI::SamplerPtr m_linearSampler;    // Bilinear sampling
    RHI::SamplerPtr m_pointSampler;     // Point sampling (for depth/CoC)

    // ============================================
    // State
    // ============================================
    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;
    bool m_initialized = false;

    // ============================================
    // Internal Methods
    // ============================================
    void ensureTextures(uint32_t width, uint32_t height);
    void createFullscreenQuad();

    // ============================================
    // Descriptor Set Resources (SM 5.1, DX12 only)
    // ============================================
    void initDescriptorSets();

    // SM 5.1 shaders
    RHI::ShaderPtr m_fullscreenVS_ds;
    RHI::ShaderPtr m_cocPS_ds;
    RHI::ShaderPtr m_downsampleSplitPS_ds;
    RHI::ShaderPtr m_blurHPS_ds;
    RHI::ShaderPtr m_blurVPS_ds;
    RHI::ShaderPtr m_compositePS_ds;

    // SM 5.1 PSOs
    RHI::PipelineStatePtr m_cocPSO_ds;
    RHI::PipelineStatePtr m_downsampleSplitPSO_ds;
    RHI::PipelineStatePtr m_blurHPSO_ds;
    RHI::PipelineStatePtr m_blurVPSO_ds;
    RHI::PipelineStatePtr m_compositePSO_ds;

    // Descriptor set layout and set
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_cocPSO_ds != nullptr; }

    // Individual pass execution (Descriptor Set)
    void renderCoCPass_DS(RHI::ICommandList* cmdList, RHI::ITexture* depthBuffer,
                          float nearZ, float farZ, uint32_t width, uint32_t height,
                          const SDepthOfFieldSettings& settings);
    void renderDownsampleSplitPass_DS(RHI::ICommandList* cmdList, RHI::ITexture* hdrInput,
                                       uint32_t width, uint32_t height);
    void renderBlurPass_DS(RHI::ICommandList* cmdList, bool horizontal,
                           uint32_t halfWidth, uint32_t halfHeight,
                           const SDepthOfFieldSettings& settings);
    void renderCompositePass_DS(RHI::ICommandList* cmdList, RHI::ITexture* hdrInput,
                                 uint32_t width, uint32_t height);
};
