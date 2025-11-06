#include "Panels.h"
#include "imgui.h"
#include "Scene.h"

void Panels::DrawInspector(Scene& scene) {
    ImGui::Begin("Inspector");
    if (scene.selected>=0 && scene.selected<(int)scene.entities.size()) {
        auto& e = scene.entities[scene.selected];
        char buf[128]; snprintf(buf,128,"%s", e.name.c_str());
        if (ImGui::InputText("Name", buf, IM_ARRAYSIZE(buf))) e.name = buf;
        ImGui::Text("ID: %d", e.id);
    } else {
        ImGui::Text("No selection");
    }
    ImGui::End();
}
