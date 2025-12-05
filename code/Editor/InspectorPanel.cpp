#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "GameObject.h"
#include "Component.h"
#include "PropertyVisitor.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include "Components/PointLight.h"
#include "Components/SpotLight.h"
#include "Components/ReflectionProbe.h"
#include "Components/LightProbe.h"
#include "Rendering/ReflectionProbeBaker.h"
#include "Rendering/LightProbeBaker.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"  // FFPath namespace
#include <string>
#include <windows.h>
#include <commdlg.h>

// Font Awesome icons
#define ICON_FA_FILE ""           //
#define ICON_FA_FOLDER_OPEN ""    //
#define ICON_FA_EDIT ""           //

// ImGui implementation of PropertyVisitor for reflection-based UI
class ImGuiPropertyVisitor : public CPropertyVisitor {
public:
    void VisitFloat(const char* name, float& value) override {
        ImGui::DragFloat(name, &value, 0.1f);
    }

    void VisitFloatSlider(const char* name, float& value, float min, float max) override {
        ImGui::SliderFloat(name, &value, min, max);
    }

    void VisitInt(const char* name, int& value) override {
        ImGui::DragInt(name, &value);
    }

    void VisitBool(const char* name, bool& value) override {
        ImGui::Checkbox(name, &value);
    }

    void VisitString(const char* name, std::string& value) override {
        char buf[260];
        snprintf(buf, sizeof(buf), "%s", value.c_str());
        if (ImGui::InputText(name, buf, sizeof(buf))) {
            value = buf;
        }
    }

    void VisitFloat3(const char* name, DirectX::XMFLOAT3& value) override {
        float arr[3] = { value.x, value.y, value.z };
        if (ImGui::DragFloat3(name, arr, 0.1f)) {
            value = { arr[0], arr[1], arr[2] };
        }
    }

    void VisitFloat3ReadOnly(const char* name, const DirectX::XMFLOAT3& value) override {
        // Display as read-only (disabled) input
        float arr[3] = { value.x, value.y, value.z };
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::InputFloat3(name, arr, "%.3f", ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor(2);
    }

    void VisitFloat3AsAngles(const char* name, DirectX::XMFLOAT3& valueRadians) override {
        // Convert radians to degrees for UI
        float degrees[3] = {
            DirectX::XMConvertToDegrees(valueRadians.x),
            DirectX::XMConvertToDegrees(valueRadians.y),
            DirectX::XMConvertToDegrees(valueRadians.z)
        };

        // Display as degrees
        if (ImGui::DragFloat3(name, degrees, 0.1f)) {
            // Convert back to radians
            valueRadians.x = DirectX::XMConvertToRadians(degrees[0]);
            valueRadians.y = DirectX::XMConvertToRadians(degrees[1]);
            valueRadians.z = DirectX::XMConvertToRadians(degrees[2]);
        }
    }

    void VisitEnum(const char* name, int& value, const std::vector<const char*>& options) override {
        if (ImGui::BeginCombo(name, options[value])) {
            for (int i = 0; i < (int)options.size(); ++i) {
                bool selected = (i == value);
                if (ImGui::Selectable(options[i], selected)) {
                    value = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    void VisitFilePath(const char* name, std::string& value, const char* filter) override {
        // Unity-style single-line layout with unique ID to prevent dialog conflicts
        ImGui::PushID(name);

        // Label (left side)
        ImGui::Text("%s", name);
        ImGui::SameLine();

        // File type icon
        ImGui::TextDisabled(ICON_FA_FILE);
        ImGui::SameLine();

        // Determine if this is a material field and calculate button width
        // Note: filter contains embedded \0 characters, so we can't use strstr directly
        bool isMaterialField = false;
        if (filter) {
            // Search through the entire filter string (which has embedded nulls)
            // by checking each null-terminated segment
            const char* p = filter;
            while (*p != '\0') {
                if (strstr(p, "ffasset")) {
                    isMaterialField = true;
                    break;
                }
                // Move to next segment (skip current string + null terminator)
                p += strlen(p) + 1;
                // If we hit double-null (end of filter), stop
                if (*p == '\0') break;
            }
        }
        float buttonWidth = isMaterialField ? 80.0f : 50.0f;  // More space if Edit button present

        // Path display (reserve space for buttons on right)
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushItemWidth(-buttonWidth);

        char displayBuf[260];
        snprintf(displayBuf, sizeof(displayBuf), "%s", value.empty() ? "(None)" : value.c_str());
        ImGui::InputText("##path", displayBuf, sizeof(displayBuf), ImGuiInputTextFlags_ReadOnly);

        ImGui::PopItemWidth();
        ImGui::PopStyleColor();

        // Browse button (small button with icon)
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "##browse")) {
            char filename[MAX_PATH] = "";
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);

            if (filter) {
                ofn.lpstrFilter = filter;
            } else {
                ofn.lpstrFilter = "All Files\0*.*\0";
            }

            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

            // Use FFPath for initial directory
            std::string assetsDir = FFPath::GetAssetsDir();
            ofn.lpstrInitialDir = assetsDir.c_str();

            if (GetOpenFileNameA(&ofn)) {
                // Normalize path to relative format
                value = FFPath::Normalize(filename);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Browse for file");
        }

        // Edit button (only for material fields)
        if (isMaterialField) {
            ImGui::SameLine();
            bool hasAsset = !value.empty();

            if (!hasAsset) {
                ImGui::BeginDisabled();
            }

            if (ImGui::SmallButton(ICON_FA_EDIT "##edit")) {
                Panels::OpenMaterialEditor(value);
            }

            if (!hasAsset) {
                ImGui::EndDisabled();
            }

            if (ImGui::IsItemHovered() && hasAsset) {
                ImGui::SetTooltip("Edit material");
            }
        }

        ImGui::PopID();
    }

    void VisitLabel(const char* name, const char* value) override {
        ImGui::Text("%s: %s", name, value);
    }
};

void Panels::DrawInspector(CScene& scene) {
    ImGui::Begin("Inspector");
    auto* sel = scene.GetSelectedObject();
    if (sel) {
        // GameObject name
        std::string n = sel->GetName();
        char buf[128];
        snprintf(buf, 128, "%s", n.c_str());
        if (ImGui::InputText("Name", buf, IM_ARRAYSIZE(buf))) {
            sel->SetName(buf);
        }

        ImGui::Separator();

        // Display all components using reflection
        ImGuiPropertyVisitor visitor;

        // Transform component
        if (auto* tr = sel->GetComponent<STransform>()) {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                tr->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add Transform")) {
                sel->AddComponent<STransform>();
            }
        }

        // MeshRenderer component
        if (auto* mr = sel->GetComponent<SMeshRenderer>()) {
            if (ImGui::CollapsingHeader("MeshRenderer", ImGuiTreeNodeFlags_DefaultOpen)) {
                mr->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add MeshRenderer")) {
                sel->AddComponent<SMeshRenderer>();
            }
        }
        // DirectionalLight component
        if (auto* dl = sel->GetComponent<SDirectionalLight>()) {
            if (ImGui::CollapsingHeader("DirectionalLight", ImGuiTreeNodeFlags_DefaultOpen)) {
                dl->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add DirectionalLight")) {
                sel->AddComponent<SDirectionalLight>();
            }
        }

        // PointLight component
        if (auto* pl = sel->GetComponent<SPointLight>()) {
            if (ImGui::CollapsingHeader("PointLight", ImGuiTreeNodeFlags_DefaultOpen)) {
                pl->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add PointLight")) {
                sel->AddComponent<SPointLight>();
            }
        }

        // SpotLight component
        if (auto* sl = sel->GetComponent<SSpotLight>()) {
            if (ImGui::CollapsingHeader("SpotLight", ImGuiTreeNodeFlags_DefaultOpen)) {
                sl->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add SpotLight")) {
                sel->AddComponent<SSpotLight>();
            }
        }

        // ReflectionProbe component
        if (auto* rp = sel->GetComponent<SReflectionProbe>()) {
            if (ImGui::CollapsingHeader("ReflectionProbe", ImGuiTreeNodeFlags_DefaultOpen)) {
                rp->VisitProperties(visitor);

                ImGui::Separator();

                // Bake Now button
                if (ImGui::Button("Bake Now", ImVec2(-1, 0))) {
                    // Get probe position from Transform
                    auto* tr = sel->GetComponent<STransform>();
                    if (!tr) {
                        CFFLog::Error("ReflectionProbe requires Transform component");
                    } else if (rp->assetPath.empty()) {
                        CFFLog::Error("ReflectionProbe assetPath is empty. Please set an asset path first.");
                    } else {
                        // Create baker
                        static CReflectionProbeBaker baker;
                        if (!baker.Initialize()) {
                            CFFLog::Error("Failed to initialize ReflectionProbeBaker");
                        } else {
                            // Bake the probe
                            CFFLog::Info("Baking Reflection Probe...");
                            bool success = baker.BakeProbe(
                                tr->position,
                                rp->resolution,
                                scene,
                                rp->assetPath
                            );

                            if (success) {
                                CFFLog::Info("Reflection Probe baked successfully!");
                                rp->isDirty = false;

                                // Reload probes to update TextureCubeArray with new data
                                scene.ReloadProbesFromScene();
                                CFFLog::Info("Scene probes reloaded");
                            } else {
                                CFFLog::Error("Failed to bake Reflection Probe");
                            }
                        }
                    }
                }

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Bake reflection probe cubemap and IBL maps");
                }

                // Show dirty status
                if (rp->isDirty) {
                    ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Status: Needs Rebake");
                } else {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: Up to Date");
                }
            }
        } else {
            if (ImGui::Button("Add ReflectionProbe")) {
                sel->AddComponent<SReflectionProbe>();
            }
        }

        // LightProbe component
        if (auto* lp = sel->GetComponent<SLightProbe>()) {
            if (ImGui::CollapsingHeader("LightProbe", ImGuiTreeNodeFlags_DefaultOpen)) {
                lp->VisitProperties(visitor);

                ImGui::Separator();

                // Bake Now button
                if (ImGui::Button("Bake Light Probe", ImVec2(-1, 0))) {
                    // Get probe position from Transform
                    auto* tr = sel->GetComponent<STransform>();
                    if (!tr) {
                        CFFLog::Error("LightProbe requires Transform component");
                    } else {
                        // Create baker
                        static CLightProbeBaker baker;
                        if (!baker.Initialize()) {
                            CFFLog::Error("Failed to initialize LightProbeBaker");
                        } else {
                            // Bake the probe
                            CFFLog::Info("Baking Light Probe at (%.1f, %.1f, %.1f)...",
                                        tr->position.x, tr->position.y, tr->position.z);
                            bool success = baker.BakeProbe(
                                *lp,
                                tr->position,
                                scene
                            );

                            if (success) {
                                scene.ReloadLightProbesFromScene();
                                CFFLog::Info("Light Probe baked successfully!");
                                lp->isDirty = false;
                            } else {
                                CFFLog::Error("Failed to bake Light Probe");
                            }
                        }
                    }
                }

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Bake light probe SH coefficients from scene lighting");
                }

                // Show dirty status
                if (lp->isDirty) {
                    ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Status: Needs Rebake");
                } else {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: Up to Date");
                }

                // Display SH coefficients (read-only, for debugging)
                ImGui::Separator();
                ImGui::Text("SH Coefficients (L0-L2):");
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                for (int i = 0; i < 9; i++) {
                    char label[32];
                    snprintf(label, sizeof(label), "SH[%d]", i);
                    float arr[3] = { lp->shCoeffs[i].x, lp->shCoeffs[i].y, lp->shCoeffs[i].z };
                    ImGui::InputFloat3(label, arr, "%.4f", ImGuiInputTextFlags_ReadOnly);
                }
                ImGui::PopStyleColor(2);
            }
        } else {
            if (ImGui::Button("Add LightProbe")) {
                sel->AddComponent<SLightProbe>();
            }
        }

    } else {
        ImGui::Text("No selection");
    }
    ImGui::End();
}
