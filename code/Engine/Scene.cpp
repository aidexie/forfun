#include "Scene.h"
#include "Core/Loader/FFAssetLoader.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"  // FFPath namespace
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include "SceneSerializer.h"
#include <imgui.h>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <regex>

bool CScene::Initialize() {
    if (m_initialized) {
        CFFLog::Warning("Scene: Already initialized!");
        return true;
    }

    CFFLog::Info("Scene: Initializing GPU resources...");

    // Initialize Reflection Probe Manager (creates TextureCubeArray)
    if (!m_probeManager.Initialize()) {
        CFFLog::Error("Failed to initialize ReflectionProbeManager!");
        return false;
    }

    // Initialize Light Probe Manager (creates StructuredBuffer for SH data)
    if (!m_lightProbeManager.Initialize()) {
        CFFLog::Error("Failed to initialize LightProbeManager!");
        return false;
    }

    // Load BRDF LUT (shared across all environments, managed by ProbeManager)
    std::string brdfLutPath = FFPath::GetAbsolutePath("skybox/brdf_lut.ktx2");
    if (!m_probeManager.LoadBrdfLut(brdfLutPath)) {
        CFFLog::Error("Failed to load BRDF LUT from %s", brdfLutPath.c_str());
        return false;
    }

    CFFLog::Info("Scene: GPU resources initialized!");
    m_initialized = true;
    return true;
}

void CScene::Clear() {
    // Clear all GameObjects
    while (m_world.Count() > 0) {
        m_world.Destroy(0);
    }
    m_selected = -1;
    m_filePath.clear();
    CFFLog::Info("Scene: Cleared all GameObjects");
}

// === Scene File Management ===

bool CScene::LoadFromFile(const std::string& scenePath) {
    CFFLog::Info("Scene: Loading from %s", scenePath.c_str());

    // 1. Deserialize GameObjects (Serializer clears existing scene internally)
    if (!CSceneSerializer::LoadScene(*this, scenePath)) {
        CFFLog::Error("Scene: Failed to load scene file!");
        return false;
    }

    // 3. Load environment (skybox + IBL) from lightSettings
    if (!m_lightSettings.skyboxAssetPath.empty()) {
        if (!ReloadEnvironment(m_lightSettings.skyboxAssetPath)) {
            CFFLog::Warning("Scene: Failed to load environment, continuing without skybox");
        }
    }

    // 4. Load reflection probes (now GameObjects exist)
    ReloadProbesFromScene();

    // 5. Load light probes (now GameObjects exist)
    ReloadLightProbesFromScene();

    // 6. Record file path
    m_filePath = scenePath;

    // 7. Auto-load 2D lightmap if exists
    size_t dotPos = scenePath.find_last_of('.');
    if (dotPos != std::string::npos) {
        m_lightmapPath = scenePath.substr(0, dotPos) + ".lightmap";
        std::string absPath = FFPath::GetAbsolutePath(m_lightmapPath);
        if (std::filesystem::exists(absPath)) {
            if (m_lightmap2D.LoadLightmap(m_lightmapPath)) {
                CFFLog::Info("Scene: Auto-loaded 2D lightmap from %s", m_lightmapPath.c_str());
            }
        }else{
                CFFLog::Info("Scene: 2D lightmap %s was not exist", m_lightmapPath.c_str());
        }
    }

    CFFLog::Info("Scene: Loaded successfully!");
    return true;
}

bool CScene::SaveToFile(const std::string& scenePath) {
    CFFLog::Info("Scene: Saving to %s", scenePath.c_str());

    if (!CSceneSerializer::SaveScene(*this, scenePath)) {
        CFFLog::Error("Scene: Failed to save scene file!");
        return false;
    }

    m_filePath = scenePath;
    CFFLog::Info("Scene: Saved successfully!");
    return true;
}

// === Environment Resource Management ===
bool CScene::ReloadEnvironment(const std::string& ffassetPath) {
    CFFLog::Info("Scene: Reloading environment from %s", ffassetPath.c_str());

    // Normalize and resolve to absolute path
    std::string normalizedPath = FFPath::Normalize(ffassetPath);
    std::string fullPath = FFPath::GetAbsolutePath(ffassetPath);

    // Parse .ffasset file
    CFFAssetLoader::SkyboxAsset skyboxAsset;
    if (!CFFAssetLoader::LoadSkyboxAsset(fullPath, skyboxAsset)) {
        CFFLog::Error("Failed to load .ffasset file!");
        return false;
    }

    // Reload skybox display cubemap
    if (!m_skybox.InitializeFromKTX2(skyboxAsset.envPath)) {
        CFFLog::Error("Failed to reload skybox from KTX2!");
        return false;
    }

    // Update lightSettings (store normalized relative path)
    m_lightSettings.skyboxAssetPath = normalizedPath;

    // Reload global probe (index 0) with IBL data
    m_probeManager.LoadGlobalProbe(skyboxAsset.irrPath, skyboxAsset.prefilterPath);

    CFFLog::Info("Scene: Environment reloaded successfully!");
    return true;
}

// === Reflection Probe Management ===

void CScene::ReloadProbesFromScene() {
    CFFLog::Info("Scene: Reloading local probes from scene...");

    // 只加载局部 Probe (index 1-7)
    // 全局 IBL (index 0) 由 ProbeManager::Initialize() 设置默认值，
    // 或通过 ReloadEnvironment() -> LoadGlobalProbe() 更新
    m_probeManager.LoadLocalProbesFromScene(*this);
    CFFLog::Info("Scene: Local probes reloaded!");
}

// === Light Probe Management ===

void CScene::ReloadLightProbesFromScene() {
    CFFLog::Info("Scene: Reloading light probes from scene...");
    m_lightProbeManager.LoadProbesFromScene(*this);
    CFFLog::Info("Scene: Light probes reloaded! (%d probes)", m_lightProbeManager.GetProbeCount());
}

void CScene::Shutdown() {
    CFFLog::Info("Scene: Shutting down...");
    m_volumetricLightmap.Shutdown();
    m_lightProbeManager.Shutdown();
    m_probeManager.Shutdown();
    m_skybox.Shutdown();
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
