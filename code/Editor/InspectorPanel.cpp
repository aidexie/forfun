#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "GameObject.h"
#include "Component.h"
#include "PropertyVisitor.h"
#include "Components/Transform.h"
#include "Components/MeshRenderer.h"
#include <string>

// ImGui implementation of PropertyVisitor for reflection-based UI
class ImGuiPropertyVisitor : public PropertyVisitor {
public:
    void VisitFloat(const char* name, float& value) override {
        ImGui::DragFloat(name, &value, 0.1f);
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
    } else {
        ImGui::Text("No selection");
    }
    ImGui::End();
}
