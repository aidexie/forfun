#include "FFAssetLoader.h"
#include "Core/FFLog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

bool CFFAssetLoader::LoadSkyboxAsset(const std::string& ffassetPath, SkyboxAsset& outAsset) {
    // Read JSON file
    std::ifstream file(ffassetPath);
    if (!file.is_open()) {
        CFFLog::Error("FFAssetLoader: Failed to open %s", ffassetPath.c_str());
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        CFFLog::Error("FFAssetLoader: Failed to parse JSON: %s", e.what());
        return false;
    }

    // Check type
    if (!j.contains("type") || j["type"] != "skybox") {
        std::string typeStr = j.contains("type") ? j["type"].get<std::string>() : "missing";
        CFFLog::Error("FFAssetLoader: Asset type is not 'skybox' (got: %s)", typeStr.c_str());
        return false;
    }

    // Check version
    if (!j.contains("version")) {
        CFFLog::Warning("FFAssetLoader: Missing version field");
    }

    // Get base directory
    fs::path basePath = fs::path(ffassetPath).parent_path();

    // Parse data fields
    if (!j.contains("data")) {
        CFFLog::Error("FFAssetLoader: Missing 'data' field");
        return false;
    }

    auto& data = j["data"];

    if (!data.contains("env") || !data.contains("irr") || !data.contains("prefilter")) {
        CFFLog::Error("FFAssetLoader: Missing required texture paths (env/irr/prefilter)");
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

    CFFLog::Info("FFAssetLoader: Loaded skybox asset from %s", ffassetPath.c_str());
    CFFLog::Info("  - Environment: %s", outAsset.envPath.c_str());
    CFFLog::Info("  - Irradiance: %s", outAsset.irrPath.c_str());
    CFFLog::Info("  - Prefilter: %s", outAsset.prefilterPath.c_str());

    return true;
}
