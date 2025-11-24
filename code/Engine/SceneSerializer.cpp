// SceneSerializer.cpp
#include "Core/FFLog.h"
// NOTE: Requires nlohmann/json library
// Download json.hpp from: https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
// Place at: E:\forfun\thirdparty\nlohmann\json.hpp

#include "SceneSerializer.h"
#include "Scene.h"
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






