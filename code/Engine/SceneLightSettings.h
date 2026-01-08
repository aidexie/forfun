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
    X(SSAO, "SSAO")

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
    bool enabled = true;       // Enable/disable bloom effect
    float threshold = 1.0f;    // Luminance cutoff (0-5)
    float intensity = 1.0f;    // Bloom strength multiplier (0-3)
    float scatter = 0.7f;      // Mip blend factor (0-1), higher = more diffuse glow
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

    // G-Buffer Debug Visualization
    EGBufferDebugMode gBufferDebugMode = EGBufferDebugMode::None;

    // Future additions:
    // float iblIntensity = 1.0f;
    // float ambientIntensity = 1.0f;
    // bool fogEnabled = false;
    // DirectX::XMFLOAT3 fogColor = {0.5f, 0.6f, 0.7f};
    // float fogDensity = 0.01f;
};
