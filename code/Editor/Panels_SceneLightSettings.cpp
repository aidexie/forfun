#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/ForwardRenderPipeline.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"  // FFPath namespace
#include <windows.h>
#include <commdlg.h>

static bool s_showWindow = false;  // Default: hidden (user opens via menu)
static bool s_isBaking = false;    // Volumetric Lightmap baking state

void Panels::DrawSceneLightSettings(CForwardRenderPipeline* pipeline) {
    if (!s_showWindow) return;

    if (ImGui::Begin("Scene Light Settings", &s_showWindow)) {
        auto& settings = CScene::Instance().GetLightSettings();

        // ============================================
        // Environment Section
        // ============================================
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
                // Normalize path using FFPath
                std::string normalizedPath = FFPath::Normalize(fileName);
                settings.skyboxAssetPath = normalizedPath;

                // Apply immediately: reload environment (skybox + IBL)
                CScene::Instance().ReloadEnvironment(settings.skyboxAssetPath);
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // ============================================
        // Volumetric Lightmap Section
        // ============================================
        ImGui::Text("Volumetric Lightmap");
        ImGui::Separator();

        auto& vlConfig = settings.volumetricLightmap;
        auto& volumetricLightmap = CScene::Instance().GetVolumetricLightmap();

        // Enable checkbox
        if (ImGui::Checkbox("Enable##VL", &vlConfig.enabled)) {
            volumetricLightmap.SetEnabled(vlConfig.enabled);
        }

        ImGui::Spacing();

        // Volume bounds
        ImGui::Text("Volume Bounds:");
        ImGui::PushItemWidth(200);
        ImGui::DragFloat3("Min##VLMin", &vlConfig.volumeMin.x, 1.0f, -1000.0f, 1000.0f, "%.1f");
        ImGui::DragFloat3("Max##VLMax", &vlConfig.volumeMax.x, 1.0f, -1000.0f, 1000.0f, "%.1f");
        ImGui::PopItemWidth();

        // Min brick size
        ImGui::PushItemWidth(150);
        ImGui::DragFloat("Min Brick Size (m)##VL", &vlConfig.minBrickWorldSize, 0.1f, 0.5f, 20.0f, "%.1f");
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Minimum size of the finest bricks.\nSmaller = more precision, more memory.\nRecommended: 1.0 - 4.0 meters.");
        }

        ImGui::Spacing();

        // Show derived params if initialized
        if (volumetricLightmap.IsInitialized()) {
            const auto& derived = volumetricLightmap.GetDerivedParams();
            ImGui::TextDisabled("Derived: MaxLevel=%d, IndirectionRes=%d^3",
                               derived.maxLevel, derived.indirectionResolution);
            if (volumetricLightmap.HasBakedData()) {
                ImGui::TextDisabled("Bricks: %d, AtlasSize: %d^3",
                                   derived.actualBrickCount, derived.brickAtlasSize);
            }
        }

        ImGui::Spacing();

        // Bake buttons
        if (s_isBaking) {
            ImGui::BeginDisabled();
            ImGui::Button("Baking...", ImVec2(200, 30));
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Build & Bake Volumetric Lightmap", ImVec2(250, 30))) {
                s_isBaking = true;

                // Initialize with current config
                CVolumetricLightmap::Config config;
                config.volumeMin = vlConfig.volumeMin;
                config.volumeMax = vlConfig.volumeMax;
                config.minBrickWorldSize = vlConfig.minBrickWorldSize;

                auto& vl = CScene::Instance().GetVolumetricLightmap();

                // Re-initialize if config changed
                vl.Shutdown();
                if (vl.Initialize(config)) {
                    // Build octree
                    vl.BuildOctree(CScene::Instance());

                    // Bake all bricks
                    vl.BakeAllBricks(CScene::Instance());

                    // Create GPU resources
                    if (vl.CreateGPUResources()) {
                        vl.SetEnabled(true);
                        vlConfig.enabled = true;
                        CFFLog::Info("[VolumetricLightmap] Bake complete and GPU resources created!");
                    } else {
                        CFFLog::Error("[VolumetricLightmap] Failed to create GPU resources!");
                    }
                } else {
                    CFFLog::Error("[VolumetricLightmap] Failed to initialize!");
                }

                s_isBaking = false;
            }
        }

        // Clear button
        ImGui::SameLine();
        if (ImGui::Button("Clear##VL")) {
            CScene::Instance().GetVolumetricLightmap().Shutdown();
            vlConfig.enabled = false;
            CFFLog::Info("[VolumetricLightmap] Cleared.");
        }

        ImGui::Spacing();

        // Debug visualization checkbox
        if (volumetricLightmap.HasBakedData()) {
            bool debugDraw = volumetricLightmap.IsDebugDrawEnabled();
            if (ImGui::Checkbox("Show Octree Debug##VL", &debugDraw)) {
                volumetricLightmap.SetDebugDrawEnabled(debugDraw);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Visualize the octree brick structure.\nColors indicate subdivision levels:\nRed=0, Orange=1, Yellow=2, Green=3, etc.");
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // ============================================
        // Clustered Lighting Debug Section
        // ============================================
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
