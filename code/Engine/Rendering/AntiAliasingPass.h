#pragma once
#include "RHI/RHIPointers.h"
#include "RHI/RHIResources.h"
#include "RHI/ICommandList.h"
#include <cstdint>

struct SAntiAliasingSettings;
enum class EAntiAliasingMode : int;

// ============================================
// CAntiAliasingPass - Post-Process Anti-Aliasing
// ============================================
// Applies screen-space anti-aliasing to LDR output.
// Supports FXAA (fast, single-pass) and SMAA (higher quality, 3-pass).
//
// Pipeline Position:
//   PostProcess (Tonemapping) -> [AntiAliasing] -> Debug Lines/Grid -> Output
//
// Input:  LDR texture (R8G8B8A8_UNORM_SRGB, after tonemapping)
// Output: Anti-aliased LDR texture (same format)
// ============================================
class CAntiAliasingPass {
public:
    CAntiAliasingPass() = default;
    ~CAntiAliasingPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Applies anti-aliasing from inputTexture to outputTexture
    // Both textures should be LDR format (R8G8B8A8_UNORM_SRGB)
    void Render(RHI::ITexture* inputTexture,
                RHI::ITexture* outputTexture,
                uint32_t width, uint32_t height,
                const SAntiAliasingSettings& settings);

    // Check if AA is enabled (for pipeline optimization)
    bool IsEnabled(const SAntiAliasingSettings& settings) const;

private:
    // Algorithm implementations
    void renderFXAA(RHI::ICommandList* cmdList,
                    RHI::ITexture* input, RHI::ITexture* output,
                    uint32_t width, uint32_t height,
                    const SAntiAliasingSettings& settings);

    void renderSMAA(RHI::ICommandList* cmdList,
                    RHI::ITexture* input, RHI::ITexture* output,
                    uint32_t width, uint32_t height,
                    const SAntiAliasingSettings& settings);

    // Resource management
    void createSharedResources();
    void createFXAAResources();
    void createSMAAResources();
    void ensureSMAATextures(uint32_t width, uint32_t height);

    // ============================================
    // Shared Resources
    // ============================================
    RHI::BufferPtr m_fullscreenQuadVB;
    RHI::SamplerPtr m_linearSampler;
    RHI::SamplerPtr m_pointSampler;
    RHI::ShaderPtr m_fullscreenVS;  // Shared vertex shader

    // ============================================
    // FXAA Resources
    // ============================================
    RHI::ShaderPtr m_fxaaPS;
    RHI::PipelineStatePtr m_fxaaPSO;

    // ============================================
    // SMAA Resources (3-pass algorithm)
    // ============================================
    // Pass 1: Edge Detection
    RHI::ShaderPtr m_smaaEdgePS;
    RHI::PipelineStatePtr m_smaaEdgePSO;
    RHI::TexturePtr m_smaaEdgesTex;  // RG8 edges

    // Pass 2: Blending Weight Calculation
    RHI::ShaderPtr m_smaaBlendPS;
    RHI::PipelineStatePtr m_smaaBlendPSO;
    RHI::TexturePtr m_smaaBlendTex;  // RGBA8 blend weights

    // Pass 3: Neighborhood Blending
    RHI::ShaderPtr m_smaaNeighborPS;
    RHI::PipelineStatePtr m_smaaNeighborPSO;

    // SMAA Lookup Textures (loaded once)
    RHI::TexturePtr m_smaaAreaTex;    // Pre-computed area texture (160x560 RG8)
    RHI::TexturePtr m_smaaSearchTex;  // Pre-computed search texture (64x16 R8)

    // Cached dimensions for SMAA intermediate textures
    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;

    bool m_initialized = false;
};
