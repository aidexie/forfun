#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "World.h"
#include "GameObject.h"

void Panels::DrawHierarchy(Scene& scene) {
    ImGui::Begin("Hierarchy");
    for (int i=0;i<(int)scene.world.Count();++i) {
        auto* go = scene.world.Get((std::size_t)i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
            | ((scene.selected==i)? ImGuiTreeNodeFlags_Selected : 0);
        if (ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", go->GetName().c_str())) {
            if (ImGui::IsItemClicked()) scene.selected = i;
            ImGui::TreePop();
        } else {
            if (ImGui::IsItemClicked()) scene.selected = i;
        }
    }
    if (ImGui::Button("Create GameObject")) {
        scene.world.Create("GameObject");
    }
    ImGui::End();
}
