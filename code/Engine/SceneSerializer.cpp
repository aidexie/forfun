// SceneSerializer.cpp
#include "Core/FFLog.h"
// NOTE: Requires nlohmann/json library
// Download json.hpp from: https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
// Place at: E:\forfun\thirdparty\nlohmann\json.hpp

#include "SceneSerializer.h"
#include "Scene.h"
#include "SceneLightSettings.h"  // EDiffuseGIMode
#include "World.h"
#include "GameObject.h"
#include "Component.h"
#include "ComponentRegistry.h"
#include "PropertyVisitor.h"
#include "Components/Transform.h"  // STransform
#include "Components/MeshRenderer.h"  // SMeshRenderer
#include "Components/DirectionalLight.h"  // SDirectionalLight

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

// ===========================
// JSON Write Visitor
// ===========================
class CJsonWriteVisitor : public CPropertyVisitor {
public:
    CJsonWriteVisitor(json& j) : m_json(j) {}

    void VisitFloat(const char* name, float& value) override {
        m_json[name] = value;
    }

    void VisitInt(const char* name, int& value) override {
        m_json[name] = value;
    }

    void VisitBool(const char* name, bool& value) override {
        m_json[name] = value;
    }

    void VisitString(const char* name, std::string& value) override {
        m_json[name] = value;
    }

    void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) override {
        m_json[name] = { value.x, value.y, value.z };
    }

    void VisitFloat3Array(const char* name, DirectX::XMFLOAT3* values, int count) override {
        // Store as flat array: [x0,y0,z0, x1,y1,z1, ...]
        json arr = json::array();
        for (int i = 0; i < count; i++) {
            arr.push_back(values[i].x);
            arr.push_back(values[i].y);
            arr.push_back(values[i].z);
        }
        m_json[name] = arr;
    }

    void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) override {
        m_json[name] = value;
    }

private:
    json& m_json;
};

// ===========================
// JSON Read Visitor
// ===========================
class CJsonReadVisitor : public CPropertyVisitor {
public:
    CJsonReadVisitor(const json& j) : m_json(j) {}

    void VisitFloat(const char* name, float& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<float>();
        }
    }

    void VisitInt(const char* name, int& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<int>();
        }
    }

    void VisitBool(const char* name, bool& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<bool>();
        }
    }

    void VisitString(const char* name, std::string& value) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<std::string>();
        }
    }

    void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) override {
        if (m_json.contains(name) && m_json[name].is_array() && m_json[name].size() == 3) {
            value.x = m_json[name][0].get<float>();
            value.y = m_json[name][1].get<float>();
            value.z = m_json[name][2].get<float>();
        }
    }

    void VisitFloat3Array(const char* name, DirectX::XMFLOAT3* values, int count) override {
        if (m_json.contains(name) && m_json[name].is_array()) {
            const auto& arr = m_json[name];
            int expectedSize = count * 3;
            if (arr.size() >= expectedSize) {
                for (int i = 0; i < count; i++) {
                    values[i].x = arr[i * 3 + 0].get<float>();
                    values[i].y = arr[i * 3 + 1].get<float>();
                    values[i].z = arr[i * 3 + 2].get<float>();
                }
            }
        }
    }

    void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) override {
        if (m_json.contains(name)) {
            value = m_json[name].get<int>();
        }
    }

private:
    const json& m_json;
};

// ===========================
// Serialize CComponent
// ===========================
static void SerializeComponent(const CComponent* comp, json& j) {
    j["type"] = comp->GetTypeName();
    CJsonWriteVisitor visitor(j);
    // const_cast is safe here: CJsonWriteVisitor only reads values, doesn't modify CComponent
    const_cast<CComponent*>(comp)->VisitProperties(visitor);
}

// ===========================
// Centralized CComponent Factory
// ===========================
// Automatically uses CComponentRegistry (no manual code needed!)
// Components auto-register via REGISTER_COMPONENT macro in their header files
static CComponent* CreateComponentByType(CGameObject* go, const std::string& typeName) {
    return CComponentRegistry::Instance().Create(go, typeName);
}

// ===========================
// Deserialize CComponent
// ===========================
static CComponent* DeserializeComponent(CGameObject* go, const json& j) {
    if (!j.contains("type")) return nullptr;

    std::string type = j["type"].get<std::string>();
    CComponent* comp = CreateComponentByType(go, type);

    if (comp) {
        // Load properties using reflection
        CJsonReadVisitor visitor(j);
        comp->VisitProperties(visitor);
    } else {
        CFFLog::Error("Failed to create component of type: %s", type.c_str());
    }

    return comp;
}

// ===========================
// Save CScene
// ===========================
bool CSceneSerializer::SaveScene(const CScene& scene, const std::string& filepath) {
    try {
        json j;
        j["version"] = "1.0";
        j["gameObjects"] = json::array();

        // Serialize all GameObjects
        for (std::size_t i = 0; i < scene.GetWorld().Count(); ++i) {
            auto* go = scene.GetWorld().Get(i);
            if (!go) continue;

            json goJson;
            goJson["name"] = go->GetName();
            goJson["components"] = json::array();

            // Serialize all components automatically using ForEachComponent
            go->ForEachComponent([&](const CComponent* comp) {
                json compJson;
                SerializeComponent(comp, compJson);
                goJson["components"].push_back(compJson);
            });

            j["gameObjects"].push_back(goJson);
        }

        // Serialize light settings
        json settingsJson;
        settingsJson["skyboxAssetPath"] = scene.GetLightSettings().skyboxAssetPath;
        settingsJson["diffuseGIMode"] = static_cast<int>(scene.GetLightSettings().diffuseGIMode);
        settingsJson["gBufferDebugMode"] = static_cast<int>(scene.GetLightSettings().gBufferDebugMode);

        // Serialize Volumetric Lightmap config
        const auto& vlConfig = scene.GetLightSettings().volumetricLightmap;
        json vlJson;
        vlJson["volumeMin"] = { vlConfig.volumeMin.x, vlConfig.volumeMin.y, vlConfig.volumeMin.z };
        vlJson["volumeMax"] = { vlConfig.volumeMax.x, vlConfig.volumeMax.y, vlConfig.volumeMax.z };
        vlJson["minBrickWorldSize"] = vlConfig.minBrickWorldSize;
        vlJson["enabled"] = vlConfig.enabled;
        settingsJson["volumetricLightmap"] = vlJson;
        j["lightSettings"] = settingsJson;

        // Write to file
        std::ofstream file(filepath);
        if (!file.is_open()) {
            CFFLog::Error("Failed to open file for writing: %s", filepath.c_str());
            return false;
        }

        file << j.dump(2); // Pretty print with 2 spaces
        file.close();

        CFFLog::Info("Scene saved to: %s", filepath.c_str());
        return true;

    } catch (const std::exception& e) {
        CFFLog::Error("Failed to save scene: %s", e.what());
        return false;
    }
}

// ===========================
// Load CScene
// ===========================
bool CSceneSerializer::LoadScene(CScene& scene, const std::string& filepath) {
    try {
        scene.SetFilePath(filepath);
        // Read file
        std::ifstream file(filepath);
        if (!file.is_open()) {
            CFFLog::Error("Failed to open scene file: %s", filepath.c_str());
            return false;
        }

        json j;
        file >> j;
        file.close();

        // Clear existing CScene
        while (scene.GetWorld().Count() > 0) {
            scene.GetWorld().Destroy(0);
        }
        scene.SetSelected(-1);

        // Load light settings (just deserialize, don't apply - CScene::LoadFromFile handles that)
        if (j.contains("lightSettings")) {
            const auto& settingsJson = j["lightSettings"];
            if (settingsJson.contains("skyboxAssetPath")) {
                scene.GetLightSettings().skyboxAssetPath = settingsJson["skyboxAssetPath"].get<std::string>();
            }
            if (settingsJson.contains("diffuseGIMode")) {
                scene.GetLightSettings().diffuseGIMode = static_cast<EDiffuseGIMode>(settingsJson["diffuseGIMode"].get<int>());
            }
            if (settingsJson.contains("gBufferDebugMode")) {
                scene.GetLightSettings().gBufferDebugMode = static_cast<EGBufferDebugMode>(settingsJson["gBufferDebugMode"].get<int>());
            }

            // Load Volumetric Lightmap config
            if (settingsJson.contains("volumetricLightmap")) {
                const auto& vlJson = settingsJson["volumetricLightmap"];
                auto& vlConfig = scene.GetLightSettings().volumetricLightmap;

                if (vlJson.contains("volumeMin") && vlJson["volumeMin"].is_array() && vlJson["volumeMin"].size() == 3) {
                    vlConfig.volumeMin.x = vlJson["volumeMin"][0].get<float>();
                    vlConfig.volumeMin.y = vlJson["volumeMin"][1].get<float>();
                    vlConfig.volumeMin.z = vlJson["volumeMin"][2].get<float>();
                }
                if (vlJson.contains("volumeMax") && vlJson["volumeMax"].is_array() && vlJson["volumeMax"].size() == 3) {
                    vlConfig.volumeMax.x = vlJson["volumeMax"][0].get<float>();
                    vlConfig.volumeMax.y = vlJson["volumeMax"][1].get<float>();
                    vlConfig.volumeMax.z = vlJson["volumeMax"][2].get<float>();
                }
                if (vlJson.contains("minBrickWorldSize")) {
                    vlConfig.minBrickWorldSize = vlJson["minBrickWorldSize"].get<float>();
                }
                if (vlJson.contains("enabled")) {
                    vlConfig.enabled = vlJson["enabled"].get<bool>();
                }
            }
        }

        // Load GameObjects
        if (j.contains("gameObjects") && j["gameObjects"].is_array()) {
            for (const auto& goJson : j["gameObjects"]) {
                std::string name = goJson.value("name", "GameObject");
                auto* go = scene.GetWorld().Create(name);

                // Load components
                if (goJson.contains("components") && goJson["components"].is_array()) {
                    for (const auto& compJson : goJson["components"]) {
                        DeserializeComponent(go, compJson);
                    }
                }
            }
        }

        CFFLog::Info("Scene loaded from: %s", filepath.c_str());
        return true;

    } catch (const std::exception& e) {
        CFFLog::Error("Failed to load scene: %s", e.what());
        return false;
    }
}

// ===========================
// Serialize Single GameObject to JSON String
// ===========================
std::string CSceneSerializer::SerializeGameObject(const CGameObject* go) {
    if (!go) return "";

    try {
        json j;
        j["name"] = go->GetName();
        j["components"] = json::array();

        // Serialize all components
        go->ForEachComponent([&](const CComponent* comp) {
            json compJson;
            SerializeComponent(comp, compJson);
            j["components"].push_back(compJson);
        });

        return j.dump();  // Compact JSON string

    } catch (const std::exception& e) {
        CFFLog::Error("Failed to serialize GameObject: %s", e.what());
        return "";
    }
}

// ===========================
// Deserialize GameObject from JSON String
// ===========================
CGameObject* CSceneSerializer::DeserializeGameObject(CWorld& world, const std::string& jsonString) {
    if (jsonString.empty()) return nullptr;

    try {
        json j = json::parse(jsonString);

        std::string name = j.value("name", "GameObject");
        auto* go = world.Create(name);

        // Load components
        if (j.contains("components") && j["components"].is_array()) {
            for (const auto& compJson : j["components"]) {
                DeserializeComponent(go, compJson);
            }
        }

        return go;

    } catch (const std::exception& e) {
        CFFLog::Error("Failed to deserialize GameObject: %s", e.what());
        return nullptr;
    }
}







