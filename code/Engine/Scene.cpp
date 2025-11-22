#include "Scene.h"
#include "Core/FFAssetLoader.h"
#include <iostream>
#include <filesystem>

bool CScene::Initialize(const std::string& hdrPath, int cubemapSize) {
    if (m_initialized) {
        std::cout << "Scene: Already initialized!" << std::endl;
        return true;
    }

    std::cout << "Scene: Initializing..." << std::endl;

    // Check if path is .ffasset (pre-baked) or .hdr (need to generate)
    std::filesystem::path assetPath(hdrPath);
    bool isFFAsset = (assetPath.extension() == ".ffasset");

    if (isFFAsset) {
        std::cout << "Scene: Loading from .ffasset (pre-baked IBL)..." << std::endl;

        // Parse .ffasset file
        CFFAssetLoader::SkyboxAsset skyboxAsset;
        if (!CFFAssetLoader::LoadSkyboxAsset(hdrPath, skyboxAsset)) {
            std::cout << "ERROR: Failed to load .ffasset file!" << std::endl;
            return false;
        }

        // Load skybox environment from KTX2
        if (!m_skybox.InitializeFromKTX2(skyboxAsset.envPath)) {
            std::cout << "ERROR: Failed to load skybox from KTX2!" << std::endl;
            return false;
        }

        // Initialize IBL generator (for shader resources)
        if (!m_iblGen.Initialize()) {
            std::cout << "ERROR: Failed to initialize IBL generator!" << std::endl;
            return false;
        }

        // Load irradiance map from KTX2
        if (!m_iblGen.LoadIrradianceFromKTX2(skyboxAsset.irrPath)) {
            std::cout << "ERROR: Failed to load irradiance map from KTX2!" << std::endl;
            return false;
        }

        // Load pre-filtered map from KTX2
        if (!m_iblGen.LoadPreFilteredFromKTX2(skyboxAsset.prefilterPath)) {
            std::cout << "ERROR: Failed to load pre-filtered map from KTX2!" << std::endl;
            return false;
        }

        // Load BRDF LUT from fixed path
        std::string brdfLutPath = "E:/forfun/assets/skybox/brdf_lut.ktx2";
        if (!m_iblGen.LoadBrdfLutFromKTX2(brdfLutPath)) {
            std::cout << "ERROR: Failed to load BRDF LUT from " << brdfLutPath << std::endl;
            return false;
        }

        std::cout << "Scene: Pre-baked IBL loaded successfully!" << std::endl;
    }
    else {
        std::cout << "Scene: Loading from HDR (will generate IBL)..." << std::endl;

        // Initialize IBL generator first
        if (!m_iblGen.Initialize()) {
            std::cout << "ERROR: Failed to initialize IBL generator!" << std::endl;
            return false;
        }

        // Initialize skybox (loads HDR and converts to cubemap with mipmaps)
        if (!m_skybox.Initialize(hdrPath, cubemapSize)) {
            std::cout << "ERROR: Failed to initialize skybox!" << std::endl;
            return false;
        }

        // Auto-generate IBL resources for rendering (required for PBR)
        std::cout << "Scene: Generating IBL resources for rendering..." << std::endl;

        ID3D11ShaderResourceView* envMap = m_skybox.GetEnvironmentMap();
        if (!envMap) {
            std::cout << "ERROR: Failed to get environment map from skybox!" << std::endl;
            return false;
        }

        // Generate irradiance map (32x32, single mip)
        std::cout << "  - Generating irradiance map (diffuse IBL)..." << std::endl;
        if (!m_iblGen.GenerateIrradianceMap(envMap, 64)) {
            std::cout << "ERROR: Failed to generate irradiance map!" << std::endl;
            return false;
        }

        // Generate pre-filtered environment map (128x128, 7 mip levels)
        std::cout << "  - Generating pre-filtered map (specular IBL, 7 mip levels)..." << std::endl;
        if (!m_iblGen.GeneratePreFilteredMap(envMap, 128, 7)) {
            std::cout << "ERROR: Failed to generate pre-filtered map!" << std::endl;
            return false;
        }

        // Generate BRDF LUT (512x512)
        std::cout << "  - Generating BRDF LUT (512x512)..." << std::endl;
        if (!m_iblGen.GenerateBrdfLut(512)) {
            std::cout << "ERROR: Failed to generate BRDF LUT!" << std::endl;
            return false;
        }

        std::cout << "Scene: IBL resources generated successfully!" << std::endl;
    }
    std::cout << "Scene: Initialization complete!" << std::endl;
    std::cout << "Note: Debug visualization can be enabled via IBL Debug window." << std::endl;
    m_initialized = true;
    return true;
}

void CScene::Shutdown() {
    std::cout << "Scene: Shutting down..." << std::endl;
    m_skybox.Shutdown();
    m_iblGen.Shutdown();
    m_initialized = false;
}
