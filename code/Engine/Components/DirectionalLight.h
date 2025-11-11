#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include <DirectXMath.h>

// DirectionalLight: Directional light source (like the sun)
// Direction is controlled by the GameObject's Transform rotation
// Automatically casts shadows when added to scene
struct DirectionalLight : public Component {
    DirectX::XMFLOAT3 Color{1.0f, 1.0f, 1.0f};  // Light color (RGB)
    float Intensity = 1.0f;                      // Light intensity multiplier

    // Shadow parameters (DirectionalLight always casts shadows)
    int ShadowMapSizeIndex = 1;       // 0=1024, 1=2048, 2=4096 (default: 2048)
    float ShadowDistance = 100.0f;    // Maximum shadow distance from camera (in camera space)
    float ShadowBias = 0.005f;        // Depth bias to prevent shadow acne

    // CSM (Cascaded Shadow Maps) parameters
    int CascadeCount = 4;             // Number of cascades (1-4, default: 4)
    float CascadeSplitLambda = 0.95f; // Split scheme balance (0=uniform, 1=log, default: 0.95)
    bool DebugShowCascades = false;   // Debug: visualize cascade levels with colors

    const char* GetTypeName() const override { return "DirectionalLight"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat3("Color", Color);
        visitor.VisitFloat("Intensity", Intensity);

        // Shadow parameters
        visitor.VisitEnum("Shadow Map Size", ShadowMapSizeIndex, {"1024", "2048", "4096"});
        visitor.VisitFloat("Shadow Distance", ShadowDistance);
        visitor.VisitFloat("Shadow Bias", ShadowBias);

        // CSM parameters
        visitor.VisitInt("Cascade Count", CascadeCount);
        visitor.VisitFloat("Split Lambda", CascadeSplitLambda);
        visitor.VisitBool("Debug Show Cascades", DebugShowCascades);
    }

    // Get actual shadow map resolution from index
    int GetShadowMapResolution() const {
        switch (ShadowMapSizeIndex) {
            case 0: return 1024;
            case 2: return 4096;
            default: return 2048;  // case 1 or invalid
        }
    }

    // Get light direction in world space from Transform component
    // Returns normalized direction vector (Transform's forward direction)
    DirectX::XMFLOAT3 GetDirection() const {
        using namespace DirectX;

        // Get Transform component from owner GameObject
        auto* transform = GetOwner() ? GetOwner()->GetComponent<Transform>() : nullptr;
        if (!transform) {
            // Default direction if no Transform
            return XMFLOAT3(0.0f, -1.0f, 0.0f);
        }

        // Calculate forward vector from rotation (DirectX uses -Z as forward)
        XMMATRIX R = XMMatrixRotationRollPitchYaw(
            transform->rotationEuler.y,  // yaw
            transform->rotationEuler.x,  // pitch
            transform->rotationEuler.z   // roll
        );

        // Transform forward vector (-Z direction) by rotation
        XMVECTOR forward = XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f);
        XMVECTOR dirVec = XMVector3TransformNormal(forward, R);
        dirVec = XMVector3Normalize(dirVec);

        XMFLOAT3 result;
        XMStoreFloat3(&result, dirVec);
        return result;
    }
};

// Auto-register component
REGISTER_COMPONENT(DirectionalLight)
