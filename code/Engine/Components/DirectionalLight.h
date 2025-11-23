#pragma once
#include "Component.h"
#include "PropertyVisitor.h"
#include "ComponentRegistry.h"
#include "Transform.h"
#include <DirectXMath.h>

// DirectionalLight: Directional light source (like the sun)
// Direction is controlled by the GameObject's Transform rotation
// Automatically casts shadows when added to scene
struct SDirectionalLight : public CComponent {
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};  // Light color (RGB)
    float intensity = 1.0f;                      // Light intensity multiplier
    float ibl_intensity = 1.0f;                   // IBL (ambient) intensity multiplier (global)

    // Shadow parameters (DirectionalLight always casts shadows)
    int shadow_map_size_index = 1;       // 0=1024, 1=2048, 2=4096 (default: 2048)
    float shadow_distance = 100.0f;    // Maximum shadow distance from camera (in camera space)
    float shadow_bias = 0.005f;        // Depth bias to prevent shadow acne
    bool enable_soft_shadows = true;    // Enable PCF for soft shadow edges (3x3 sampling)

    // CSM (Cascaded Shadow Maps) parameters
    int cascade_count = 4;             // Number of cascades (1-4, default: 4)
    float cascade_split_lambda = 0.95f; // Split scheme balance (0=uniform, 1=log, default: 0.95)
    float shadow_near_plane_offset = 50.0f; // Near plane offset to capture tall objects (similar to Unity)
    float cascade_blend_range = 0.0f;   // Blend range at cascade boundaries (0=off, 0.1=10% blend, reduces seams)
    bool debug_show_cascades = false;   // Debug: visualize cascade levels with colors

    const char* GetTypeName() const override { return "DirectionalLight"; }

    void VisitProperties(CPropertyVisitor& visitor) override {
        visitor.VisitFloat3("Color", color);
        visitor.VisitFloatSlider("Intensity", intensity, 0.0f, 10.0f);
        visitor.VisitFloatSlider("IBL Intensity", ibl_intensity, 0.0f, 10.0f);

        // Shadow parameters
        visitor.VisitEnum("Shadow Map Size", shadow_map_size_index, {"1024", "2048", "4096"});
        visitor.VisitFloat("Shadow Distance", shadow_distance);
        visitor.VisitFloat("Shadow Bias", shadow_bias);
        visitor.VisitBool("Enable Soft Shadows", enable_soft_shadows);

        // CSM parameters
        visitor.VisitInt("Cascade Count", cascade_count);
        visitor.VisitFloat("Split Lambda", cascade_split_lambda);
        visitor.VisitFloat("Near Plane Offset", shadow_near_plane_offset);
        visitor.VisitFloat("Cascade Blend Range", cascade_blend_range);
        visitor.VisitBool("Debug Show Cascades", debug_show_cascades);
    }

    // Get actual shadow map resolution from index
    int GetShadowMapResolution() const {
        switch (shadow_map_size_index) {
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
        auto* transform = GetOwner() ? GetOwner()->GetComponent<STransform>() : nullptr;
        if (!transform) {
            // Default direction if no Transform
            return XMFLOAT3(0.0f, -1.0f, 0.0f);
        }

        // Calculate forward vector from rotation (DirectX uses -Z as forward)
        // XMMatrixRotationRollPitchYaw parameter order: (Pitch, Yaw, Roll)
        XMMATRIX R = XMMatrixRotationRollPitchYaw(
            transform->rotationEuler.x,  // pitch
            transform->rotationEuler.y,  // yaw
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
REGISTER_COMPONENT(SDirectionalLight)
