#include "Scene.h"
#include <iostream>

bool CScene::Initialize(const std::string& hdrPath, int cubemapSize) {
    if (m_initialized) {
        std::cout << "Scene: Already initialized!" << std::endl;
        return true;
    }

    std::cout << "Scene: Initializing..." << std::endl;

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
    if (!m_iblGen.GenerateIrradianceMap(envMap, 32)) {
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
