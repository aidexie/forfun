#pragma once
#include <string>

// Forward declarations
class CScene;

// CScene serialization to/from JSON files
class CSceneSerializer {
public:
    // Save scene to JSON file
    static bool SaveScene(const CScene& scene, const std::string& filepath);

    // Load scene from JSON file (clears existing CScene)
    static bool LoadScene(CScene& scene, const std::string& filepath);
};



