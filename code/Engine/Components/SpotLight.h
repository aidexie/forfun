#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>

// Spot light component for Clustered Shading
// Combines point light properties with directional cone
struct SSpotLight : public CComponent {
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};  // Linear RGB
    float intensity = 1.0f;                      // Luminous intensity (arbitrary units)
    float range = 10.0f;                         // Maximum light radius (for culling)

    // Spot light specific properties
    DirectX::XMFLOAT3 direction{0.0f, -1.0f, 0.0f};  // Local direction (default: down)
    float innerConeAngle = 15.0f;                     // Inner cone angle in degrees (full brightness)
    float outerConeAngle = 30.0f;                     // Outer cone angle in degrees (falloff to zero)

    const char* GetTypeName() const override { return "SpotLight"; }

    void VisitProperties(CPropertyVisitor& visitor) override {
        visitor.VisitFloat3("color", color);
        visitor.VisitFloat("intensity", intensity);
        visitor.VisitFloat("range", range);
        visitor.VisitFloat3("direction", direction);
        visitor.VisitFloat("innerConeAngle", innerConeAngle);
        visitor.VisitFloat("outerConeAngle", outerConeAngle);
    }
};

REGISTER_COMPONENT(SSpotLight)
