#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/Skybox.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIResources.h"
#include "Core/FFLog.h"

// Window visibility state
static bool s_showWindow = false;

// Cache for last environment texture (to detect texture changes)
static RHI::ITexture* g_lastEnvTexture = nullptr;

void Panels::DrawIrradianceDebug() {
    if (!s_showWindow) return;

    if (ImGui::Begin("IBL Debug", &s_showWindow)) {

    ImGui::Text("Image-Based Lighting Debug Visualization");
    ImGui::Separator();

    // Get Scene singleton references
    CSkybox& skybox = CScene::Instance().GetSkybox();

    // Display size for each face (shared between tabs)
    static float displaySize = 128.0f;
    ImGui::SliderFloat("Display Size", &displaySize, 64.0f, 256.0f);
    ImGui::Separator();

    // Use tabs to separate different IBL resources
    if (ImGui::BeginTabBar("IBLTabs", ImGuiTabBarFlags_None)) {

        // === Tab 1: Environment Map ===
        if (ImGui::BeginTabItem("Environment Map")) {
            ImGui::Text("Source environment cubemap (skybox display)");
            ImGui::Separator();

            // Get environment map from Scene's skybox
            RHI::ITexture* envTexture = skybox.GetEnvironmentTexture();
            if (!envTexture || !envTexture->GetSRV()) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "No environment map loaded!");
                ImGui::Text("Load a skybox first.");
            }
            else {
                // Get texture info from RHI
                uint32_t texWidth = envTexture->GetWidth();
                uint32_t texHeight = envTexture->GetHeight();
                uint32_t mipLevels = envTexture->GetMipLevels();

                int maxMipLevel = (int)mipLevels - 1;

                // Display texture info
                ImGui::Text("Resolution: %u x %u", texWidth, texHeight);
                ImGui::Text("Format: R16G16B16A16_FLOAT (HDR)");
                ImGui::Text("Mip Levels: %u", mipLevels);
                ImGui::Separator();

                // Mip level selection
                static int selectedEnvMip = 0;
                if (selectedEnvMip > maxMipLevel) selectedEnvMip = maxMipLevel;

                ImGui::SliderInt("Mip Level", &selectedEnvMip, 0, maxMipLevel);

                int mipSize = texWidth >> selectedEnvMip;  // size / 2^mip
                ImGui::Text("Mip %d: %d x %d", selectedEnvMip, mipSize, mipSize);

                if (selectedEnvMip == 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "(Full Resolution)");
                }
                else {
                    float roughnessApprox = (float)selectedEnvMip / (float)maxMipLevel;
                    ImGui::SameLine();
                    ImGui::Text("(~roughness %.2f)", roughnessApprox);
                }
                ImGui::Separator();

                // Helper lambda to get face SRV
                auto getFaceSRV = [&](int faceIndex) -> void* {
                    return envTexture->GetSRVSlice(faceIndex, selectedEnvMip);
                };

                // Row 1: +Y (Top)
                ImGui::Dummy(ImVec2(displaySize, 0));
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::Text("+Y");
                void* envTopSRV = getFaceSRV(2);
                if (envTopSRV) {
                    ImGui::Image(envTopSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Error");
                }
                ImGui::EndGroup();

                // Row 2: -X, +Z, +X, -Z
                ImGui::BeginGroup();

                // -X (Left)
                ImGui::BeginGroup();
                ImGui::Text("-X");
                void* envLeftSRV = getFaceSRV(1);
                if (envLeftSRV) {
                    ImGui::Image(envLeftSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Error");
                }
                ImGui::EndGroup();
                ImGui::SameLine();

                // +Z (Front)
                ImGui::BeginGroup();
                ImGui::Text("+Z");
                void* envFrontSRV = getFaceSRV(4);
                if (envFrontSRV) {
                    ImGui::Image(envFrontSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Error");
                }
                ImGui::EndGroup();
                ImGui::SameLine();

                // +X (Right)
                ImGui::BeginGroup();
                ImGui::Text("+X");
                void* envRightSRV = getFaceSRV(0);
                if (envRightSRV) {
                    ImGui::Image(envRightSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Error");
                }
                ImGui::EndGroup();
                ImGui::SameLine();

                // -Z (Back)
                ImGui::BeginGroup();
                ImGui::Text("-Z");
                void* envBackSRV = getFaceSRV(5);
                if (envBackSRV) {
                    ImGui::Image(envBackSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Error");
                }
                ImGui::EndGroup();

                ImGui::EndGroup();

                // Row 3: -Y (Bottom)
                ImGui::Dummy(ImVec2(displaySize, 0));
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::Text("-Y");
                void* envBottomSRV = getFaceSRV(3);
                if (envBottomSRV) {
                    ImGui::Image(envBottomSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Error");
                }
                ImGui::EndGroup();
            }

            ImGui::EndTabItem();
        }

        // === Tab 2: BRDF LUT ===
        if (ImGui::BeginTabItem("BRDF LUT")) {
            ImGui::Text("BRDF lookup table for Split Sum Approximation");
            ImGui::Text("512x512 2D texture, loaded from KTX2");
            ImGui::Separator();

            // Show/Hide visualization control
            static bool showBrdfLutViz = false;
            if (!showBrdfLutViz) {
                if (ImGui::Button("Show Visualization", ImVec2(200, 30))) {
                    showBrdfLutViz = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Display BRDF LUT texture for debugging");
                }
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Click 'Show Visualization' to display BRDF LUT");
            } else {
                if (ImGui::Button("Hide Visualization", ImVec2(200, 30))) {
                    showBrdfLutViz = false;
                }
                ImGui::Separator();

                RHI::ITexture* brdfLutTex = CScene::Instance().GetProbeManager().GetBrdfLutTexture();
                if (brdfLutTex && brdfLutTex->GetSRV()) {
                    ImGui::Text("Resolution: 512 x 512");
                    ImGui::Text("Format: R16G16_FLOAT (RG channels)");
                    ImGui::Text("R channel: Scale (multiply with F0)");
                    ImGui::Text("G channel: Bias (add after multiplication)");
                    ImGui::Separator();

                    // Display larger preview
                    static float lutDisplaySize = 512.0f;
                    ImGui::SliderFloat("LUT Display Size", &lutDisplaySize, 256.0f, 512.0f);
                    ImGui::Separator();

                    ImGui::Text("X-axis: cos(NdotV) [0=grazing, 1=perpendicular]");
                    ImGui::Text("Y-axis: Roughness [0=mirror, 1=rough]");
                    ImGui::Separator();

                    ImGui::Image(brdfLutTex->GetSRV(), ImVec2(lutDisplaySize, lutDisplaySize));

                    ImGui::Separator();
                    ImGui::TextWrapped("Expected appearance: Bright in top-left (smooth + perpendicular), "
                        "dark in bottom-right (rough + grazing). "
                        "Red and green channels should have similar but slightly different gradients.");
                } else {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "BRDF LUT not loaded!");
                }
            }

            ImGui::EndTabItem();
        }

        // === Tab 3: Probe Debug (TODO) ===
        if (ImGui::BeginTabItem("Probe Debug")) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "TODO: Probe Debug Visualization");
            ImGui::Separator();
            ImGui::Text("Future features:");
            ImGui::BulletText("View irradiance/prefiltered maps per probe");
            ImGui::BulletText("Select probe by index (0=global, 1-7=local)");
            ImGui::BulletText("Show probe positions in viewport");
            ImGui::BulletText("Per-face visualization with mip selection");
            ImGui::Separator();
            ImGui::Text("Probe Count: %d", CScene::Instance().GetProbeManager().GetProbeCount());
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextWrapped("Note: This displays HDR values tonemapped by ImGui. "
        "For accurate inspection, check individual pixel values with a color picker tool.");

    }
    ImGui::End();
}

void Panels::ShowIrradianceDebug(bool show) {
    s_showWindow = show;
}

bool Panels::IsIrradianceDebugVisible() {
    return s_showWindow;
}
