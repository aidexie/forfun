#include "FFAssetLoader.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

bool CFFAssetLoader::LoadSkyboxAsset(const std::string& ffassetPath, SkyboxAsset& outAsset) {
    // Read JSON file
    std::ifstream file(ffassetPath);
    if (!file.is_open()) {
        std::cerr << "FFAssetLoader: Failed to open " << ffassetPath << std::endl;
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        std::cerr << "FFAssetLoader: Failed to parse JSON: " << e.what() << std::endl;
        return false;
    }

    // Check type
    if (!j.contains("type") || j["type"] != "skybox") {
        std::cerr << "FFAssetLoader: ERROR - Asset type is not 'skybox' (got: "
                  << (j.contains("type") ? j["type"].get<std::string>() : "missing") << ")" << std::endl;
        return false;
    }

    // Check version
    if (!j.contains("version")) {
        std::cerr << "FFAssetLoader: Warning - Missing version field" << std::endl;
    }

    // Get base directory
    fs::path basePath = fs::path(ffassetPath).parent_path();

    // Parse data fields
    if (!j.contains("data")) {
        std::cerr << "FFAssetLoader: Missing 'data' field" << std::endl;
        return false;
    }

    auto& data = j["data"];

    if (!data.contains("env") || !data.contains("irr") || !data.contains("prefilter")) {
        std::cerr << "FFAssetLoader: Missing required texture paths (env/irr/prefilter)" << std::endl;
        return false;
    }

    // Build absolute paths
    outAsset.envPath = (basePath / data["env"].get<std::string>()).string();
    outAsset.irrPath = (basePath / data["irr"].get<std::string>()).string();
    outAsset.prefilterPath = (basePath / data["prefilter"].get<std::string>()).string();

    // Optional: source path
    if (j.contains("source")) {
        outAsset.sourcePath = (basePath / j["source"].get<std::string>()).string();
    }

    std::cout << "FFAssetLoader: Loaded skybox asset from " << ffassetPath << std::endl;
    std::cout << "  - Environment: " << outAsset.envPath << std::endl;
    std::cout << "  - Irradiance: " << outAsset.irrPath << std::endl;
    std::cout << "  - Prefilter: " << outAsset.prefilterPath << std::endl;

    return true;
}
