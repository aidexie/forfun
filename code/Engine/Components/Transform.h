#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include <DirectXMath.h>

struct Transform : public Component {
    DirectX::XMFLOAT3 position{0,0,0};
    DirectX::XMFLOAT3 rotationEuler{0,0,0}; // radians
    DirectX::XMFLOAT3 scale{1,1,1};

    DirectX::XMMATRIX WorldMatrix() const {
        using namespace DirectX;
        XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(rotationEuler.y, rotationEuler.x, rotationEuler.z); // yaw(y), pitch(x), roll(z)
        XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
        return S * R * T;
    }

    const char* GetTypeName() const override { return "Transform"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat3("Position", position);
        visitor.VisitFloat3("Rotation", rotationEuler);
        visitor.VisitFloat3("Scale", scale);
    }
};
