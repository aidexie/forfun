#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>

// Material component - defines PBR material properties
// Each GameObject can have its own material parameters
struct Material : public Component {
    DirectX::XMFLOAT3 Albedo{1.0f, 1.0f, 1.0f};  // Base color (sRGB, will be converted to linear in shader)
    float Metallic = 0.0f;   // Metallic: 0=dielectric, 1=metal
    float Roughness = 0.5f;  // Roughness: 0=smooth, 1=rough

    const char* GetTypeName() const override { return "Material"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat3("Albedo", Albedo);
        visitor.VisitFloatSlider("Metallic", Metallic, 0.0f, 1.0f);
        visitor.VisitFloatSlider("Roughness", Roughness, 0.0f, 1.0f);
    }
};

// Auto-register component
REGISTER_COMPONENT(Material)
