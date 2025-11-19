#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "GameObject.h"
#include "Component.h"
#include "PropertyVisitor.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include "Components/DirectionalLight.h"
#include "Components/Material.h"
#include <string>
#include <windows.h>
#include <commdlg.h>

// ImGui implementation of PropertyVisitor for reflection-based UI
class ImGuiPropertyVisitor : public PropertyVisitor {
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
        // Display current value as read-only text with frame
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));

        // Negative width = reserve space from right edge (for Browse button)
        ImGui::PushItemWidth(-80.0f);

        char displayBuf[260];
        snprintf(displayBuf, sizeof(displayBuf), "%s", value.empty() ? "(None)" : value.c_str());
        ImGui::InputText(name, displayBuf, sizeof(displayBuf), ImGuiInputTextFlags_ReadOnly);

        ImGui::PopItemWidth();
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(70, 0))) {
            char filename[MAX_PATH] = "";
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);

            // Build filter string
            if (filter) {
                ofn.lpstrFilter = filter;
            } else {
                ofn.lpstrFilter = "All Files\0*.*\0";
            }

            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrInitialDir = "E:\\forfun\\assets";

            if (GetOpenFileNameA(&ofn)) {
                // Convert to relative path (relative to assets folder)
                std::string fullPath = filename;
                std::string assetsPath = "E:\\forfun\\assets\\";
                if (fullPath.find(assetsPath) == 0) {
                    value = fullPath.substr(assetsPath.length());
                } else {
                    value = fullPath;
                }
            }
        }
    }

    void VisitLabel(const char* name, const char* value) override {
        ImGui::Text("%s: %s", name, value);
    }
};

void Panels::DrawInspector(Scene& scene) {
    ImGui::Begin("Inspector");
    auto* sel = scene.GetSelected();
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
        if (auto* tr = sel->GetComponent<Transform>()) {
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                tr->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add Transform")) {
                sel->AddComponent<Transform>();
            }
        }

        // MeshRenderer component
        if (auto* mr = sel->GetComponent<MeshRenderer>()) {
            if (ImGui::CollapsingHeader("MeshRenderer", ImGuiTreeNodeFlags_DefaultOpen)) {
                mr->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add MeshRenderer")) {
                sel->AddComponent<MeshRenderer>();
            }
        }
        // DirectionalLight component
        if (auto* dl = sel->GetComponent<DirectionalLight>()) {
            if (ImGui::CollapsingHeader("DirectionalLight", ImGuiTreeNodeFlags_DefaultOpen)) {
                dl->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add DirectionalLight")) {
                sel->AddComponent<DirectionalLight>();
            }
        }

        // Material component
        if (auto* mat = sel->GetComponent<Material>()) {
            if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                mat->VisitProperties(visitor);
            }
        } else {
            if (ImGui::Button("Add Material")) {
                sel->AddComponent<Material>();
            }
        }
    } else {
        ImGui::Text("No selection");
    }
    ImGui::End();
}
