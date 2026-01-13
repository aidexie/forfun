#pragma once
#include "RHI/RHIPointers.h"
#include <cstdint>

// Forward declarations
namespace RHI {
    class ICommandList;
    class ITexture;
}

// ============================================
// Hi-Z Configuration Constants
// ============================================
namespace HiZConfig {
    constexpr uint32_t THREAD_GROUP_SIZE = 8;   // 8x8 threads per group
}

// ============================================
// Hi-Z Settings (exposed to editor)
// ============================================
struct SHiZSettings {
    bool enabled = true;           // Enable Hi-Z pyramid generation
    bool debugVisualize = false;   // Show specific mip in debug view
    int debugMipLevel = 0;         // Which mip to visualize (0 = full res)
};

// ============================================
// Constant buffer for Hi-Z compute shader (b0)
// ============================================
struct alignas(16) CB_HiZ {
    uint32_t srcMipSizeX;          // Source mip width
    uint32_t srcMipSizeY;          // Source mip height
    uint32_t dstMipSizeX;          // Destination mip width
    uint32_t dstMipSizeY;          // Destination mip height
    uint32_t srcMipLevel;          // Source mip level index
    uint32_t _pad[3];              // Padding to 16-byte alignment
};

// ============================================
// CHiZPass - Hierarchical-Z Depth Pyramid
// ============================================
// Builds a depth mip pyramid for accelerated screen-space ray tracing.
// Uses MAX reduction for reversed-Z (near=1, far=0) to keep closest surface.
//
// Pipeline:
//   1. Copy depth buffer to mip 0 (R32_FLOAT)
//   2. For each mip level 1 to N: downsample with MAX(2x2)
//
// Input:
//   - Depth buffer (D32_FLOAT)
//
// Output:
//   - Hi-Z pyramid texture (R32_FLOAT, full mip chain)
//
// Usage:
//   - SSR: Hierarchical ray tracing acceleration
//   - Occlusion culling: Conservative visibility tests
// ============================================
class CHiZPass {
public:
    CHiZPass() = default;
    ~CHiZPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Build Hi-Z pyramid from depth buffer
    void BuildPyramid(RHI::ICommandList* cmdList,
                      RHI::ITexture* depthBuffer,
                      uint32_t width, uint32_t height);

    // ============================================
    // Output
    // ============================================
    // Get Hi-Z pyramid texture (full mip chain)
    RHI::ITexture* GetHiZTexture() const { return m_hiZTexture.get(); }

    // Get number of mip levels in the pyramid
    uint32_t GetMipCount() const { return m_mipCount; }

    // Get pyramid dimensions
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    // ============================================
    // Settings
    // ============================================
    SHiZSettings& GetSettings() { return m_settings; }
    const SHiZSettings& GetSettings() const { return m_settings; }

private:
    void createShaders();
    void createTextures(uint32_t width, uint32_t height);
    void createSamplers();

    // Dispatch helpers
    void dispatchCopyDepth(RHI::ICommandList* cmdList, RHI::ITexture* depthBuffer);
    void dispatchBuildMip(RHI::ICommandList* cmdList, uint32_t mipLevel);

    // ============================================
    // Compute Shaders
    // ============================================
    RHI::ShaderPtr m_copyDepthCS;     // Mip 0: copy from depth buffer
    RHI::ShaderPtr m_buildMipCS;      // Mip 1+: MAX downsample

    // ============================================
    // Pipeline States
    // ============================================
    RHI::PipelineStatePtr m_copyDepthPSO;
    RHI::PipelineStatePtr m_buildMipPSO;

    // ============================================
    // Hi-Z Pyramid Texture
    // ============================================
    RHI::TexturePtr m_hiZTexture;     // R32_FLOAT with full mip chain

    // ============================================
    // Samplers
    // ============================================
    RHI::SamplerPtr m_pointSampler;   // Point sampling for depth reads

    // ============================================
    // State
    // ============================================
    SHiZSettings m_settings;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_mipCount = 0;
    bool m_initialized = false;
};
