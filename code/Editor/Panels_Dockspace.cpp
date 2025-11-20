#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "SceneSerializer.h"
#include "Engine/Rendering/MainPass.h"
#include "Engine/Rendering/IBLGenerator.h"
#include <windows.h> // For file dialogs
#include <commdlg.h>
#include <string>
#include <iostream>

// Helper: Open file dialog
static std::string OpenFileDialog(const char* filter) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        return std::string(filename);
    }
    return "";
}

// Helper: Save file dialog
static std::string SaveFileDialog(const char* filter) {
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "scene";

    if (GetSaveFileNameA(&ofn)) {
        return std::string(filename);
    }
    return "";
}

void Panels::DrawDockspace(bool* pOpen, Scene& scene, MainPass* mainPass) {
    ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_MenuBar;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("DockSpace", pOpen, winFlags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpace(dockspace_id, ImVec2(0,0), dockFlags);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                std::string path = SaveFileDialog("Scene Files (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    SceneSerializer::SaveScene(scene, path);
                }
            }
            if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
                std::string path = OpenFileDialog("Scene Files (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    SceneSerializer::LoadScene(scene, path);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                *pOpen = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::End();
}
