#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>

struct SBloomSettings;

namespace RHI {
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// CBloomPass: HDR Bloom post-processing effect using Dual Kawase Blur
// Creates a soft glow effect from bright pixels in the HDR buffer
//
// Algorithm:
// 1. Threshold pass: Extract bright pixels (> threshold) at half resolution
// 2. Downsample chain: 5 levels of progressive blur (Kawase 5-tap)
// 3. Upsample chain: Reconstruct with tent filter, accumulating glow
//
// Output: Half-resolution bloom texture for compositing in PostProcessPass
class CBloomPass {
public:
    CBloomPass() = default;
    ~CBloomPass() = default;

    // Initialize bloom pass resources
    bool Initialize();
    void Shutdown();

    // Render bloom effect
    // Returns bloom texture (half resolution) or nullptr if disabled
    // hdrInput: Full resolution HDR input texture
    // width/height: Full resolution dimensions
    // settings: Bloom configuration (threshold, intensity, scatter)
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          uint32_t width, uint32_t height,
                          const SBloomSettings& settings);

    // Get the final bloom result texture (half resolution)
    RHI::ITexture* GetBloomTexture() const { return m_mipChain[0].get(); }

private:
    static constexpr int kMipCount = 5;

    // Mip chain: R11G11B10_FLOAT for bandwidth efficiency
    // Mip[0] = half res (threshold output), Mip[1-4] = successive halves
    RHI::TexturePtr m_mipChain[kMipCount];
    uint32_t m_mipWidth[kMipCount] = {};
    uint32_t m_mipHeight[kMipCount] = {};

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::SamplerPtr m_linearSampler;

    // Fallback black texture for when bloom is disabled
    RHI::TexturePtr m_blackTexture;

    // Cached dimensions to detect resize
    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;

    bool m_initialized = false;

    void ensureMipChain(uint32_t width, uint32_t height);
    void createFullscreenQuad();
    void createBlackTexture();

    // ============================================
    // Descriptor Set Resources (SM 5.1, DX12 only)
    // ============================================
    void initDescriptorSets();

    // SM 5.1 shaders
    RHI::ShaderPtr m_fullscreenVS_ds;
    RHI::ShaderPtr m_thresholdPS_ds;
    RHI::ShaderPtr m_downsamplePS_ds;
    RHI::ShaderPtr m_upsamplePS_ds;

    // SM 5.1 PSOs
    RHI::PipelineStatePtr m_thresholdPSO_ds;
    RHI::PipelineStatePtr m_downsamplePSO_ds;
    RHI::PipelineStatePtr m_upsamplePSO_ds;
    RHI::PipelineStatePtr m_upsampleBlendPSO_ds;

    // Descriptor set layout and set
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_thresholdPSO_ds != nullptr; }
};
