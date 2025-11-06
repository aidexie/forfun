#pragma once
#include <string>

// Forward declarations
struct Scene;

// Scene serialization to/from JSON files
class SceneSerializer {
public:
    // Save scene to JSON file
    static bool SaveScene(const Scene& scene, const std::string& filepath);

    // Load scene from JSON file (clears existing scene)
    static bool LoadScene(Scene& scene, const std::string& filepath);
};
