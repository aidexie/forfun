#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/ForwardRenderPipeline.h"
#include "Core/FFLog.h"
#include <windows.h>
#include <commdlg.h>

static bool s_showWindow = false;  // Default: hidden (user opens via menu)

void Panels::DrawSceneLightSettings(CForwardRenderPipeline* pipeline) {
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

                // Apply immediately: reload environment (skybox + IBL)
                CScene::Instance().ReloadEnvironment(settings.skyboxAssetPath);
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Clustered Lighting Debug
        if (pipeline) {
            ImGui::Text("Clustered Lighting Debug");
            ImGui::Separator();

            static int debugModeIndex = 0;
            const char* debugModes[] = { "None", "Light Count Heatmap", "Cluster AABB" };

            if (ImGui::Combo("Debug Mode", &debugModeIndex, debugModes, IM_ARRAYSIZE(debugModes))) {
                auto& clusteredPass = pipeline->GetSceneRenderer().GetClusteredLightingPass();
                switch (debugModeIndex) {
                    case 0: clusteredPass.SetDebugMode(CClusteredLightingPass::EDebugMode::None); break;
                    case 1: clusteredPass.SetDebugMode(CClusteredLightingPass::EDebugMode::LightCountHeatmap); break;
                    case 2: clusteredPass.SetDebugMode(CClusteredLightingPass::EDebugMode::ClusterAABB); break;
                }
                CFFLog::Info("Clustered lighting debug mode: %s", debugModes[debugModeIndex]);
            }

            ImGui::Spacing();
        }

        ImGui::Spacing();

        // Apply button (manual apply if needed)
        if (ImGui::Button("Apply Settings")) {
            if (!settings.skyboxAssetPath.empty()) {
                CScene::Instance().ReloadEnvironment(settings.skyboxAssetPath);
            }
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
