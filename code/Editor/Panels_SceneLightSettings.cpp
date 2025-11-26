#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Core/FFLog.h"
#include <windows.h>
#include <commdlg.h>

static bool s_showWindow = false;  // Default: hidden (user opens via menu)

void Panels::DrawSceneLightSettings() {
    if (!s_showWindow) return;

    if (ImGui::Begin("Scene Light Settings", &s_showWindow)) {
        auto& settings = CScene::Instance().GetLightSettings();

        ImGui::Text("Environment");
        ImGui::Separator();

        // Skybox Asset Path
        ImGui::Text("Skybox Asset:");
        ImGui::PushItemWidth(-100);

        char buffer[512];
        strncpy_s(buffer, settings.skyboxAssetPath.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';

        if (ImGui::InputText("##SkyboxPath", buffer, sizeof(buffer))) {
            settings.skyboxAssetPath = buffer;
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();

        if (ImGui::Button("Browse...##Skybox")) {
            // Open file dialog
            OPENFILENAMEA ofn{};
            char fileName[MAX_PATH] = "";

            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFilter = "FFAsset Files\0*.ffasset\0All Files\0*.*\0";
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = "Select Skybox Asset";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                // Convert absolute path to relative path if possible
                std::string selectedPath = fileName;

                // Simple relative path conversion (assumes asset is in E:/forfun/assets)
                size_t pos = selectedPath.find("assets");
                if (pos != std::string::npos) {
                    selectedPath = selectedPath.substr(pos + 7); // Skip "assets/"
                }

                settings.skyboxAssetPath = selectedPath;

                // Apply immediately: reload skybox
                CFFLog::Info("Applying new skybox: %s", settings.skyboxAssetPath.c_str());
                CScene::Instance().Initialize(settings.skyboxAssetPath);
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Apply button (manual apply if needed)
        if (ImGui::Button("Apply Settings")) {
            CFFLog::Info("Applying scene light settings");
            CScene::Instance().Initialize(settings.skyboxAssetPath);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(Settings auto-apply on change)");
    }
    ImGui::End();
}

void Panels::ShowSceneLightSettings(bool show) {
    s_showWindow = show;
}

bool Panels::IsSceneLightSettingsVisible() {
    return s_showWindow;
}
