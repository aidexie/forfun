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
    float IblIntensity = 1.0f;                   // IBL (ambient) intensity multiplier (global)

    // Shadow parameters (DirectionalLight always casts shadows)
    int ShadowMapSizeIndex = 1;       // 0=1024, 1=2048, 2=4096 (default: 2048)
    float ShadowDistance = 100.0f;    // Maximum shadow distance from camera (in camera space)
    float ShadowBias = 0.005f;        // Depth bias to prevent shadow acne
    bool EnableSoftShadows = true;    // Enable PCF for soft shadow edges (3x3 sampling)

    // CSM (Cascaded Shadow Maps) parameters
    int CascadeCount = 4;             // Number of cascades (1-4, default: 4)
    float CascadeSplitLambda = 0.95f; // Split scheme balance (0=uniform, 1=log, default: 0.95)
    float ShadowNearPlaneOffset = 50.0f; // Near plane offset to capture tall objects (similar to Unity)
    float CascadeBlendRange = 0.0f;   // Blend range at cascade boundaries (0=off, 0.1=10% blend, reduces seams)
    bool DebugShowCascades = false;   // Debug: visualize cascade levels with colors

    const char* GetTypeName() const override { return "DirectionalLight"; }

    void VisitProperties(PropertyVisitor& visitor) override {
        visitor.VisitFloat3("Color", Color);
        visitor.VisitFloatSlider("Intensity", Intensity, 0.0f, 10.0f);
        visitor.VisitFloatSlider("IBL Intensity", IblIntensity, 0.0f, 10.0f);

        // Shadow parameters
        visitor.VisitEnum("Shadow Map Size", ShadowMapSizeIndex, {"1024", "2048", "4096"});
        visitor.VisitFloat("Shadow Distance", ShadowDistance);
        visitor.VisitFloat("Shadow Bias", ShadowBias);
        visitor.VisitBool("Enable Soft Shadows", EnableSoftShadows);

        // CSM parameters
        visitor.VisitInt("Cascade Count", CascadeCount);
        visitor.VisitFloat("Split Lambda", CascadeSplitLambda);
        visitor.VisitFloat("Near Plane Offset", ShadowNearPlaneOffset);
        visitor.VisitFloat("Cascade Blend Range", CascadeBlendRange);
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
