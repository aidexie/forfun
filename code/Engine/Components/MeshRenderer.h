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

struct SMeshRenderer : public CComponent {
    std::string path; // Path to mesh file (.obj, .gltf, .glb)
    std::vector<std::shared_ptr<GpuMeshResource>> meshes; // GPU resources (glTF may have multiple sub-meshes)

    // Debug: show bounds wireframe in viewport
    bool showBounds = false;

    // Ensure GPU mesh exists, returns true if ok
    bool EnsureUploaded();

    // Get local space AABB from mesh resource (read-only access)
    bool GetLocalBounds(DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax) const;

    const char* GetTypeName() const override { return "MeshRenderer"; }

    void VisitProperties(CPropertyVisitor& visitor) override {
        // Save old value to detect changes
        std::string oldPath = path;

        // Expose path with browse button
        visitor.VisitFilePath("Path", path, "Mesh Files\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");

        // If path changed, mark for reload
        if (path != oldPath) {
            meshes.clear(); // Clear to trigger reload in EnsureUploaded
        }

        // Show local bounds info (read-only) from mesh resource
        DirectX::XMFLOAT3 localMin, localMax;
        if (GetLocalBounds(localMin, localMax)) {
            visitor.VisitFloat3ReadOnly("Local Bounds Min", localMin);
            visitor.VisitFloat3ReadOnly("Local Bounds Max", localMax);
        }

        // Debug option
        visitor.VisitBool("Show Bounds", showBounds);
    }
};

// Auto-register component
REGISTER_COMPONENT(SMeshRenderer)
