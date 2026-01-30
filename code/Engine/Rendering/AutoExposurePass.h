#pragma once
#include "RHI/RHIPointers.h"
#include <DirectXMath.h>
#include <cstdint>

namespace RHI {
    class ICommandList;
    class ITexture;
    class IDescriptorSet;
    class IDescriptorSetLayout;
}

struct SAutoExposureSettings;

// ============================================
// Auto Exposure Configuration
// ============================================
namespace AutoExposureConfig {
    constexpr uint32_t HISTOGRAM_BINS = 256;
    constexpr uint32_t HISTOGRAM_THREAD_GROUP_SIZE = 16;  // 16x16 threads per group
    constexpr float MIN_LOG_LUMINANCE = -8.0f;  // log2(1/256) ~ very dark
    constexpr float MAX_LOG_LUMINANCE = 4.0f;   // log2(16) ~ very bright
}

// ============================================
// Constant Buffers
// ============================================

// Unified constant buffer for both histogram and adaptation passes (b0)
// Both shaders use the same cbuffer layout to avoid register conflicts
struct alignas(16) CB_AutoExposure {
    DirectX::XMFLOAT2 screenSize;
    DirectX::XMFLOAT2 rcpScreenSize;
    float minLogLuminance;
    float maxLogLuminance;
    float centerWeight;
    float deltaTime;
    float minExposure;
    float maxExposure;
    float adaptSpeedUp;
    float adaptSpeedDown;
    float exposureCompensation;
    float _pad0;
    float _pad1;
    float _pad2;
};

// Constant buffer for debug visualization (b0)
struct alignas(16) CB_HistogramDebug {
    DirectX::XMFLOAT2 screenSize;
    DirectX::XMFLOAT2 histogramPos;     // Bottom-left corner position
    DirectX::XMFLOAT2 histogramSize;    // Width x Height in pixels
    float currentExposure;
    float targetExposure;
    float minLogLuminance;
    float maxLogLuminance;
    float _pad[2];
};

// ============================================
// CAutoExposurePass - Histogram-Based Auto Exposure
// ============================================
// Implements automatic exposure adjustment based on scene luminance.
//
// Algorithm:
// 1. Build luminance histogram (256 bins, log scale)
// 2. Calculate average luminance with center weighting
// 3. Compute target exposure from average luminance
// 4. Smooth adaptation with asymmetric speeds (dark->bright faster)
//
// Reference: "Automatic Exposure" - Krzysztof Narkowicz
//            https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/
//
// Pipeline:
//   1. Histogram Pass (compute): HDR buffer -> 256-bin histogram
//   2. Adaptation Pass (compute): Histogram -> target exposure -> smoothed exposure
//   3. Debug Pass (pixel, optional): Render histogram overlay
//
// Input:
//   - HDR scene buffer (R16G16B16A16_FLOAT or R11G11B10_FLOAT)
//
// Output:
//   - Exposure multiplier (float) for use in tonemapping
// ============================================
class CAutoExposurePass {
public:
    CAutoExposurePass() = default;
    ~CAutoExposurePass() = default;

    // ============================================
    // Lifecycle
    // ============================================
    bool Initialize();
    void Shutdown();

    // ============================================
    // Rendering
    // ============================================
    // Compute exposure from HDR scene luminance
    void Render(RHI::ICommandList* cmdList,
                RHI::ITexture* hdrInput,
                uint32_t width, uint32_t height,
                float deltaTime,
                const SAutoExposureSettings& settings);

    // Render debug histogram overlay (call after tonemapping, on LDR target)
    void RenderDebugOverlay(RHI::ICommandList* cmdList,
                           RHI::ITexture* renderTarget,
                           uint32_t width, uint32_t height);

    // ============================================
    // Output
    // ============================================
    // Get current exposure multiplier (1.0 = no adjustment)
    // NOTE: This returns CPU-side cached value. For GPU-only path, use GetExposureBuffer()
    float GetExposure() const { return m_currentExposure; }

    // Get exposure buffer for GPU-only path (bind directly to tonemapping shader)
    // Buffer contains: [0] = current exposure, [1] = target exposure
    RHI::IBuffer* GetExposureBuffer() const { return m_exposureBuffer.get(); }

    // Get histogram data for external debug UI (256 bins)
    const uint32_t* GetHistogramData() const { return m_histogramCache; }

    // Check if descriptor set mode is available (DX12)
    bool IsDescriptorSetModeAvailable() const { return m_computePerPassLayout != nullptr && m_histogramPSO_ds != nullptr; }

private:
    bool createShaders();
    void createBuffers();
    void createSamplers();
    void createDebugResources();

    void dispatchHistogram(RHI::ICommandList* cmdList,
                          RHI::ITexture* hdrInput,
                          uint32_t width, uint32_t height,
                          const SAutoExposureSettings& settings);

    void dispatchAdaptation(RHI::ICommandList* cmdList,
                           float deltaTime,
                           uint32_t pixelCount,
                           const SAutoExposureSettings& settings);

    void readbackHistogram(RHI::ICommandList* cmdList);

    // Descriptor set path (DX12)
    void initDescriptorSets();
    void dispatchHistogram_DS(RHI::ICommandList* cmdList,
                              RHI::ITexture* hdrInput,
                              uint32_t width, uint32_t height,
                              const SAutoExposureSettings& settings);
    void dispatchAdaptation_DS(RHI::ICommandList* cmdList,
                               float deltaTime,
                               uint32_t pixelCount,
                               const SAutoExposureSettings& settings);

    // ============================================
    // Compute Shaders
    // ============================================
    RHI::ShaderPtr m_histogramCS;       // Build luminance histogram
    RHI::ShaderPtr m_adaptationCS;      // Calculate and adapt exposure

    // ============================================
    // Debug Visualization Shaders
    // ============================================
    RHI::ShaderPtr m_debugVS;           // Fullscreen quad vertex shader
    RHI::ShaderPtr m_debugPS;           // Histogram bar rendering

    // ============================================
    // Pipeline States
    // ============================================
    RHI::PipelineStatePtr m_histogramPSO;
    RHI::PipelineStatePtr m_adaptationPSO;
    RHI::PipelineStatePtr m_debugPSO;

    // ============================================
    // Buffers
    // ============================================
    RHI::BufferPtr m_histogramBuffer;       // UAV: 256 uint32_t bins
    RHI::BufferPtr m_exposureBuffer;        // UAV: 2 floats (current, target)
    RHI::BufferPtr m_histogramReadback;     // Staging buffer for CPU readback
    RHI::BufferPtr m_exposureReadback;      // Staging buffer for exposure readback

    // ============================================
    // State
    // ============================================
    float m_currentExposure = 1.0f;
    float m_targetExposure = 1.0f;
    uint32_t m_histogramCache[AutoExposureConfig::HISTOGRAM_BINS] = {};
    bool m_initialized = false;
    bool m_firstFrame = true;

    // ============================================
    // Descriptor Set Resources (DX12)
    // ============================================
    RHI::IDescriptorSetLayout* m_computePerPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    RHI::ShaderPtr m_histogramCS_ds;
    RHI::ShaderPtr m_adaptationCS_ds;

    RHI::PipelineStatePtr m_histogramPSO_ds;
    RHI::PipelineStatePtr m_adaptationPSO_ds;

    RHI::SamplerPtr m_pointSampler;
    RHI::SamplerPtr m_linearSampler;
};
