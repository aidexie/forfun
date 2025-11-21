#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "World.h"
#include "GameObject.h"

void Panels::DrawHierarchy(CScene& scene) {
    ImGui::Begin("Hierarchy");
    for (int i=0;i<(int)scene.GetWorld().Count();++i) {
        auto* go = scene.GetWorld().Get((std::size_t)i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
            | ((scene.GetSelected() == i) ? ImGuiTreeNodeFlags_Selected : 0);
        if (ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", go->GetName().c_str())) {
            if (ImGui::IsItemClicked()) scene.SetSelected(i);
            ImGui::TreePop();
        } else {
            if (ImGui::IsItemClicked()) scene.SetSelected(i);
        }
    }
    if (ImGui::Button("Create GameObject")) {
        scene.GetWorld().Create("GameObject");
    }
    ImGui::End();
}
