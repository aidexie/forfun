#pragma once
#include <string>
#include <DirectXMath.h>

// ============================================
// Diffuse GI Mode - 漫反射全局光照模式
// ============================================
enum class EDiffuseGIMode : int
{
    VolumetricLightmap = 0,  // 使用 Volumetric Lightmap（需要烘焙数据）
    GlobalIBL = 1,           // 使用 Global IBL（Skybox Irradiance）
    None = 2,                // 禁用漫反射 GI（烘焙首次/调试用）
    Lightmap2D = 3           // 使用 2D Lightmap（UV2 纹理采样）
};

// ============================================
// G-Buffer Debug Visualization Mode
// ============================================
#define GBUFFER_DEBUG_MODES \
    X(None, "None") \
    X(WorldPosition, "World Position") \
    X(Normal, "Normal") \
    X(Albedo, "Albedo") \
    X(Metallic, "Metallic") \
    X(Roughness, "Roughness") \
    X(AO, "AO") \
    X(Emissive, "Emissive") \
    X(MaterialID, "Material ID") \
    X(Velocity, "Velocity") \
    X(Depth, "Depth") \
    X(SSAO, "SSAO") \
    X(HiZ_Mip0, "Hi-Z Mip 0") \
    X(HiZ_Mip1, "Hi-Z Mip 1") \
    X(HiZ_Mip2, "Hi-Z Mip 2") \
    X(HiZ_Mip3, "Hi-Z Mip 3") \
    X(HiZ_Mip4, "Hi-Z Mip 4") \
    X(SSR_Result, "SSR Result") \
    X(SSR_Confidence, "SSR Confidence")

enum class EGBufferDebugMode : int {
    #define X(name, str) name,
    GBUFFER_DEBUG_MODES
    #undef X
    COUNT
};

inline const char* const* GetGBufferDebugModeNames() {
    static const char* names[] = {
        #define X(name, str) str,
        GBUFFER_DEBUG_MODES
        #undef X
    };
    return names;
}

inline constexpr int GetGBufferDebugModeCount() {
    return static_cast<int>(EGBufferDebugMode::COUNT);
}

// ============================================
// Bloom Settings - HDR Bloom Post-Processing
// ============================================
struct SBloomSettings
{
    float threshold = 1.0f;    // Luminance cutoff (0-5)
    float intensity = 1.0f;    // Bloom strength multiplier (0-3)
    float scatter = 0.7f;      // Mip blend factor (0-1), higher = more diffuse glow
};

// ============================================
// Motion Blur Settings - Camera Motion Blur
// ============================================
struct SMotionBlurSettings
{
    float intensity = 0.5f;      // Blur strength multiplier (0-1)
    int sampleCount = 12;        // Number of samples along velocity (8-16)
    float maxBlurPixels = 32.0f; // Maximum blur radius in pixels (8-64)
};

// ============================================
// Color Grading Preset - Built-in color looks
// ============================================
enum class EColorGradingPreset : int
{
    Neutral = 0,    // No grading (identity)
    Warm,           // Warm tones, lifted shadows
    Cool,           // Cool tones, blue tint
    Cinematic,      // High contrast, teal/orange
    Custom          // User-loaded LUT
};

inline const char* GetColorGradingPresetName(EColorGradingPreset preset) {
    switch (preset) {
        case EColorGradingPreset::Neutral:   return "Neutral";
        case EColorGradingPreset::Warm:      return "Warm";
        case EColorGradingPreset::Cool:      return "Cool";
        case EColorGradingPreset::Cinematic: return "Cinematic";
        case EColorGradingPreset::Custom:    return "Custom";
        default: return "Unknown";
    }
}

// ============================================
// Color Grading Settings - LDR Color Correction
// ============================================
struct SColorGradingSettings
{
    // Preset selection
    EColorGradingPreset preset = EColorGradingPreset::Neutral;

    // Custom LUT path (.cube file)
    std::string lutPath = "";

    // Lift/Gamma/Gain (per-channel RGB control)
    // Range: -1.0 to +1.0 for each channel
    DirectX::XMFLOAT3 lift  = {0.0f, 0.0f, 0.0f};   // Shadows adjustment
    DirectX::XMFLOAT3 gamma = {0.0f, 0.0f, 0.0f};   // Midtones adjustment
    DirectX::XMFLOAT3 gain  = {0.0f, 0.0f, 0.0f};   // Highlights adjustment

    // Simple adjustments (range: -1.0 to +1.0)
    float saturation = 0.0f;   // -1 (grayscale) to +1 (oversaturated)
    float contrast = 0.0f;     // -1 (flat) to +1 (high contrast)
    float temperature = 0.0f;  // -1 (cool/blue) to +1 (warm/orange)

    // Apply preset values
    void ApplyPreset(EColorGradingPreset newPreset) {
        preset = newPreset;
        // Reset all values first
        lift = gamma = gain = {0.0f, 0.0f, 0.0f};
        saturation = contrast = temperature = 0.0f;
        lutPath = "";

        switch (newPreset) {
            case EColorGradingPreset::Neutral:
                // All defaults (identity)
                break;
            case EColorGradingPreset::Warm:
                temperature = 0.3f;
                saturation = 0.1f;
                lift = {0.02f, 0.01f, -0.02f};
                break;
            case EColorGradingPreset::Cool:
                temperature = -0.3f;
                contrast = 0.1f;
                lift = {-0.02f, 0.0f, 0.03f};
                break;
            case EColorGradingPreset::Cinematic:
                contrast = 0.15f;
                saturation = -0.1f;
                lift = {-0.03f, -0.02f, 0.02f};
                gain = {0.02f, 0.0f, -0.02f};
                break;
            case EColorGradingPreset::Custom:
                // Keep current values, user will load LUT
                break;
        }
    }
};

// ============================================
// Anti-Aliasing Mode - Post-Process AA Algorithm
// ============================================
enum class EAntiAliasingMode : int
{
    Off = 0,    // No anti-aliasing
    FXAA = 1,   // Fast Approximate AA (NVIDIA, single pass, ~0.5ms)
    SMAA = 2    // Subpixel Morphological AA (3-pass, higher quality, ~1.5ms)
};

inline const char* GetAntiAliasingModeName(EAntiAliasingMode mode) {
    switch (mode) {
        case EAntiAliasingMode::Off:  return "Off";
        case EAntiAliasingMode::FXAA: return "FXAA";
        case EAntiAliasingMode::SMAA: return "SMAA";
        default: return "Unknown";
    }
}

// ============================================
// Anti-Aliasing Settings
// ============================================
struct SAntiAliasingSettings
{
    EAntiAliasingMode mode = EAntiAliasingMode::Off;

    // FXAA-specific settings
    float fxaaSubpixelQuality = 0.75f;   // 0.0 (sharp) to 1.0 (soft)
    float fxaaEdgeThreshold = 0.166f;    // Edge detection sensitivity
    float fxaaEdgeThresholdMin = 0.0833f; // Minimum edge threshold
};

// ============================================
// FSR 2.0 Quality Mode - Upscaling Presets
// ============================================
enum class EFSR2QualityMode : int
{
    NativeAA = 0,        // 1.0x (FSR as TAA only, no upscaling)
    Quality = 1,         // 1.5x upscale
    Balanced = 2,        // 1.7x upscale
    Performance = 3,     // 2.0x upscale
    UltraPerformance = 4 // 3.0x upscale
};

inline const char* GetFSR2QualityModeName(EFSR2QualityMode mode) {
    switch (mode) {
        case EFSR2QualityMode::NativeAA:         return "Native AA";
        case EFSR2QualityMode::Quality:          return "Quality";
        case EFSR2QualityMode::Balanced:         return "Balanced";
        case EFSR2QualityMode::Performance:      return "Performance";
        case EFSR2QualityMode::UltraPerformance: return "Ultra Performance";
        default: return "Unknown";
    }
}

// ============================================
// FSR 2.0 Settings - Temporal Upscaling
// ============================================
struct SFSR2Settings
{
    bool enabled = false;  // Enable FSR 2.0 (replaces TAA when enabled)
    EFSR2QualityMode qualityMode = EFSR2QualityMode::Quality;
    float sharpness = 0.5f;   // RCAS sharpening strength (0.0-1.0)
};

// ============================================
// Depth of Field Settings - Cinematic Focus Blur
// ============================================
struct SDepthOfFieldSettings
{
    float focusDistance = 10.0f;   // Focus plane distance in world units (1-100)
    float focalRange = 5.0f;       // Depth range that remains in focus (1-20)
    float aperture = 2.8f;         // f-stop value, lower = more blur (1.4-16)
    float maxBlurRadius = 8.0f;    // Maximum blur radius in pixels (4-16)
};

// ============================================
// Auto Exposure Settings - HDR Eye Adaptation
// ============================================
struct SAutoExposureSettings
{
    float minEV = -4.0f;            // Minimum exposure (EV units, very dark scenes)
    float maxEV = 4.0f;             // Maximum exposure (EV units, very bright scenes)
    float adaptSpeedUp = 2.0f;      // Dark->Bright adaptation speed (seconds)
    float adaptSpeedDown = 4.0f;    // Bright->Dark adaptation speed (seconds)
    float exposureCompensation = 0.0f;  // Manual bias (-2 to +2 EV)
    float centerWeight = 0.7f;      // Center metering weight (0=uniform, 1=center only)
};

// ============================================
// Volumetric Lightmap 配置
// ============================================
struct SVolumetricLightmapConfig
{
    // 体积范围（世界坐标）
    DirectX::XMFLOAT3 volumeMin = {-50.0f, -10.0f, -50.0f};
    DirectX::XMFLOAT3 volumeMax = {50.0f, 30.0f, 50.0f};

    // 最小 Brick 的世界尺寸（决定最大精度）
    // 例如：2.0f 表示最精细的 Brick 覆盖 2m × 2m × 2m
    float minBrickWorldSize = 2.0f;

    // 是否启用
    bool enabled = false;
};

// ============================================
// Scene-level lighting settings
// ============================================
class CSceneLightSettings {
public:
    // Environment / Skybox
    std::string skyboxAssetPath = "";

    // Diffuse GI Mode
    EDiffuseGIMode diffuseGIMode = EDiffuseGIMode::GlobalIBL;

    // Volumetric Lightmap
    SVolumetricLightmapConfig volumetricLightmap;

    // Post-Processing: Bloom
    SBloomSettings bloom;

    // Post-Processing: Motion Blur
    SMotionBlurSettings motionBlur;

    // Post-Processing: Auto Exposure
    SAutoExposureSettings autoExposure;

    // Post-Processing: Color Grading
    SColorGradingSettings colorGrading;

    // Post-Processing: Anti-Aliasing
    SAntiAliasingSettings antiAliasing;

    // Post-Processing: FSR 2.0 (Temporal Upscaling)
    SFSR2Settings fsr2;

    // Post-Processing: Depth of Field
    SDepthOfFieldSettings depthOfField;

    // G-Buffer Debug Visualization
    EGBufferDebugMode gBufferDebugMode = EGBufferDebugMode::None;

    // Future additions:
    // float iblIntensity = 1.0f;
    // float ambientIntensity = 1.0f;
    // bool fogEnabled = false;
    // DirectX::XMFLOAT3 fogColor = {0.5f, 0.6f, 0.7f};
    // float fogDensity = 0.01f;
};
