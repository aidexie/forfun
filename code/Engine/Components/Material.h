#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>

// Material component - defines PBR material properties
// Each GameObject can have its own material parameters
struct SMaterial : public CComponent {
    DirectX::XMFLOAT3 albedo{1.0f, 1.0f, 1.0f};  // Base color (sRGB, will be converted to linear in shader)
    float metallic = 0.0f;   // Metallic: 0=dielectric (default), 1=metal
    float roughness = 0.5f;  // Roughness: 0=smooth/mirror, 1=rough/matte (default: 0.5)

    const char* GetTypeName() const override { return "Material"; }

    void VisitProperties(CPropertyVisitor& visitor) override {
        visitor.VisitFloat3("Albedo", albedo);
        visitor.VisitFloatSlider("Metallic", metallic, 0.0f, 1.0f);
        visitor.VisitFloatSlider("Roughness", roughness, 0.0f, 1.0f);
    }
};

// Auto-register component
REGISTER_COMPONENT(SMaterial)
