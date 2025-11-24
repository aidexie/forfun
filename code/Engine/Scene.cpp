#include "Scene.h"
#include "Core/FFAssetLoader.h"
#include "Core/FFLog.h"
#include <filesystem>

bool CScene::Initialize(const std::string& skybox_path) {
    if (m_initialized) {
        CFFLog::Warning("Scene: Already initialized!");
        return true;
    }

    CFFLog::Info("Scene: Initializing...");

    // Check if path is .ffasset (pre-baked) or .hdr (need to generate)
    std::filesystem::path assetPath(skybox_path);
    bool isFFAsset = (assetPath.extension() == ".ffasset");

    if (isFFAsset) {
        CFFLog::Info("Scene: Loading from .ffasset (pre-baked IBL)...");

        // Parse .ffasset file
        CFFAssetLoader::SkyboxAsset skyboxAsset;
        if (!CFFAssetLoader::LoadSkyboxAsset(skybox_path, skyboxAsset))
        {
            CFFLog::Error("Failed to load .ffasset file!");
            return false;
        }

        // Load skybox environment from KTX2
        if (!m_skybox.InitializeFromKTX2(skyboxAsset.envPath)) {
            CFFLog::Error("Failed to load skybox from KTX2!");
            return false;
        }

        // Initialize IBL generator (for shader resources)
        if (!m_iblGen.Initialize()) {
            CFFLog::Error("Failed to initialize IBL generator!");
            return false;
        }

        // Load irradiance map from KTX2
        if (!m_iblGen.LoadIrradianceFromKTX2(skyboxAsset.irrPath)) {
            CFFLog::Error("Failed to load irradiance map from KTX2!");
            return false;
        }

        // Load pre-filtered map from KTX2
        if (!m_iblGen.LoadPreFilteredFromKTX2(skyboxAsset.prefilterPath)) {
            CFFLog::Error("Failed to load pre-filtered map from KTX2!");
            return false;
        }

        // Load BRDF LUT from fixed path
        std::string brdfLutPath = "E:/forfun/assets/skybox/brdf_lut.ktx2";
        if (!m_iblGen.LoadBrdfLutFromKTX2(brdfLutPath)) {
            CFFLog::Error("Failed to load BRDF LUT from %s", brdfLutPath.c_str());
            return false;
        }

        CFFLog::Info("Scene: Pre-baked IBL loaded successfully!");
    }
    CFFLog::Info("Scene: Initialization complete!");
    CFFLog::Info("Note: Debug visualization can be enabled via IBL Debug window.");
    m_initialized = true;
    return true;
}

void CScene::Shutdown() {
    CFFLog::Info("Scene: Shutting down...");
    m_skybox.Shutdown();
    m_iblGen.Shutdown();
    m_initialized = false;
}
