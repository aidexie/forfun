#pragma once

#include "../RHIResources.h"
#include <memory>

namespace RHI {
namespace DX12 {

class CDX12CommandList;

// ============================================
// DX12 GenerateMips Pass
// ============================================
// Compute-based mipmap generation for 2D textures and texture arrays
// Supports explicit gamma handling for sRGB textures
//
// Usage:
//   pass.Initialize();
//   pass.Execute(cmdList, texture);
//   pass.Shutdown();

class CDX12GenerateMipsPass {
public:
    CDX12GenerateMipsPass() = default;
    ~CDX12GenerateMipsPass();

    // Non-copyable
    CDX12GenerateMipsPass(const CDX12GenerateMipsPass&) = delete;
    CDX12GenerateMipsPass& operator=(const CDX12GenerateMipsPass&) = delete;

    // Initialize compute shaders and PSOs
    bool Initialize();

    // Release all resources
    void Shutdown();

    // Generate mipmaps for a texture
    // Texture must have UnorderedAccess flag and mipLevels > 1
    void Execute(CDX12CommandList* cmdList, ITexture* texture);

    // Check if initialized
    bool IsInitialized() const { return m_initialized; }

private:
    // Constant buffer structure for mip generation
    struct CB_GenerateMips {
        uint32_t srcMipSizeX;
        uint32_t srcMipSizeY;
        uint32_t dstMipSizeX;
        uint32_t dstMipSizeY;
        uint32_t srcMipLevel;
        uint32_t arraySlice;
        uint32_t isSRGB;
        uint32_t padding;
    };

    bool m_initialized = false;

    // Shaders
    std::unique_ptr<IShader> m_cs2D;       // For Texture2D
    std::unique_ptr<IShader> m_csArray;    // For Texture2DArray / Cubemaps

    // Pipeline states
    std::unique_ptr<IPipelineState> m_pso2D;
    std::unique_ptr<IPipelineState> m_psoArray;

    // Sampler for bilinear filtering
    std::unique_ptr<ISampler> m_sampler;
};

} // namespace DX12
} // namespace RHI
