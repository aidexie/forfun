#include "RenderConfig.h"
#include "PathManager.h"
#include "FFLog.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

// ============================================
// Helper: Backend enum to/from string
// ============================================

static std::string BackendToString(RHI::EBackend backend) {
    switch (backend) {
        case RHI::EBackend::DX11: return "DX11";
        case RHI::EBackend::DX12: return "DX12";
        default: return "Unknown";
    }
}

static RHI::EBackend StringToBackend(const std::string& str) {
    if (str == "DX11") return RHI::EBackend::DX11;
    if (str == "DX12") return RHI::EBackend::DX12;
    CFFLog::Warning("[RenderConfig] Unknown backend '%s', defaulting to DX11", str.c_str());
    return RHI::EBackend::DX11;
}

// ============================================
// Load / Save
// ============================================

bool SRenderConfig::Load(const std::string& path, SRenderConfig& outConfig) {
    std::ifstream file(path);
    if (!file.is_open()) {
        CFFLog::Warning("[RenderConfig] Config file not found: %s (using defaults)", path.c_str());
        return false;
    }

    try {
        json j;
        file >> j;

        // Backend
        if (j.contains("backend")) {
            outConfig.backend = StringToBackend(j["backend"].get<std::string>());
        }

        // Window settings
        if (j.contains("window")) {
            auto& win = j["window"];
            if (win.contains("width")) outConfig.windowWidth = win["width"].get<uint32_t>();
            if (win.contains("height")) outConfig.windowHeight = win["height"].get<uint32_t>();
            if (win.contains("fullscreen")) outConfig.fullscreen = win["fullscreen"].get<bool>();
            if (win.contains("vsync")) outConfig.vsync = win["vsync"].get<bool>();
        }

        // Graphics settings
        if (j.contains("graphics")) {
            auto& gfx = j["graphics"];
            if (gfx.contains("msaaSamples")) outConfig.msaaSamples = gfx["msaaSamples"].get<uint32_t>();
            if (gfx.contains("enableValidation")) outConfig.enableValidation = gfx["enableValidation"].get<bool>();
        }

        CFFLog::Info("[RenderConfig] Loaded config from %s", path.c_str());
        CFFLog::Info("[RenderConfig]   Backend: %s", BackendToString(outConfig.backend).c_str());
        CFFLog::Info("[RenderConfig]   Resolution: %dx%d", outConfig.windowWidth, outConfig.windowHeight);
        CFFLog::Info("[RenderConfig]   VSync: %s", outConfig.vsync ? "On" : "Off");

        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error("[RenderConfig] Failed to parse config: %s", e.what());
        return false;
    }
}

bool SRenderConfig::Save(const std::string& path, const SRenderConfig& config) {
    try {
        json j;

        // Backend
        j["backend"] = BackendToString(config.backend);

        // Window settings
        j["window"]["width"] = config.windowWidth;
        j["window"]["height"] = config.windowHeight;
        j["window"]["fullscreen"] = config.fullscreen;
        j["window"]["vsync"] = config.vsync;

        // Graphics settings
        j["graphics"]["msaaSamples"] = config.msaaSamples;
        j["graphics"]["enableValidation"] = config.enableValidation;

        // Write to file
        std::ofstream file(path);
        if (!file.is_open()) {
            CFFLog::Error("[RenderConfig] Failed to open file for writing: %s", path.c_str());
            return false;
        }

        file << j.dump(4);  // Pretty print with 4-space indent
        CFFLog::Info("[RenderConfig] Saved config to %s", path.c_str());
        return true;
    }
    catch (const std::exception& e) {
        CFFLog::Error("[RenderConfig] Failed to save config: %s", e.what());
        return false;
    }
}

std::string SRenderConfig::GetDefaultPath() {
    return FFPath::GetAssetsDir() + "/config/render.json";
}
