#pragma once
#include <string>

// Scene-level lighting settings
class CSceneLightSettings {
public:
    std::string skyboxAssetPath = "";

    // Future additions:
    // float iblIntensity = 1.0f;
    // float ambientIntensity = 1.0f;
    // bool fogEnabled = false;
    // DirectX::XMFLOAT3 fogColor = {0.5f, 0.6f, 0.7f};
    // float fogDensity = 0.01f;
};
