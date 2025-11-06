#include "Panels.h"
#include "imgui.h"
#include "Scene.h"

void Panels::DrawHierarchy(Scene& scene) {
    ImGui::Begin("Hierarchy");
    for (int i=0;i<(int)scene.entities.size();++i) {
        auto& e = scene.entities[i];
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
            | ((scene.selected==i)? ImGuiTreeNodeFlags_Selected : 0);
        if (ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s (%d)", e.name.c_str(), e.id)) {
            if (ImGui::IsItemClicked()) scene.selected = i;
            ImGui::TreePop();
        } else {
            if (ImGui::IsItemClicked()) scene.selected = i;
        }
    }
    if (ImGui::Button("Create Entity")) {
        int id = (int)scene.entities.size();
        scene.entities.push_back({id, "Entity"});
    }
    ImGui::End();
}
