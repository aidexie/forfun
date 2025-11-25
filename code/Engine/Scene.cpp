#include "Scene.h"
#include "Core/Loader/FFAssetLoader.h"
#include "Core/FFLog.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include <filesystem>
#include <sstream>
#include <iomanip>

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

std::string CScene::GenerateReport() const {
    std::ostringstream oss;

    // Header
    oss << "================================\n";
    oss << "[SCENE STATE REPORT]\n";
    oss << "================================\n\n";

    // GameObject count
    oss << "[GameObjects]\n";
    oss << "  Total Count: " << m_world.Count() << "\n";

    if (m_world.Count() == 0) {
        oss << "  (empty scene)\n";
    } else {
        oss << "\n";

        // List all GameObjects with their components
        for (size_t i = 0; i < m_world.Count(); ++i) {
            auto* obj = m_world.Get(i);
            oss << "  [" << i << "] \"" << obj->GetName() << "\"";

            if (m_selected == static_cast<int>(i)) {
                oss << " (SELECTED)";
            }
            oss << "\n";

            // List components
            auto* transform = obj->GetComponent<STransform>();
            if (transform) {
                oss << "      Transform: pos("
                    << std::fixed << std::setprecision(2)
                    << transform->position.x << ", "
                    << transform->position.y << ", "
                    << transform->position.z << ")"
                    << " scale("
                    << transform->scale.x << ", "
                    << transform->scale.y << ", "
                    << transform->scale.z << ")\n";
            }

            auto* meshRenderer = obj->GetComponent<SMeshRenderer>();
            if (meshRenderer) {
                oss << "      MeshRenderer: \"" << meshRenderer->path << "\"\n";
            }

            auto* dirLight = obj->GetComponent<SDirectionalLight>();
            if (dirLight) {
                auto dir = dirLight->GetDirection();
                oss << "      DirectionalLight: color("
                    << std::fixed << std::setprecision(2)
                    << dirLight->color.x << ", "
                    << dirLight->color.y << ", "
                    << dirLight->color.z << ")"
                    << " intensity=" << dirLight->intensity
                    << " dir(" << dir.x << ", " << dir.y << ", " << dir.z << ")\n";
            }
        }
    }

    // Selection state
    oss << "\n[Selection]\n";
    if (m_selected >= 0 && static_cast<size_t>(m_selected) < m_world.Count()) {
        oss << "  Selected Object: [" << m_selected << "] \""
            << m_world.Get(static_cast<size_t>(m_selected))->GetName() << "\"\n";
    } else {
        oss << "  Selected Object: None\n";
    }

    // Skybox state
    oss << "\n[Environment]\n";
    oss << "  Skybox Asset: ";
    if (m_lightSettings.skyboxAssetPath.empty()) {
        oss << "(none)\n";
    } else {
        oss << "\"" << m_lightSettings.skyboxAssetPath << "\"\n";
    }
    oss << "  Initialized: " << (m_initialized ? "Yes" : "No") << "\n";

    // Light count summary
    oss << "\n[Lights]\n";
    int dirLightCount = 0;
    for (size_t i = 0; i < m_world.Count(); ++i) {
        auto* obj = m_world.Get(i);
        if (obj->GetComponent<SDirectionalLight>()) {
            dirLightCount++;
        }
    }
    oss << "  Directional Lights: " << dirLightCount << "\n";

    oss << "\n================================\n";

    return oss.str();
}
