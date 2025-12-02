#include "Scene.h"
#include "Core/Loader/FFAssetLoader.h"
#include "Core/FFLog.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include "SceneSerializer.h"
#include <imgui.h>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <regex>

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

        // Initialize Reflection Probe Manager
        if (!m_probeManager.Initialize()) {
            CFFLog::Error("Failed to initialize ReflectionProbeManager!");
            return false;
        }

        // Load probes (global IBL + local probes from scene)
        m_probeManager.LoadProbesFromScene(*this, skyboxAsset.irrPath, skyboxAsset.prefilterPath);
    }
    CFFLog::Info("Scene: Initialization complete!");
    CFFLog::Info("Note: Debug visualization can be enabled via IBL Debug window.");
    m_initialized = true;
    return true;
}

void CScene::Shutdown() {
    CFFLog::Info("Scene: Shutting down...");
    m_probeManager.Shutdown();
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

// ===========================
// Copy GameObject to Clipboard
// ===========================
void CScene::CopyGameObject(CGameObject* go) {
    if (!go) {
        CFFLog::Warning("[Scene] CopyGameObject: GameObject is null");
        return;
    }

    std::string json = CSceneSerializer::SerializeGameObject(go);
    if (json.empty()) {
        CFFLog::Error("[Scene] Failed to serialize GameObject for copy");
        return;
    }

    ImGui::SetClipboardText(json.c_str());
    CFFLog::Info("[Scene] Copied GameObject \"%s\" to clipboard", go->GetName().c_str());
}

// ===========================
// Paste GameObject from Clipboard
// ===========================
CGameObject* CScene::PasteGameObject() {
    const char* clipboardText = ImGui::GetClipboardText();
    if (!clipboardText || strlen(clipboardText) == 0) {
        CFFLog::Warning("[Scene] Clipboard is empty, cannot paste");
        return nullptr;
    }

    std::string jsonString(clipboardText);
    CGameObject* newGo = CSceneSerializer::DeserializeGameObject(m_world, jsonString);
    if (!newGo) {
        CFFLog::Error("[Scene] Failed to deserialize GameObject from clipboard");
        return nullptr;
    }

    // Handle naming conflict: "Name" -> "Name (1)", "Name (1)" -> "Name (2)", etc.
    std::string originalName = newGo->GetName();
    std::string uniqueName = originalName;

    // Parse existing suffix: "Name (N)" pattern
    std::regex pattern(R"(^(.*?)\s*\((\d+)\)$)");
    std::smatch match;
    std::string baseName = originalName;
    int currentSuffix = 0;

    if (std::regex_match(originalName, match, pattern)) {
        baseName = match[1].str();
        currentSuffix = std::stoi(match[2].str());
    }

    // Find next available number
    int nextSuffix = currentSuffix + 1;
    bool nameConflict = true;
    while (nameConflict) {
        uniqueName = baseName + " (" + std::to_string(nextSuffix) + ")";
        nameConflict = false;

        // Check if this name already exists
        for (size_t i = 0; i < m_world.Count(); ++i) {
            if (m_world.Get(i)->GetName() == uniqueName) {
                nameConflict = true;
                nextSuffix++;
                break;
            }
        }
    }

    newGo->SetName(uniqueName);

    // Apply Transform offset to avoid exact overlap
    auto* transform = newGo->GetComponent<STransform>();
    if (transform) {
        transform->position.x += 0.5f;  // Offset 0.5 units to the right
    }

    CFFLog::Info("[Scene] Pasted GameObject as \"%s\"", uniqueName.c_str());
    return newGo;
}

// ===========================
// Duplicate GameObject (Copy + Paste)
// ===========================
CGameObject* CScene::DuplicateGameObject(CGameObject* go) {
    if (!go) {
        CFFLog::Warning("[Scene] DuplicateGameObject: GameObject is null");
        return nullptr;
    }

    // Serialize and immediately deserialize (bypass clipboard)
    std::string json = CSceneSerializer::SerializeGameObject(go);
    if (json.empty()) {
        CFFLog::Error("[Scene] Failed to serialize GameObject for duplication");
        return nullptr;
    }

    CGameObject* newGo = CSceneSerializer::DeserializeGameObject(m_world, json);
    if (!newGo) {
        CFFLog::Error("[Scene] Failed to deserialize GameObject for duplication");
        return nullptr;
    }

    // Handle naming conflict (same logic as Paste)
    std::string originalName = newGo->GetName();
    std::string uniqueName = originalName;

    std::regex pattern(R"(^(.*?)\s*\((\d+)\)$)");
    std::smatch match;
    std::string baseName = originalName;
    int currentSuffix = 0;

    if (std::regex_match(originalName, match, pattern)) {
        baseName = match[1].str();
        currentSuffix = std::stoi(match[2].str());
    }

    int nextSuffix = currentSuffix + 1;
    bool nameConflict = true;
    while (nameConflict) {
        uniqueName = baseName + " (" + std::to_string(nextSuffix) + ")";
        nameConflict = false;

        for (size_t i = 0; i < m_world.Count(); ++i) {
            if (m_world.Get(i)->GetName() == uniqueName) {
                nameConflict = true;
                nextSuffix++;
                break;
            }
        }
    }

    newGo->SetName(uniqueName);

    // Apply Transform offset
    auto* transform = newGo->GetComponent<STransform>();
    if (transform) {
        transform->position.x += 0.5f;
    }

    CFFLog::Info("[Scene] Duplicated GameObject as \"%s\"", uniqueName.c_str());
    return newGo;
}
