#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>

// Point light component for Clustered Shading
// Simple design: position (from Transform), color, intensity, range
struct SPointLight : public CComponent {
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};  // Linear RGB
    float intensity = 1.0f;                      // Luminous intensity (arbitrary units)
    float range = 10.0f;                         // Maximum light radius (for culling)

    const char* GetTypeName() const override { return "PointLight"; }

    void VisitProperties(CPropertyVisitor& visitor) override {
        visitor.VisitFloat3("color", color);
        visitor.VisitFloat("intensity", intensity);
        visitor.VisitFloat("range", range);
    }
};

REGISTER_COMPONENT(SPointLight)
