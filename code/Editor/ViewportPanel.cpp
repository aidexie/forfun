#include "Panels.h"
#include "imgui.h"
#include "ImGuizmo.h"
#include "Camera.h"
#include "Offscreen.h"
#include "DX11Context.h"
#include "Scene.h"
#include "GameObject.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Engine/Rendering/ForwardRenderPipeline.h"
#include "Rendering/GridPass.h"
#include "PickingUtils.h"
#include "Core/FFLog.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ViewportPanel.cpp  —— file-scope statics
static ImVec2 s_lastAvail = ImVec2(0, 0);

// Transform Gizmo state
static ImGuizmo::OPERATION s_gizmoOperation = ImGuizmo::TRANSLATE;
static ImGuizmo::MODE s_gizmoMode = ImGuizmo::WORLD;
static bool s_useSnap = false;
static float s_snapTranslate[3] = {1.0f, 1.0f, 1.0f};
static float s_snapRotate = 15.0f;  // degrees
static float s_snapScale = 0.5f;

ImVec2 Panels::GetViewportLastSize() {
    return s_lastAvail;
}

void Panels::DrawViewport(CScene& scene, CCamera& editorCam,  // ✅ 改用 CCamera
    ID3D11ShaderResourceView* srv,
    size_t srcWidth, size_t srcHeight,
    CForwardRenderPipeline* pipeline)
{
    ImGui::Begin("Viewport");

    // ============================================
    // Keyboard Shortcuts for Gizmo Modes
    // ============================================
    if (ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) {
            s_gizmoOperation = ImGuizmo::TRANSLATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E)) {
            s_gizmoOperation = ImGuizmo::ROTATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            s_gizmoOperation = ImGuizmo::SCALE;
        }
    }

    // ============================================
    // Gizmo Toolbar
    // ============================================

    // Gizmo operation buttons (with highlight for active mode)
    bool isTranslate = (s_gizmoOperation == ImGuizmo::TRANSLATE);
    bool isRotate = (s_gizmoOperation == ImGuizmo::ROTATE);
    bool isScale = (s_gizmoOperation == ImGuizmo::SCALE);

    if (isTranslate) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 0.4f, 1.0f));
    if (ImGui::Button("Translate (W)", ImVec2(100, 0))) {
        s_gizmoOperation = ImGuizmo::TRANSLATE;
    }
    if (isTranslate) ImGui::PopStyleColor();
    ImGui::SameLine();

    if (isRotate) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 0.4f, 1.0f));
    if (ImGui::Button("Rotate (E)", ImVec2(100, 0))) {
        s_gizmoOperation = ImGuizmo::ROTATE;
    }
    if (isRotate) ImGui::PopStyleColor();
    ImGui::SameLine();

    if (isScale) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.6f, 0.4f, 1.0f));
    if (ImGui::Button("Scale (R)", ImVec2(100, 0))) {
        s_gizmoOperation = ImGuizmo::SCALE;
    }
    if (isScale) ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::Separator();
    ImGui::SameLine();

    // Local/World toggle
    const char* modeText = (s_gizmoMode == ImGuizmo::WORLD) ? "World" : "Local";
    if (ImGui::Button(modeText, ImVec2(60, 0))) {
        s_gizmoMode = (s_gizmoMode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }
    ImGui::SameLine();

    // Snapping toggle
    ImGui::Checkbox("Snap", &s_useSnap);

    // Snap settings (when enabled)
    if (s_useSnap) {
        ImGui::SameLine();
        ImGui::PushItemWidth(60);

        if (s_gizmoOperation == ImGuizmo::TRANSLATE) {
            // For translate, show unified snap value
            if (ImGui::DragFloat("##snapTrans", &s_snapTranslate[0], 0.1f, 0.01f, 10.0f, "%.2f")) {
                // Apply to all axes
                s_snapTranslate[1] = s_snapTranslate[0];
                s_snapTranslate[2] = s_snapTranslate[0];
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Translate snap (meters)");
        }
        else if (s_gizmoOperation == ImGuizmo::ROTATE) {
            // For rotate, show angle snap
            ImGui::DragFloat("##snapRot", &s_snapRotate, 1.0f, 1.0f, 90.0f, "%.0f°");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation snap (degrees)");
        }
        else if (s_gizmoOperation == ImGuizmo::SCALE) {
            // For scale, show scale snap
            ImGui::DragFloat("##snapScale", &s_snapScale, 0.05f, 0.01f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale snap");
        }

        ImGui::PopItemWidth();
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Show Grid toggle
    bool gridEnabled = CGridPass::Instance().IsEnabled();
    if (ImGui::Checkbox("Show Grid", &gridEnabled)) {
        CGridPass::Instance().SetEnabled(gridEnabled);
    }

    ImGui::Separator();

    // Measure current available size and remember it for the next frame's render pass
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.f) avail.x = 1.f;
    if (avail.y < 1.f) avail.y = 1.f;
    s_lastAvail = avail;

    // Keep editor camera's aspect in sync with the panel (optional)
    editorCam.aspectRatio = (avail.y > 0.f) ? (avail.x / avail.y) : editorCam.aspectRatio;  // ✅ aspect → aspectRatio

    // Draw the provided texture (no ownership). If null, show placeholder.
    ImVec2 imagePos = ImGui::GetCursorScreenPos();

    if (srv && srcWidth > 0 && srcHeight > 0) {
        ImGui::Image((ImTextureID)srv, avail, ImVec2(0, 0), ImVec2(1, 1));
    }
    else {
        ImGui::TextUnformatted("No viewport image.");
    }

    // ============================================
    // Custom View Orientation Gizmo (Top-Right Corner)
    // ============================================
    if (pipeline) {
        // Gizmo properties
        const float gizmoSize = 120.0f;
        const ImVec2 gizmoPos(imagePos.x + avail.x - gizmoSize - 15, imagePos.y + 15);
        const ImVec2 center(gizmoPos.x + gizmoSize * 0.5f, gizmoPos.y + gizmoSize * 0.5f);
        const float axisLength = gizmoSize * 0.35f;

        // ✅ Get camera view matrix from camera parameter
        XMMATRIX viewMat = editorCam.GetViewMatrix();
        XMFLOAT4X4 view;
        XMStoreFloat4x4(&view, viewMat);

        // Extract world axes in camera space from view matrix
        // View matrix rotates world to camera, so columns give us world axes directions
        XMFLOAT3 worldX(view._11, view._21, view._31);  // World X axis in camera space
        XMFLOAT3 worldY(view._12, view._22, view._32);  // World Y axis in camera space
        // In DirectX left-handed system, camera looks down +Z, so we need to flip the Z axis
        XMFLOAT3 worldZ(-view._13, -view._23, -view._33);  // World Z axis in camera space (flipped)

        // Get draw list
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Draw background circle
        drawList->AddCircleFilled(center, gizmoSize * 0.5f, IM_COL32(40, 40, 40, 200), 32);
        drawList->AddCircle(center, gizmoSize * 0.5f, IM_COL32(80, 80, 80, 255), 32, 2.0f);

        // Helper lambda to draw axis line with label
        auto DrawAxis = [&](const XMFLOAT3& dir, const char* label, ImU32 colorPos, ImU32 colorNeg) {
            // Project 3D direction to 2D screen space
            ImVec2 endPos(center.x + dir.x * axisLength, center.y - dir.y * axisLength);
            ImVec2 endNeg(center.x - dir.x * axisLength, center.y + dir.y * axisLength);

            // Determine if axis is facing camera (for label visibility only)
            // In camera space, forward is (0,0,1), so just use dir.z
            float dotPos = dir.z;

            // Always draw negative direction (thinner, gray)
            drawList->AddLine(center, endNeg, colorNeg, 3.0f);

            // Always draw positive direction (thicker, colored)
            drawList->AddLine(center, endPos, colorPos, 5.0f);

            // Draw arrowhead for positive direction
            XMFLOAT2 arrowDir(dir.x, -dir.y);
            float len = sqrtf(arrowDir.x * arrowDir.x + arrowDir.y * arrowDir.y);
            if (len > 0.001f) {
                arrowDir.x /= len;
                arrowDir.y /= len;
            }
            XMFLOAT2 arrowPerp(-arrowDir.y, arrowDir.x);

            ImVec2 tip = endPos;
            ImVec2 base1(endPos.x - arrowDir.x * 8 + arrowPerp.x * 4,
                         endPos.y - arrowDir.y * 8 + arrowPerp.y * 4);
            ImVec2 base2(endPos.x - arrowDir.x * 8 - arrowPerp.x * 4,
                         endPos.y - arrowDir.y * 8 - arrowPerp.y * 4);
            drawList->AddTriangleFilled(tip, base1, base2, colorPos);

            // Draw label at the end (only if facing camera)
            if (dotPos > 0.3f) {
                ImVec2 labelPos(endPos.x + 8, endPos.y - 8);
                drawList->AddText(labelPos, colorPos, label);
            }
        };

        // Draw axes (draw order: farthest to nearest for proper occlusion)
        // Calculate depth for each axis (use Z component in camera space as depth)
        float depthX = worldX.z;
        float depthY = worldY.z;
        float depthZ = worldZ.z;

        // Sort and draw (simple bubble sort for 3 items)
        struct AxisData { float depth; int index; };
        AxisData axes[3] = {{depthX, 0}, {depthY, 1}, {depthZ, 2}};

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2 - i; j++) {
                if (axes[j].depth > axes[j + 1].depth) {
                    AxisData temp = axes[j];
                    axes[j] = axes[j + 1];
                    axes[j + 1] = temp;
                }
            }
        }

        // Draw in depth order (back to front)
        // Negative directions all use same gray color for clarity
        const ImU32 negativeColor = IM_COL32(120, 120, 120, 180);

        for (int i = 0; i < 3; i++) {
            if (axes[i].index == 0) {
                // X axis (Red)
                DrawAxis(worldX, "X", IM_COL32(255, 60, 60, 255), negativeColor);
            } else if (axes[i].index == 1) {
                // Y axis (Green)
                DrawAxis(worldY, "Y", IM_COL32(100, 255, 100, 255), negativeColor);
            } else {
                // Z axis (Blue)
                DrawAxis(worldZ, "Z", IM_COL32(80, 150, 255, 255), negativeColor);
            }
        }

        // Draw center dot
        drawList->AddCircleFilled(center, 4.0f, IM_COL32(255, 255, 255, 255));
    }

    // ============================================
    // Transform Gizmo (ImGuizmo)
    // ============================================
    if (pipeline) {
        CGameObject* selectedObj = scene.GetSelectedObject();

        // Enable/disable gizmo based on selection
        ImGuizmo::Enable(selectedObj != nullptr);

        if (selectedObj) {
            STransform* transform = selectedObj->GetComponent<STransform>();
            if (transform) {
                // Set ImGuizmo to draw in the viewport window
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist();

                // Get viewport rect
                ImGuizmo::SetRect(imagePos.x, imagePos.y, avail.x, avail.y);

                // ✅ Get camera matrices from camera parameter
                XMMATRIX view = editorCam.GetViewMatrix();
                XMMATRIX proj = editorCam.GetProjectionMatrix();

                // Convert to float arrays (ImGuizmo expects row-major float[16])
                XMFLOAT4X4 viewF, projF;
                XMStoreFloat4x4(&viewF, view);
                XMStoreFloat4x4(&projF, proj);

                // Get object world matrix
                XMMATRIX worldMat = transform->WorldMatrix();
                XMFLOAT4X4 worldF;
                XMStoreFloat4x4(&worldF, worldMat);

                // Prepare snap values
                float* snapValues = nullptr;
                if (s_useSnap) {
                    if (s_gizmoOperation == ImGuizmo::TRANSLATE) {
                        snapValues = s_snapTranslate;
                    } else if (s_gizmoOperation == ImGuizmo::ROTATE) {
                        snapValues = &s_snapRotate;
                    } else if (s_gizmoOperation == ImGuizmo::SCALE) {
                        snapValues = &s_snapScale;
                    }
                }

                // Get delta matrix for incremental updates
                XMFLOAT4X4 deltaMatF;
                bool manipulated = ImGuizmo::Manipulate(
                    &viewF._11, &projF._11,
                    s_gizmoOperation, s_gizmoMode,
                    &worldF._11,
                    &deltaMatF._11,  // Get delta matrix
                    snapValues);

                if (manipulated) {
                    // Decompose ImGuizmo's world matrix to extract T, R, S
                    XMMATRIX newWorld = XMLoadFloat4x4(&worldF);
                    XMVECTOR scaleVec, rotationQuat, translationVec;

                    if (XMMatrixDecompose(&scaleVec, &rotationQuat, &translationVec, newWorld)) {
                        // Update position and scale
                        XMStoreFloat3(&transform->position, translationVec);
                        XMStoreFloat3(&transform->scale, scaleVec);

                        // Convert quaternion to rotation matrix
                        XMMATRIX rotMat = XMMatrixRotationQuaternion(rotationQuat);
                        XMFLOAT4X4 m;
                        XMStoreFloat4x4(&m, rotMat);

                        // Extract Euler angles from rotation matrix
                        // For XMMatrixRotationRollPitchYaw(pitch, yaw, roll) order
                        float pitch, yaw, roll;
                        float sinPitch = -m._31;

                        if (fabsf(sinPitch) >= 0.9999f) {
                            // Gimbal lock case
                            pitch = copysignf(XM_PIDIV2, sinPitch);
                            yaw = atan2f(-m._13, m._33);
                            roll = 0.0f;
                        } else {
                            pitch = asinf(sinPitch);
                            yaw = atan2f(m._32, m._33);
                            roll = atan2f(m._12, m._11);  // Use m._12 (sin) not m._21
                        }

                        transform->rotationEuler = XMFLOAT3(pitch, yaw, roll);
                    }
                }
            }
        }
    }

    // ============================================
    // Mouse Picking (Object Selection)
    // ============================================
    // Only process clicks when:
    // - Left mouse button clicked
    // - Mouse is over viewport image
    // - Not manipulating gizmo
    // - Not over other ImGui UI
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsItemHovered() &&
        !ImGuizmo::IsUsing() &&
        !ImGui::IsAnyItemActive() &&
        pipeline)
    {
        // Get mouse position relative to viewport image
        ImVec2 mousePos = ImGui::GetMousePos();
        float mouseX = mousePos.x - imagePos.x;
        float mouseY = mousePos.y - imagePos.y;

        // Check if mouse is within viewport bounds
        if (mouseX >= 0 && mouseX < avail.x && mouseY >= 0 && mouseY < avail.y) {
            // ✅ Generate ray from screen coordinates using camera parameter
            XMMATRIX view = editorCam.GetViewMatrix();
            XMMATRIX proj = editorCam.GetProjectionMatrix();

            PickingUtils::Ray ray = PickingUtils::GenerateRayFromScreen(
                mouseX, mouseY, avail.x, avail.y, view, proj);

            // Find closest intersected object
            float closestDistance = FLT_MAX;
            int closestObjectIndex = -1;

            const auto& objects = scene.GetWorld().Objects();

            for (size_t i = 0; i < objects.size(); ++i) {
                CGameObject* obj = objects[i].get();
                if (!obj) continue;

                // Get transform and mesh renderer
                STransform* transform = obj->GetComponent<STransform>();
                SMeshRenderer* meshRenderer = obj->GetComponent<SMeshRenderer>();

                if (!transform || !meshRenderer) continue;

                // Get local-space AABB from mesh
                XMFLOAT3 localMin, localMax;
                if (!meshRenderer->GetLocalBounds(localMin, localMax)) {
                    continue;  // No bounds available
                }

                // Transform AABB to world space
                XMMATRIX worldMat = transform->WorldMatrix();
                XMFLOAT3 worldMin, worldMax;
                PickingUtils::TransformAABB(localMin, localMax, worldMat, worldMin, worldMax);

                // Test ray intersection with world-space AABB
                std::optional<float> hitDistance = PickingUtils::RayAABBIntersect(ray, worldMin, worldMax);

                if (hitDistance.has_value() && hitDistance.value() < closestDistance) {
                    closestDistance = hitDistance.value();
                    closestObjectIndex = static_cast<int>(i);
                }
            }

            // Update selection (only if we hit something)
            if (closestObjectIndex >= 0) {
                scene.SetSelected(closestObjectIndex);
            }
        }
    }

    ImGui::End();
}

