#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>

struct STransform : public CComponent {
    DirectX::XMFLOAT3 position{0,0,0};
    DirectX::XMFLOAT3 rotationEuler{0,0,0}; // (pitch, yaw, roll) in radians
    DirectX::XMFLOAT3 scale{1,1,1};

    DirectX::XMMATRIX WorldMatrix() const {
        using namespace DirectX;
        XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
        // XMMatrixRotationRollPitchYaw parameter order: (Pitch, Yaw, Roll)
        // rotationEuler storage: (pitch, yaw, roll)
        XMMATRIX R = XMMatrixRotationRollPitchYaw(rotationEuler.x, rotationEuler.y, rotationEuler.z);
        XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
        // DirectX row-major: P' = P * World, apply order: Scale -> Rotate -> Translate (SRT standard)
        return S * R * T;
    }

    DirectX::XMMATRIX GetRotationMatrix() const {
        using namespace DirectX;
        return XMMatrixRotationRollPitchYaw(rotationEuler.x, rotationEuler.y, rotationEuler.z);
    }

    void SetRotation(float pitch, float yaw, float roll) {
        // Convert degrees to radians
        rotationEuler.x = DirectX::XMConvertToRadians(pitch);
        rotationEuler.y = DirectX::XMConvertToRadians(yaw);
        rotationEuler.z = DirectX::XMConvertToRadians(roll);
    }

    const char* GetTypeName() const override { return "Transform"; }

    void VisitProperties(CPropertyVisitor& visitor) override {
        visitor.VisitFloat3("Position", position);
        visitor.VisitFloat3AsAngles("Rotation", rotationEuler);  // Display as degrees, store as radians
        visitor.VisitFloat3("Scale", scale);
    }
};

// Auto-register component
REGISTER_COMPONENT(STransform)
