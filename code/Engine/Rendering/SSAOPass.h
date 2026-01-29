#pragma once
#include "RHI/RHIPointers.h"
#include <DirectXMath.h>
#include <cstdint>

// Forward declarations
namespace RHI {
    class ICommandList;
    class IPipelineState;
    class ITexture;
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// GTAO configuration constants
namespace SSAOConfig {
    constexpr uint32_t THREAD_GROUP_SIZE = 8;   // 8x8 threads per group
    constexpr uint32_t NOISE_TEXTURE_SIZE = 4;  // 4x4 noise texture
    constexpr uint32_t DEFAULT_SLICES = 3;      // Default number of direction slices
    constexpr uint32_t DEFAULT_STEPS = 4;       // Default steps per direction
    constexpr uint32_t MAX_BLUR_RADIUS = 4;     // Maximum bilateral blur radius
    constexpr uint32_t MIN_SLICES = 2;          // Minimum slices (fast mode)
    constexpr uint32_t MAX_SLICES = 16;          // Maximum slices (quality mode)
}

// SSAO Algorithm selection
enum class ESSAOAlgorithm : int {
    GTAO = 0,    // Ground Truth AO (most accurate, UE5/Unity HDRP)
    HBAO = 1,    // Horizon-Based AO (NVIDIA, good balance)
    Crytek = 2,  // Original SSAO (Crysis 2007, classic)

    // Debug visualization modes (100+)
    Debug_RawDepth = 100,      // Raw depth buffer value [0,1]
    Debug_LinearDepth = 101,   // Linearized view-space Z
    Debug_ViewPosZ = 102,      // View-space position.z (check sign)
    Debug_ViewNormalZ = 103,   // View-space normal.z (facing camera = white)
    Debug_SampleDiff = 104     // Sample reconstruction accuracy
};

// SSAO Settings (exposed to editor, serialized with scene)
struct SSSAOSettings {
    ESSAOAlgorithm algorithm = ESSAOAlgorithm::GTAO;  // Algorithm selection
    float radius = 0.5f;            // View-space AO radius
    float intensity = 1.5f;         // AO strength multiplier
    float falloffStart = 0.2f;      // Distance falloff start (0.0-1.0 of radius)
    float falloffEnd = 1.0f;        // Distance falloff end
    float depthSigma = 0.1f;        // Bilateral blur depth threshold
    float thicknessHeuristic = 0.1f;// Thin object heuristic
    int numSlices = 10;              // Number of direction slices (2-4)
    int numSteps = 20;               // Steps per direction (4-8)
    int blurRadius = 2;             // Bilateral blur radius (1-4)
};

// Constant buffer for SSAO compute shader (b0)
struct alignas(16) CB_SSAO {
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT4X4 invProj;
    DirectX::XMFLOAT4X4 view;           // For world→view normal transform
    DirectX::XMFLOAT2 texelSize;        // 1.0 / resolution (half-res)
    DirectX::XMFLOAT2 noiseScale;       // resolution / 4.0 (noise tiling)
    float radius;                       // AO radius in view-space units
    float intensity;                    // AO strength multiplier
    float falloffStart;                 // Distance falloff start (0.0-1.0)
    float falloffEnd;                   // Distance falloff end (1.0)
    int numSlices;                      // Number of direction slices (2-4)
    int numSteps;                       // Steps per direction (4-8)
    float thicknessHeuristic;           // Thin object heuristic threshold
    int algorithm;                      // 0=GTAO, 1=HBAO, 2=Crytek
    uint32_t useReversedZ;              // 0 = standard-Z, 1 = reversed-Z
    float _pad[3];                      // Padding to 16-byte alignment
};

// Constant buffer for bilateral blur (b0)
struct alignas(16) CB_SSAOBlur {
    DirectX::XMFLOAT2 blurDirection;  // (1,0) horizontal, (0,1) vertical
    DirectX::XMFLOAT2 texelSize;
    float depthSigma;
    int blurRadius;
    float _pad[2];
};

// Constant buffer for bilateral upsample (b0)
struct alignas(16) CB_SSAOUpsample {
    DirectX::XMFLOAT2 fullResTexelSize;
    DirectX::XMFLOAT2 halfResTexelSize;
    float depthSigma;
    float _pad[3];
};

// ============================================
// CSSAOPass - Screen-Space Ambient Occlusion
// ============================================
// Implements GTAO (Ground Truth Ambient Occlusion) at half resolution
// with bilateral blur and edge-preserving upsample.
//
// Reference: "Practical Real-Time Strategies for Accurate Indirect Occlusion"
//            Jorge Jimenez et al. (2016)
//
// Pipeline:
//   1. GTAO compute at half-res (depth + normal → raw AO)
//   2. Horizontal bilateral blur (half-res)
//   3. Vertical bilateral blur (half-res)
//   4. Bilateral upsample to full-res
//
// Input:
//   - Depth buffer (D32_FLOAT)
//   - Normal buffer (G-Buffer RT1: Normal.xyz + Roughness)
//
// Output:
//   - SSAO texture (R8_UNORM, full resolution)
// ============================================
class CSSAOPass {
public:
    CSSAOPass() = default;
    ~CSSAOPass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // Resize textures when viewport changes
    void Resize(uint32_t width, uint32_t height);

    // ============================================
    // Rendering
    // ============================================
    // Render SSAO pass (computes at half-res, outputs full-res)
    void Render(RHI::ICommandList* cmdList,
                RHI::ITexture* depthBuffer,     // G-Buffer depth (full-res)
                RHI::ITexture* normalBuffer,    // G-Buffer RT1 (full-res)
                uint32_t width, uint32_t height,
                const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& proj,
                float nearZ, float farZ);

    // ============================================
    // Output
    // ============================================
    // Get final SSAO texture for lighting pass (returns white texture if not initialized)
    RHI::ITexture* GetSSAOTexture() const {
        return m_ssaoFinal ? m_ssaoFinal.get() : m_whiteFallback.get();
    }

    // ============================================
    // Settings
    // ============================================
    SSSAOSettings& GetSettings() { return m_settings; }
    const SSSAOSettings& GetSettings() const { return m_settings; }

private:
    void createShaders();
    void createTextures(uint32_t fullWidth, uint32_t fullHeight);
    void createNoiseTexture();
    void createWhiteFallbackTexture();
    void createSamplers();

    // Dispatch helpers (Descriptor Set path)
    void dispatchDownsampleDepth_DS(RHI::ICommandList* cmdList, RHI::ITexture* depthFullRes);
    void dispatchSSAO_DS(RHI::ICommandList* cmdList,
                         RHI::ITexture* depthBuffer,
                         RHI::ITexture* normalBuffer,
                         const DirectX::XMMATRIX& view,
                         const DirectX::XMMATRIX& proj,
                         float nearZ, float farZ);
    void dispatchBlurH_DS(RHI::ICommandList* cmdList);
    void dispatchBlurV_DS(RHI::ICommandList* cmdList);
    void dispatchBlur_DS(RHI::ICommandList* cmdList,
                         RHI::IPipelineState* pso,
                         RHI::ITexture* inputAO,
                         RHI::ITexture* outputAO,
                         const DirectX::XMFLOAT2& direction);
    void dispatchUpsample_DS(RHI::ICommandList* cmdList, RHI::ITexture* depthFullRes);

#ifndef FF_LEGACY_BINDING_DISABLED
    // Dispatch helpers (Legacy path)
    void dispatchDownsampleDepth(RHI::ICommandList* cmdList, RHI::ITexture* depthFullRes);
    void dispatchSSAO(RHI::ICommandList* cmdList,
                      RHI::ITexture* depthBuffer,
                      RHI::ITexture* normalBuffer,
                      const DirectX::XMMATRIX& view,
                      const DirectX::XMMATRIX& proj,
                      float nearZ, float farZ);

    void dispatchBlurH(RHI::ICommandList* cmdList);
    void dispatchBlurV(RHI::ICommandList* cmdList);
    void dispatchBlur(RHI::ICommandList* cmdList,
                      RHI::IPipelineState* pso,
                      RHI::ITexture* inputAO,
                      RHI::ITexture* outputAO,
                      const DirectX::XMFLOAT2& direction);
    void dispatchUpsample(RHI::ICommandList* cmdList, RHI::ITexture* depthFullRes);
#endif // FF_LEGACY_BINDING_DISABLED

    // ============================================
    // Compute Shaders
    // ============================================
    RHI::ShaderPtr m_ssaoCS;        // HBAO main compute (half-res)
    RHI::ShaderPtr m_blurHCS;       // Horizontal bilateral blur
    RHI::ShaderPtr m_blurVCS;       // Vertical bilateral blur
    RHI::ShaderPtr m_upsampleCS;    // Bilateral upsample to full-res
    RHI::ShaderPtr m_downsampleCS;  // Depth downsample for bilateral upsample

    // ============================================
    // Pipeline States
    // ============================================
    RHI::PipelineStatePtr m_ssaoPSO;
    RHI::PipelineStatePtr m_blurHPSO;
    RHI::PipelineStatePtr m_blurVPSO;
    RHI::PipelineStatePtr m_upsamplePSO;
    RHI::PipelineStatePtr m_downsamplePSO;

    // ============================================
    // Half-Resolution Textures
    // ============================================
    RHI::TexturePtr m_ssaoRaw;          // Raw SSAO output (half-res, noisy)
    RHI::TexturePtr m_ssaoBlurTemp;     // Temp for horizontal blur (half-res)
    RHI::TexturePtr m_ssaoHalfBlurred;  // After vertical blur (half-res)
    RHI::TexturePtr m_depthHalfRes;     // Downsampled depth for upsample

    // ============================================
    // Full-Resolution Output
    // ============================================
    RHI::TexturePtr m_ssaoFinal;        // Final upsampled SSAO (full-res)

    // ============================================
    // Noise Texture & Samplers
    // ============================================
    RHI::TexturePtr m_noiseTexture;     // 4x4 random rotation vectors
    RHI::SamplerPtr m_pointSampler;     // Point sampling for depth/AO
    RHI::SamplerPtr m_linearSampler;    // Linear sampling for upsample
    RHI::TexturePtr m_whiteFallback;    // 1x1 white texture (used when SSAO disabled)

    // ============================================
    // State
    // ============================================
    SSSAOSettings m_settings;
    uint32_t m_fullWidth = 0;
    uint32_t m_fullHeight = 0;
    uint32_t m_halfWidth = 0;
    uint32_t m_halfHeight = 0;
    bool m_initialized = false;

    // ============================================
    // Descriptor Set Resources (SM 5.1, DX12 only)
    // ============================================
    void initDescriptorSets();

    // SM 5.1 shaders
    RHI::ShaderPtr m_ssaoCS_ds;
    RHI::ShaderPtr m_blurHCS_ds;
    RHI::ShaderPtr m_blurVCS_ds;
    RHI::ShaderPtr m_upsampleCS_ds;
    RHI::ShaderPtr m_downsampleCS_ds;

    // SM 5.1 PSOs
    RHI::PipelineStatePtr m_ssaoPSO_ds;
    RHI::PipelineStatePtr m_blurHPSO_ds;
    RHI::PipelineStatePtr m_blurVPSO_ds;
    RHI::PipelineStatePtr m_upsamplePSO_ds;
    RHI::PipelineStatePtr m_downsamplePSO_ds;

    // Unified compute layout (shared across all compute passes)
    RHI::IDescriptorSetLayout* m_computePerPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    bool IsDescriptorSetModeAvailable() const { return m_computePerPassLayout != nullptr && m_ssaoPSO_ds != nullptr; }
};
