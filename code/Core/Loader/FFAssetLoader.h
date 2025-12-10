#pragma once
#include <string>

// FFAsset (ForFun Asset) Loader
class CFFAssetLoader {
public:
    struct SkyboxAsset {
        std::string envPath;
        std::string irrPath;
        std::string prefilterPath;
        std::string sourcePath;
    };

    // Parse .ffasset file and return asset data
    // Returns true if successful and type is "skybox"
    static bool LoadSkyboxAsset(const std::string& ffassetPath, SkyboxAsset& outAsset);
};
