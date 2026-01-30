#pragma once
#include "RHI/RHIPointers.h"
#include <DirectXMath.h>
#include <cstdint>

// Forward declarations
namespace RHI {
    class ICommandList;
    class ITexture;
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// ============================================
// SSR Configuration Constants
// ============================================
namespace SSRConfig {
    constexpr uint32_t THREAD_GROUP_SIZE = 8;   // 8x8 threads per group
    constexpr uint32_t MAX_HIZ_MIP = 10;        // Maximum Hi-Z mip level to use
    constexpr uint32_t DEFAULT_MAX_STEPS = 64;  // Default ray march steps
}

// ============================================
// SSR Quality Preset
// ============================================
enum class ESSRQuality : int {
    Low = 0,        // Fast, 32 steps, 4 binary
    Medium = 1,     // Balanced, 48 steps, 6 binary
    High = 2,       // Quality, 64 steps, 8 binary
    Ultra = 3,      // Maximum, 96 steps, 12 binary
    Custom = 4      // User-defined settings
};

// ============================================
// SSR Algorithm Mode
// ============================================
enum class ESSRMode : int {
    SimpleLinear = 0,   // Simple linear ray march (no Hi-Z, educational)
    HiZTrace = 1,       // Single ray Hi-Z tracing (default, fast)
    Stochastic = 2,     // Multiple rays with importance sampling
    Temporal = 3        // Stochastic + temporal accumulation (best quality)
};

// ============================================
// SSR Settings (exposed to editor)
// ============================================
struct SSSRSettings {
    ESSRQuality quality = ESSRQuality::High;  // Quality preset
    ESSRMode mode = ESSRMode::Stochastic;       // Algorithm mode
    float maxDistance = 50.0f;      // Maximum ray distance (view-space)
    float thickness = 0.5f;         // Surface thickness for hit detection
    float stride = 1.0f;            // Initial step stride (pixels)
    float strideZCutoff = 100.0f;   // View-Z at which stride scales
    int maxSteps = 64;              // Maximum ray march steps
    int binarySearchSteps = 8;      // Binary search refinement steps
    float jitterOffset = 0.0f;      // Temporal jitter (0-1, animated)
    float fadeStart = 0.8f;         // Edge fade start (0-1)
    float fadeEnd = 1.0f;           // Edge fade end (0-1)
    float roughnessFade = 0.5f;     // Roughness cutoff for SSR
    float intensity = 1.0f;         // SSR intensity multiplier
    bool debugVisualize = false;    // Show SSR debug mode

    // Stochastic settings (Mode: Stochastic/Temporal)
    int numRays = 4;                // Rays per pixel (1-8)
    float brdfBias = 0.7f;          // BRDF importance sampling bias (0=uniform, 1=full GGX)

    // Stochastic SSR improvements
    bool useAdaptiveRays = true;    // Adapt ray count based on roughness
    float fireflyClampThreshold = 10.0f;  // Absolute luminance clamp
    float fireflyMultiplier = 4.0f;       // Adaptive threshold = avg * multiplier

    // Temporal settings (Mode: Temporal)
    float temporalBlend = 0.9f;     // History blend factor (0=current only, 1=history only)
    float motionThreshold = 0.01f;  // Motion rejection threshold

    // Resolution settings
    float resolutionScale = 1.0f;   // SSR render target scale (0.5 = half-res, 1.0 = full-res)

    // Apply quality preset
    void ApplyPreset(ESSRQuality preset) {
        quality = preset;
        switch (preset) {
            case ESSRQuality::Low:
                maxSteps = 32;
                binarySearchSteps = 4;
                stride = 2.0f;
                numRays = 1;
                break;
            case ESSRQuality::Medium:
                maxSteps = 48;
                binarySearchSteps = 6;
                stride = 1.5f;
                numRays = 2;
                break;
            case ESSRQuality::High:
                maxSteps = 64;
                binarySearchSteps = 8;
                stride = 1.0f;
                numRays = 4;
                break;
            case ESSRQuality::Ultra:
                maxSteps = 96;
                binarySearchSteps = 12;
                stride = 0.5f;
                numRays = 8;
                break;
            case ESSRQuality::Custom:
                // Keep current settings
                break;
        }
    }
};

// ============================================
// CB_SSR - Constant buffer for SSR compute shader (b0)
// ============================================
struct alignas(16) CB_SSR {
    DirectX::XMFLOAT4X4 proj;           // Projection matrix
    DirectX::XMFLOAT4X4 invProj;        // Inverse projection matrix
    DirectX::XMFLOAT4X4 view;           // View matrix (world to view)
    DirectX::XMFLOAT4X4 invView;        // Inverse view matrix (view to world)
    DirectX::XMFLOAT4X4 prevViewProj;   // Previous frame view-projection (temporal)
    DirectX::XMFLOAT2 screenSize;       // Full resolution (width, height)
    DirectX::XMFLOAT2 texelSize;        // 1.0 / screenSize
    float maxDistance;                   // Maximum ray distance
    float thickness;                     // Surface thickness for hit
    float stride;                        // Ray march stride
    float strideZCutoff;                 // View-Z stride scaling cutoff (reserved)
    int maxSteps;                        // Maximum ray march steps
    int binarySearchSteps;               // Binary search refinement (reserved)
    float jitterOffset;                  // Temporal jitter
    float fadeStart;                     // Edge fade start (reserved)
    float fadeEnd;                       // Edge fade end (reserved)
    float roughnessFade;                 // Roughness cutoff
    float nearZ;                         // Camera near plane
    float farZ;                          // Camera far plane
    int hiZMipCount;                     // Number of Hi-Z mip levels
    uint32_t useReversedZ;               // 0 = standard-Z, 1 = reversed-Z
    int ssrMode;                         // 0=SimpleLinear, 1=HiZ, 2=Stochastic, 3=Temporal
    int numRays;                         // Rays per pixel (stochastic/temporal)
    float brdfBias;                      // BRDF importance sampling bias
    float temporalBlend;                 // History blend factor
    float motionThreshold;               // Motion rejection threshold
    uint32_t frameIndex;                 // Frame counter for temporal jitter
    uint32_t useAdaptiveRays;            // Enable adaptive ray count
    float fireflyClampThreshold;         // Absolute luminance clamp
    float fireflyMultiplier;             // Adaptive threshold multiplier
    float _pad;                          // Padding to 16-byte alignment
};

// ============================================
// CB_SSRComposite - Constant buffer for SSR composite shader (b0)
// ============================================
struct alignas(16) CB_SSRComposite {
    DirectX::XMFLOAT2 screenSize;        // Full resolution (width, height)
    DirectX::XMFLOAT2 texelSize;         // 1.0 / screenSize
    float ssrIntensity;                  // Overall SSR intensity multiplier
    float iblFallbackWeight;             // IBL weight when SSR misses (0-1)
    float roughnessFade;                 // Roughness cutoff for reflections
    float _pad0;
    DirectX::XMFLOAT3 camPosWS;          // Camera world position
    float _pad1;
};

// ============================================
// CSSRPass - Screen-Space Reflections
// ============================================
// Implements Hi-Z accelerated screen-space reflections.
//
// Reference: "Efficient GPU Screen-Space Ray Tracing"
//            Morgan McGuire & Michael Mara (2014)
//
// Pipeline:
//   1. For each pixel: compute reflection ray in view-space
//   2. Hi-Z accelerated ray march through depth pyramid
//   3. Binary search refinement for accurate hit
//   4. Sample scene color at hit point
//   5. Apply fade based on hit confidence, edge, and roughness
//
// Input:
//   - Depth buffer (D32_FLOAT)
//   - Normal buffer (G-Buffer RT1: Normal.xyz + Roughness)
//   - Hi-Z pyramid (from CHiZPass)
//   - Scene color (HDR buffer)
//
// Output:
//   - SSR texture (R16G16B16A16_FLOAT) - reflection color + confidence
// ============================================
class CSSRPass {
public:
    CSSRPass() = default;
    ~CSSRPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Render SSR pass
    // Returns: SSR result texture (reflection color + confidence in alpha)
    void Render(RHI::ICommandList* cmdList,
                RHI::ITexture* depthBuffer,     // G-Buffer depth (full-res)
                RHI::ITexture* normalBuffer,    // G-Buffer RT1 (Normal.xyz + Roughness)
                RHI::ITexture* hiZTexture,      // Hi-Z pyramid from CHiZPass
                RHI::ITexture* sceneColor,      // HDR scene color
                uint32_t width, uint32_t height,
                uint32_t hiZMipCount,
                const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& proj,
                float nearZ, float farZ);

    // Composite SSR results into HDR buffer
    // Blends SSR reflections with existing IBL based on confidence
    void Composite(RHI::ICommandList* cmdList,
                   RHI::ITexture* hdrBuffer,          // HDR input/output
                   RHI::ITexture* worldPosMetallic,   // G-Buffer RT0
                   RHI::ITexture* normalRoughness,    // G-Buffer RT1
                   uint32_t width, uint32_t height,
                   const DirectX::XMFLOAT3& camPosWS);

    // ============================================
    // Output
    // ============================================
    // Get SSR result texture (returns black fallback if not initialized)
    RHI::ITexture* GetSSRTexture() const {
        return m_ssrResult ? m_ssrResult.get() : m_blackFallback.get();
    }

    // ============================================
    // Settings
    // ============================================
    SSSRSettings& GetSettings() { return m_settings; }
    const SSSRSettings& GetSettings() const { return m_settings; }

private:
    void createTextures(uint32_t width, uint32_t height);
    void createSamplers();
    void createFallbackTexture();
    void createBlueNoiseTexture();
    void initDescriptorSets();

    // ============================================
    // Textures
    // ============================================
    RHI::TexturePtr m_ssrResult;        // SSR reflection color + confidence
    RHI::TexturePtr m_ssrHistory;       // SSR history for temporal accumulation
    RHI::TexturePtr m_blueNoise;        // Blue noise texture for stochastic jitter
    RHI::TexturePtr m_blackFallback;    // Black fallback when SSR disabled

    // ============================================
    // Samplers
    // ============================================
    RHI::SamplerPtr m_pointSampler;     // Point sampling for depth/Hi-Z
    RHI::SamplerPtr m_linearSampler;    // Linear sampling for color

    // ============================================
    // State
    // ============================================
    SSSRSettings m_settings;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    float m_currentScale = 1.0f;        // Current resolution scale (for detecting changes)
    bool m_initialized = false;
    uint32_t m_frameIndex = 0;
    DirectX::XMMATRIX m_prevViewProj = DirectX::XMMatrixIdentity();

    // ============================================
    // Descriptor Set Resources (DX12)
    // ============================================

    // SM 5.1 shaders
    RHI::ShaderPtr m_ssrCS;
    RHI::ShaderPtr m_compositeCS;

    // SM 5.1 PSOs
    RHI::PipelineStatePtr m_ssrPSO;
    RHI::PipelineStatePtr m_compositePSO;

    // Unified compute layout (shared across all compute passes)
    RHI::IDescriptorSetLayout* m_computePerPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    bool IsDescriptorSetModeAvailable() const { return m_computePerPassLayout != nullptr && m_ssrPSO != nullptr; }
};
