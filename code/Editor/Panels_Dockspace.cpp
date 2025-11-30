#include "Panels.h"
#include "imgui.h"
#include "Scene.h"
#include "SceneSerializer.h"
#include "Engine/Rendering/ForwardRenderPipeline.h"
#include "Engine/Rendering/IBLGenerator.h"
#include "Core/FFLog.h"
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

void Panels::DrawDockspace(bool* pOpen, CScene& scene, CForwardRenderPipeline* pipeline) {
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

    // Handle global keyboard shortcuts
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
        // Ctrl+S: Save Scene (no dialog, direct save)
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (scene.HasFilePath()) {
                CSceneSerializer::SaveScene(scene, scene.GetFilePath());
                CFFLog::Info("Scene saved to: %s", scene.GetFilePath().c_str());
            } else {
                CFFLog::Error("Cannot save: No file path set. Use 'Save Scene As...' first.");
            }
        }
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Save Scene: Direct save to current path (no dialog)
            // Disabled if no file path set
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", nullptr, scene.HasFilePath())) {
                CSceneSerializer::SaveScene(scene, scene.GetFilePath());
                CFFLog::Info("Scene saved to: %s", scene.GetFilePath().c_str());
            }

            // Save Scene As: Always show dialog
            if (ImGui::MenuItem("Save Scene As...")) {
                std::string path = SaveFileDialog("Scene Files (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    scene.SetFilePath(path);
                    CSceneSerializer::SaveScene(scene, path);
                    CFFLog::Info("Scene saved to: %s", path.c_str());
                }
            }

            if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
                std::string path = OpenFileDialog("Scene Files (*.scene)\0*.scene\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    CSceneSerializer::LoadScene(scene, path);
                    scene.SetFilePath(path);
                    CFFLog::Info("Scene loaded from: %s", path.c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                *pOpen = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("HDR Export")) {
                Panels::ShowHDRExportWindow(true);
            }
            if (ImGui::MenuItem("Scene Light Settings")) {
                Panels::ShowSceneLightSettings(true);
            }
            if (ImGui::MenuItem("IBL Debug")) {
                Panels::ShowIrradianceDebug(true);
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
    ImGui::End();
}
