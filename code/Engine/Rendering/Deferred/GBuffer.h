#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include <cstdint>

// ============================================
// CGBuffer - G-Buffer Management for Deferred Rendering
// ============================================
// Manages 5 render targets + depth buffer for deferred shading.
//
// G-Buffer Layout:
//   RT0 (R16G16B16A16_FLOAT): WorldPosition.xyz + Metallic.a
//   RT1 (R16G16B16A16_FLOAT): Normal.xyz + Roughness.a
//   RT2 (R8G8B8A8_UNORM_SRGB): Albedo.rgb + AO.a
//   RT3 (R16G16B16A16_FLOAT): Emissive.rgb + MaterialID.a
//   RT4 (R16G16_FLOAT): Velocity.xy (for TAA/MotionBlur)
//   Depth (D32_FLOAT): Scene depth
//
// Memory Budget @ 1080p: ~72 MB
// ============================================
class CGBuffer
{
public:
    // G-Buffer render target indices
    enum EGBufferRT : uint32_t {
        RT_WorldPosMetallic = 0,    // RT0: WorldPosition.xyz + Metallic
        RT_NormalRoughness = 1,      // RT1: Normal.xyz + Roughness
        RT_AlbedoAO = 2,             // RT2: Albedo.rgb + AO
        RT_EmissiveMaterialID = 3,   // RT3: Emissive.rgb + MaterialID
        RT_Velocity = 4,             // RT4: Velocity.xy
        RT_Count = 5
    };

    CGBuffer() = default;
    ~CGBuffer() = default;

    // ============================================
    // Lifecycle
    // ============================================
    // Initialize G-Buffer resources for given resolution
    bool Initialize(uint32_t width, uint32_t height);

    // Resize G-Buffer (recreates all textures)
    void Resize(uint32_t width, uint32_t height);

    // Release all resources
    void Shutdown();

    // ============================================
    // Accessors
    // ============================================
    // Get render target by index
    RHI::ITexture* GetRenderTarget(EGBufferRT index) const;

    // Get all render targets as array (for SetRenderTargets)
    void GetRenderTargets(RHI::ITexture** outRTs, uint32_t& outCount) const;

    // Get depth buffer
    RHI::ITexture* GetDepthBuffer() const { return m_depth.get(); }

    // Get dimensions
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    // ============================================
    // Convenience Accessors
    // ============================================
    RHI::ITexture* GetWorldPosMetallic() const { return m_renderTargets[RT_WorldPosMetallic].get(); }
    RHI::ITexture* GetNormalRoughness() const { return m_renderTargets[RT_NormalRoughness].get(); }
    RHI::ITexture* GetAlbedoAO() const { return m_renderTargets[RT_AlbedoAO].get(); }
    RHI::ITexture* GetEmissiveMaterialID() const { return m_renderTargets[RT_EmissiveMaterialID].get(); }
    RHI::ITexture* GetVelocity() const { return m_renderTargets[RT_Velocity].get(); }

private:
    void createRenderTargets(uint32_t width, uint32_t height);

    // G-Buffer render targets
    RHI::TexturePtr m_renderTargets[RT_Count];

    // Depth buffer (D32_FLOAT with SRV for deferred lighting)
    RHI::TexturePtr m_depth;

    // Dimensions
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
