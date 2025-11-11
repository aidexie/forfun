#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>
#include <string>
#include <memory>
#include <vector>

class Renderer;
class GpuMeshResource;

struct MeshRenderer : public Component {
    std::string path; // Path to mesh file (.obj, .gltf, .glb)
    std::vector<std::shared_ptr<GpuMeshResource>> meshes; // GPU resources (glTF may have multiple sub-meshes)

    // Ensure GPU mesh exists, returns true if ok
    bool EnsureUploaded();

    const char* GetTypeName() const override { return "MeshRenderer"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        // Save old value to detect changes
        std::string oldPath = path;

        // Expose path with browse button
        visitor.VisitFilePath("Path", path, "Mesh Files\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");

        // If path changed, mark for reload
        if (path != oldPath) {
            meshes.clear(); // Clear to trigger reload in EnsureUploaded
        }
    }
};

// Auto-register component
REGISTER_COMPONENT(MeshRenderer)
