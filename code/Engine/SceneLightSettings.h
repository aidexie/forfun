#pragma once
#include <string>
#include <DirectXMath.h>

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

    // Volumetric Lightmap
    SVolumetricLightmapConfig volumetricLightmap;

    // Future additions:
    // float iblIntensity = 1.0f;
    // float ambientIntensity = 1.0f;
    // bool fogEnabled = false;
    // DirectX::XMFLOAT3 fogColor = {0.5f, 0.6f, 0.7f};
    // float fogDensity = 0.01f;
};
