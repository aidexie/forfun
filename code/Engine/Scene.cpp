#include "Scene.h"
#include "Core/FFAssetLoader.h"
#include "Core/FFLog.h"
#include <filesystem>

bool CScene::Initialize(const std::string& hdrPath, int cubemapSize) {
    if (m_initialized) {
        CFFLog::Warning("Scene: Already initialized!");
        return true;
    }

    CFFLog::Info("Scene: Initializing...");

    // Check if path is .ffasset (pre-baked) or .hdr (need to generate)
    std::filesystem::path assetPath(hdrPath);
    bool isFFAsset = (assetPath.extension() == ".ffasset");

    if (isFFAsset) {
        CFFLog::Info("Scene: Loading from .ffasset (pre-baked IBL)...");

        // Parse .ffasset file
        CFFAssetLoader::SkyboxAsset skyboxAsset;
        if (!CFFAssetLoader::LoadSkyboxAsset(hdrPath, skyboxAsset)) {
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
    else {
        CFFLog::Info("Scene: Loading from HDR (will generate IBL)...");

        // Initialize IBL generator first
        if (!m_iblGen.Initialize()) {
            CFFLog::Error("Failed to initialize IBL generator!");
            return false;
        }

        // Initialize skybox (loads HDR and converts to cubemap with mipmaps)
        if (!m_skybox.Initialize(hdrPath, cubemapSize)) {
            CFFLog::Error("Failed to initialize skybox!");
            return false;
        }

        // Auto-generate IBL resources for rendering (required for PBR)
        CFFLog::Info("Scene: Generating IBL resources for rendering...");

        ID3D11ShaderResourceView* envMap = m_skybox.GetEnvironmentMap();
        if (!envMap) {
            CFFLog::Error("Failed to get environment map from skybox!");
            return false;
        }

        // Generate irradiance map (32x32, single mip)
        CFFLog::Info("  - Generating irradiance map (diffuse IBL)...");
        if (!m_iblGen.GenerateIrradianceMap(envMap, 64)) {
            CFFLog::Error("Failed to generate irradiance map!");
            return false;
        }

        // Generate pre-filtered environment map (128x128, 7 mip levels)
        CFFLog::Info("  - Generating pre-filtered map (specular IBL, 7 mip levels)...");
        if (!m_iblGen.GeneratePreFilteredMap(envMap, 128, 7)) {
            CFFLog::Error("Failed to generate pre-filtered map!");
            return false;
        }

        // Generate BRDF LUT (512x512)
        CFFLog::Info("  - Generating BRDF LUT (512x512)...");
        if (!m_iblGen.GenerateBrdfLut(512)) {
            CFFLog::Error("Failed to generate BRDF LUT!");
            return false;
        }

        CFFLog::Info("Scene: IBL resources generated successfully!");
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
