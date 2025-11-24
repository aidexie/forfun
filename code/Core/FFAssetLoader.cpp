#include "FFAssetLoader.h"
#include "Editor/DiagnosticLog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

bool CFFAssetLoader::LoadSkyboxAsset(const std::string& ffassetPath, SkyboxAsset& outAsset) {
    // Read JSON file
    std::ifstream file(ffassetPath);
    if (!file.is_open()) {
        CDiagnosticLog::Error("FFAssetLoader: Failed to open %s", ffassetPath.c_str());
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        CDiagnosticLog::Error("FFAssetLoader: Failed to parse JSON: %s", e.what());
        return false;
    }

    // Check type
    if (!j.contains("type") || j["type"] != "skybox") {
        std::string typeStr = j.contains("type") ? j["type"].get<std::string>() : "missing";
        CDiagnosticLog::Error("FFAssetLoader: Asset type is not 'skybox' (got: %s)", typeStr.c_str());
        return false;
    }

    // Check version
    if (!j.contains("version")) {
        CDiagnosticLog::Warning("FFAssetLoader: Missing version field");
    }

    // Get base directory
    fs::path basePath = fs::path(ffassetPath).parent_path();

    // Parse data fields
    if (!j.contains("data")) {
        CDiagnosticLog::Error("FFAssetLoader: Missing 'data' field");
        return false;
    }

    auto& data = j["data"];

    if (!data.contains("env") || !data.contains("irr") || !data.contains("prefilter")) {
        CDiagnosticLog::Error("FFAssetLoader: Missing required texture paths (env/irr/prefilter)");
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

    std::CDiagnosticLog::Info("FFAssetLoader: Loaded skybox asset from %s",ffassetPath);
    std::CDiagnosticLog::Info("  - Environment: %s",outAsset.envPath);
    std::CDiagnosticLog::Info("  - Irradiance: %s",outAsset.irrPath);
    std::CDiagnosticLog::Info("  - Prefilter: %s",outAsset.prefilterPath);

    return true;
}
