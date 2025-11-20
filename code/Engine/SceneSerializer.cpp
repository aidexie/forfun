// SceneSerializer.cpp
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
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

// ===========================
// JSON Write Visitor
// ===========================
class JsonWriteVisitor : public PropertyVisitor {
public:
    JsonWriteVisitor(json& j) : m_json(j) {}

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
class JsonReadVisitor : public PropertyVisitor {
public:
    JsonReadVisitor(const json& j) : m_json(j) {}

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
// Serialize Component
// ===========================
static void SerializeComponent(const Component* comp, json& j) {
    j["type"] = comp->GetTypeName();
    JsonWriteVisitor visitor(j);
    // const_cast is safe here: JsonWriteVisitor only reads values, doesn't modify component
    const_cast<Component*>(comp)->VisitProperties(visitor);
}

// ===========================
// Centralized Component Factory
// ===========================
// Automatically uses ComponentRegistry (no manual code needed!)
// Components auto-register via REGISTER_COMPONENT macro in their header files
static Component* CreateComponentByType(GameObject* go, const std::string& typeName) {
    return ComponentRegistry::Instance().Create(go, typeName);
}

// ===========================
// Deserialize Component
// ===========================
static Component* DeserializeComponent(GameObject* go, const json& j) {
    if (!j.contains("type")) return nullptr;

    std::string type = j["type"].get<std::string>();
    Component* comp = CreateComponentByType(go, type);

    if (comp) {
        // Load properties using reflection
        JsonReadVisitor visitor(j);
        comp->VisitProperties(visitor);
    }

    return comp;
}

// ===========================
// Save Scene
// ===========================
bool SceneSerializer::SaveScene(const Scene& scene, const std::string& filepath) {
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
            go->ForEachComponent([&](const Component* comp) {
                json compJson;
                SerializeComponent(comp, compJson);
                goJson["components"].push_back(compJson);
            });

            j["gameObjects"].push_back(goJson);
        }

        // Write to file
        std::ofstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for writing: " << filepath << std::endl;
            return false;
        }

        file << j.dump(2); // Pretty print with 2 spaces
        file.close();

        std::cout << "Scene saved to: " << filepath << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error saving scene: " << e.what() << std::endl;
        return false;
    }
}

// ===========================
// Load Scene
// ===========================
bool SceneSerializer::LoadScene(Scene& scene, const std::string& filepath) {
    try {
        // Read file
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for reading: " << filepath << std::endl;
            return false;
        }

        json j;
        file >> j;
        file.close();

        // Clear existing scene
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

        std::cout << "Scene loaded from: " << filepath << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error loading scene: " << e.what() << std::endl;
        return false;
    }
}
