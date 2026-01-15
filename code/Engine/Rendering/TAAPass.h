#pragma once
#include "RHI/RHIPointers.h"
#include <DirectXMath.h>
#include <cstdint>

namespace RHI {
    class ICommandList;
    class ITexture;
}

// ============================================
// ETAAAlgorithm - TAA Algorithm Selection
// ============================================
// Progressive levels from simplest to most complex.
// Each level builds on the previous, adding more features.
enum class ETAAAlgorithm : int {
    Off = 0,
    Basic = 1,             // Simple blend (heavy ghosting)
    NeighborhoodClamp = 2, // Min/max AABB clamping
    VarianceClip = 3,      // Variance clipping + YCoCg
    CatmullRom = 4,        // + Catmull-Rom history sampling
    MotionRejection = 5,   // + Motion/depth rejection
    Production = 6         // + Sharpening (full quality)
};

// ============================================
// STAASettings - TAA Configuration
// ============================================
struct STAASettings {
    ETAAAlgorithm algorithm = ETAAAlgorithm::Production;

    // Core parameters
    float history_blend = 0.95f;            // History weight (0.8-0.98)

    // Level 3+ (Variance Clipping)
    float variance_clip_gamma = 1.0f;       // Variance clip box scale (0.75-1.5)

    // Level 5+ (Motion Rejection)
    float velocity_rejection_scale = 0.1f;  // Scale for velocity-based rejection
    float depth_rejection_scale = 100.0f;   // Scale for depth-based rejection

    // Level 6 (Sharpening)
    bool sharpening_enabled = true;
    float sharpening_strength = 0.2f;       // 0.0-0.5 recommended

    // Jitter settings
    uint32_t jitter_samples = 8;            // 4, 8, or 16
};

// ============================================
// CB_TAA - Constant Buffer for TAA Shader
// ============================================
struct alignas(16) CB_TAA {
    DirectX::XMFLOAT4X4 inv_view_proj;
    DirectX::XMFLOAT4X4 prev_view_proj;
    DirectX::XMFLOAT2 screen_size;
    DirectX::XMFLOAT2 texel_size;
    DirectX::XMFLOAT2 jitter_offset;
    DirectX::XMFLOAT2 prev_jitter_offset;
    float history_blend;
    float variance_clip_gamma;
    float velocity_rejection_scale;
    float depth_rejection_scale;
    uint32_t algorithm;
    uint32_t frame_index;
    uint32_t flags;  // Bit 0: first frame (no history)
    float _pad;
};

// ============================================
// CB_TAASharpen - Constant Buffer for Sharpening
// ============================================
struct alignas(16) CB_TAASharpen {
    DirectX::XMFLOAT2 screen_size;
    DirectX::XMFLOAT2 texel_size;
    float sharpen_strength;
    float _pad[3];
};

// ============================================
// CTAAPass - Temporal Anti-Aliasing Pass
// ============================================
// Implements TAA with 6 progressive algorithm levels.
// Pipeline Position: After DeferredLighting, before PostProcess (HDR space)
class CTAAPass {
public:
    CTAAPass() = default;
    ~CTAAPass() = default;

    bool Initialize();
    void Shutdown();

    void Render(RHI::ICommandList* cmd_list,
                RHI::ITexture* current_color,
                RHI::ITexture* velocity_buffer,
                RHI::ITexture* depth_buffer,
                uint32_t width, uint32_t height,
                const DirectX::XMMATRIX& view_proj,
                const DirectX::XMMATRIX& prev_view_proj,
                const DirectX::XMFLOAT2& jitter_offset,
                const DirectX::XMFLOAT2& prev_jitter_offset);

    void InvalidateHistory();

    RHI::ITexture* GetOutput() const { return m_output.get(); }

    STAASettings& GetSettings() { return m_settings; }
    const STAASettings& GetSettings() const { return m_settings; }

private:
    void createShaders();
    void createTextures(uint32_t width, uint32_t height);
    void ensureTextures(uint32_t width, uint32_t height);

    // Shaders & PSOs
    RHI::ShaderPtr m_taa_cs;
    RHI::ShaderPtr m_sharpen_cs;
    RHI::PipelineStatePtr m_taa_pso;
    RHI::PipelineStatePtr m_sharpen_pso;

    // Textures
    RHI::TexturePtr m_history[2];  // Double-buffered history
    RHI::TexturePtr m_output;
    RHI::TexturePtr m_sharpen_output;

    // Samplers
    RHI::SamplerPtr m_linear_sampler;
    RHI::SamplerPtr m_point_sampler;

    // State
    STAASettings m_settings;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_frame_index = 0;
    uint32_t m_history_index = 0;
    bool m_history_valid = false;
    bool m_initialized = false;
};
