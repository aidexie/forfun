#pragma once
#include "../Component.h"
#include "../PropertyVisitor.h"
#include <DirectXMath.h>
#include <string>
#include <cstddef>
#include <vector>
class Renderer;

enum class MeshKind { Cube, Obj };

struct MeshRenderer : public Component {
    MeshKind kind = MeshKind::Cube;
    std::string path; // used if kind == Obj
    std::vector<size_t> indices;        // 渲染器内部子网格索引（glTF 可能多个）- 不暴露给编辑器

    // Ensure GPU mesh exists in renderer, returns true if ok
    bool EnsureUploaded(Renderer& r, const DirectX::XMMATRIX& world);
    // Update transform in renderer
    void UpdateTransform(Renderer& r, const DirectX::XMMATRIX& world);
    const char* GetTypeName() const override { return "MeshRenderer"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        // Save old values to detect changes
        MeshKind oldKind = kind;
        std::string oldPath = path;

        // Expose mesh kind as enum
        int kindValue = static_cast<int>(kind);
        static const std::vector<const char*> kindOptions = { "Cube", "Obj" };
        visitor.VisitEnum("Mesh Kind", kindValue, kindOptions);
        kind = static_cast<MeshKind>(kindValue);

        // Expose path with browse button (only relevant when kind == Obj)
        visitor.VisitFilePath("Path", path, "Mesh Files\0*.obj;*.gltf;*.glb\0OBJ Files\0*.obj\0glTF Files\0*.gltf;*.glb\0All Files\0*.*\0");

        // If kind or path changed, mark for reload
        if (kind != oldKind || path != oldPath) {
            indices.clear(); // Clear to trigger reload in EnsureUploaded
        }
    }
};
