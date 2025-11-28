#pragma once
#include <string>

// Forward declarations
class CScene;
class CGameObject;
class CWorld;

// CScene serialization to/from JSON files
class CSceneSerializer {
public:
    // Save scene to JSON file
    static bool SaveScene(const CScene& scene, const std::string& filepath);

    // Load scene from JSON file (clears existing CScene)
    static bool LoadScene(CScene& scene, const std::string& filepath);

    // Serialize GameObject to JSON string (for clipboard copy)
    static std::string SerializeGameObject(const CGameObject* go);

    // Deserialize GameObject from JSON string and add to World (for clipboard paste)
    static CGameObject* DeserializeGameObject(CWorld& world, const std::string& jsonString);
};



