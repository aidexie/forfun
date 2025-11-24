#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/IBLGenerator.h"
#include "Engine/Rendering/Skybox.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
#include <iostream>
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

// Window visibility state
static bool s_showWindow = false;

// Cache for environment face SRVs (key = mipLevel * 6 + faceIndex)
static std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>> g_envFaceSRVCache;
static ID3D11Texture2D* g_lastEnvTexture = nullptr;

// Helper: Create SRV for specific environment face and mip level
static ID3D11ShaderResourceView* GetEnvironmentFaceSRV(
    ID3D11ShaderResourceView* envCubemap,
    int faceIndex,
    int mipLevel)
{
    if (!envCubemap || faceIndex < 0 || faceIndex >= 6 || mipLevel < 0) {
        return nullptr;
    }

    // Get the underlying texture
    ID3D11Resource* resource;
    envCubemap->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    resource->QueryInterface(texture.GetAddressOf());
    resource->Release();

    if (!texture) return nullptr;

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Validate mip level
    if (mipLevel >= (int)desc.MipLevels) {
        return nullptr;
    }

    // Clear cache if texture changed
    if (g_lastEnvTexture != texture.Get()) {
        g_envFaceSRVCache.clear();
        g_lastEnvTexture = texture.Get();
    }

    // Check cache
    int key = mipLevel * 6 + faceIndex;
    auto it = g_envFaceSRVCache.find(key);
    if (it != g_envFaceSRVCache.end()) {
        return it->second.Get();
    }

    // Create new SRV for this face and mip level
    ID3D11Device* device = CDX11Context::Instance().GetDevice();
    if (!device) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = mipLevel;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = faceIndex;
    srvDesc.Texture2DArray.ArraySize = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) {
        CFFLog::Error("ERROR: Failed to create environment face SRV (face=%d, mip=%d)", faceIndex, mipLevel);
        return nullptr;
    }

    g_envFaceSRVCache[key] = srv;
    return srv.Get();
}

void Panels::DrawIrradianceDebug() {
    if (!s_showWindow) return;

    if (ImGui::Begin("IBL Debug", &s_showWindow)) {

    ImGui::Text("Image-Based Lighting Debug Visualization");
    ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "IBL resources auto-generated for rendering on startup");
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "Debug visualization creates SRVs on-demand (click Show buttons)");
    ImGui::Separator();

    // Get Scene singleton references
    CIBLGenerator& iblGen = CScene::Instance().GetIBLGenerator();
    CSkybox& skybox = CScene::Instance().GetSkybox();

    ImGui::Separator();

    // Display size for each face (shared between tabs)
    static float displaySize = 128.0f;
    ImGui::SliderFloat("Display Size", &displaySize, 64.0f, 256.0f);
    ImGui::Separator();

    // Use tabs to separate irradiance and pre-filtered maps
    if (ImGui::BeginTabBar("IBLTabs", ImGuiTabBarFlags_None)) {

        // === Tab 1: Irradiance Map ===
        if (ImGui::BeginTabItem("Irradiance Map")) {
            ImGui::Text("Diffuse irradiance cubemap (Lambert cosine-weighted)");
            ImGui::Text("32x32 cubemap, auto-generated on startup");
            ImGui::Separator();

            // Show/Hide visualization control
            static bool showIrradianceViz = false;
            if (!showIrradianceViz) {
                if (ImGui::Button("Show Visualization", ImVec2(200, 30))) {
                    showIrradianceViz = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Display cubemap faces for debugging\n"
                                     "Creates individual face SRVs on-demand");
                }
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Click 'Show Visualization' to display cubemap faces");
            } else {
                if (ImGui::Button("Hide Visualization", ImVec2(200, 30))) {
                    showIrradianceViz = false;
                }
                ImGui::Separator();

                // Cross layout for cubemap (standard unwrap):
                //        [+Y]
                // [-X] [-Z] [+X] [+Z]
                //        [-Y]

                // Row 1: +Y (Top)
                ImGui::Dummy(ImVec2(displaySize, 0));  // Space before
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::Text("+Y (Top)");
                ID3D11ShaderResourceView* topSRV = iblGen.GetIrradianceFaceSRV(2);
                if (topSRV) {
                    ImGui::Image((void*)topSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Not Generated");
                }
                ImGui::EndGroup();

                // Row 2: -X, -Z, +X, +Z (horizontal strip)
                ImGui::BeginGroup();

                // -X (Left)
                ImGui::BeginGroup();
                ImGui::Text("-X (Left)");
                ID3D11ShaderResourceView* leftSRV = iblGen.GetIrradianceFaceSRV(1);
                if (leftSRV) {
                    ImGui::Image((void*)leftSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Not Generated");
                }
                ImGui::EndGroup();
                ImGui::SameLine();

                // -Z (Back)
                ImGui::BeginGroup();
                ImGui::Text("+Z (Back)");
                ID3D11ShaderResourceView* backSRV = iblGen.GetIrradianceFaceSRV(4);
                if (backSRV) {
                    ImGui::Image((void*)backSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Not Generated");
                }
                ImGui::EndGroup();
                ImGui::SameLine();

                // +X (Right)
                ImGui::BeginGroup();
                ImGui::Text("+X (Right)");
                ID3D11ShaderResourceView* rightSRV = iblGen.GetIrradianceFaceSRV(0);
                if (rightSRV) {
                    ImGui::Image((void*)rightSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Not Generated");
                }
                ImGui::EndGroup();
                ImGui::SameLine();

                // +Z (Front)
                ImGui::BeginGroup();
                ImGui::Text("-Z (Front)");
                ID3D11ShaderResourceView* frontSRV = iblGen.GetIrradianceFaceSRV(5);
                if (frontSRV) {
                    ImGui::Image((void*)frontSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Not Generated");
                }
                ImGui::EndGroup();

                ImGui::EndGroup();

                // Row 3: -Y (Bottom)
                ImGui::Dummy(ImVec2(displaySize, 0));  // Space before
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::Text("-Y (Bottom)");
                ID3D11ShaderResourceView* bottomSRV = iblGen.GetIrradianceFaceSRV(3);
                if (bottomSRV) {
                    ImGui::Image((void*)bottomSRV, ImVec2(displaySize, displaySize));
                }
                else {
                    ImGui::Text("Not Generated");
                }
                ImGui::EndGroup();
            }  // End of showIrradianceViz

            ImGui::Separator();

            // Export to HDR button
            ImGui::Text("Export:");
            if (ImGui::Button("Export to HDR Files", ImVec2(200, 30))) {
                // Export to assets directory
                bool success = iblGen.SaveIrradianceMapToHDR("export/irradiance.hdr");
                if (success) {
                    CFFLog::Info("IBL: Irradiance map exported to export/irradiance_*.hdr");
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Export irradiance cubemap to 6 HDR files:\n"
                                 "  irradiance_posX.hdr, irradiance_negX.hdr,\n"
                                 "  irradiance_posY.hdr, irradiance_negY.hdr,\n"
                                 "  irradiance_posZ.hdr, irradiance_negZ.hdr\n\n"
                                 "Files saved to: E:\\forfun\\assets\\export\\");
            }

            ImGui::EndTabItem();
        }

        // === Tab 2: Pre-Filtered Map ===
        if (ImGui::BeginTabItem("Pre-Filtered Map")) {
            ImGui::Text("Specular pre-filtered environment map (GGX importance sampling)");
            ImGui::Text("128x128 cubemap with 7 mip levels, auto-generated on startup");
            ImGui::Separator();

            // Show/Hide visualization control
            static bool showPreFilteredViz = false;
            if (!showPreFilteredViz) {
                if (ImGui::Button("Show Visualization", ImVec2(200, 30))) {
                    showPreFilteredViz = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Display cubemap faces for debugging\n"
                                     "Creates individual face SRVs on-demand");
                }
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Click 'Show Visualization' to display cubemap faces");
            } else {
                if (ImGui::Button("Hide Visualization", ImVec2(200, 30))) {
                    showPreFilteredViz = false;
                }
                ImGui::Separator();

                // Get the number of mip levels from IBL generator
                int prefilterMipLevels = iblGen.GetPreFilteredMipLevels();

                if (prefilterMipLevels > 0) {
                    static int selectedMipLevel = 0;
                    // Clamp to valid range
                    if (selectedMipLevel >= prefilterMipLevels) selectedMipLevel = prefilterMipLevels - 1;

                    ImGui::SliderInt("Mip Level", &selectedMipLevel, 0, prefilterMipLevels - 1);
                    ImGui::Text("Roughness: %.2f", (float)selectedMipLevel / float(prefilterMipLevels - 1));
                    ImGui::Separator();

                    // Row 1: +Y (Top)
                    ImGui::Dummy(ImVec2(displaySize, 0));
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("+Y (Top)");
                    ID3D11ShaderResourceView* prefTopSRV = iblGen.GetPreFilteredFaceSRV(2, selectedMipLevel);
                    if (prefTopSRV) {
                        ImGui::Image((void*)prefTopSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Not Generated");
                    }
                    ImGui::EndGroup();

                    // Row 2: -X, -Z, +X, +Z
                    ImGui::BeginGroup();

                    ImGui::BeginGroup();
                    ImGui::Text("-X (Left)");
                    ID3D11ShaderResourceView* prefLeftSRV = iblGen.GetPreFilteredFaceSRV(1, selectedMipLevel);
                    if (prefLeftSRV) {
                        ImGui::Image((void*)prefLeftSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Not Generated");
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    ImGui::BeginGroup();
                    ImGui::Text("+Z (Back)");
                    ID3D11ShaderResourceView* prefBackSRV = iblGen.GetPreFilteredFaceSRV(4, selectedMipLevel);
                    if (prefBackSRV) {
                        ImGui::Image((void*)prefBackSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Not Generated");
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    ImGui::BeginGroup();
                    ImGui::Text("+X (Right)");
                    ID3D11ShaderResourceView* prefRightSRV = iblGen.GetPreFilteredFaceSRV(0, selectedMipLevel);
                    if (prefRightSRV) {
                        ImGui::Image((void*)prefRightSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Not Generated");
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    ImGui::BeginGroup();
                    ImGui::Text("-Z (Front)");
                    ID3D11ShaderResourceView* prefFrontSRV = iblGen.GetPreFilteredFaceSRV(5, selectedMipLevel);
                    if (prefFrontSRV) {
                        ImGui::Image((void*)prefFrontSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Not Generated");
                    }
                    ImGui::EndGroup();

                    ImGui::EndGroup();

                    // Row 3: -Y (Bottom)
                    ImGui::Dummy(ImVec2(displaySize, 0));
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("-Y (Bottom)");
                    ID3D11ShaderResourceView* prefBottomSRV = iblGen.GetPreFilteredFaceSRV(3, selectedMipLevel);
                    if (prefBottomSRV) {
                        ImGui::Image((void*)prefBottomSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Not Generated");
                    }
                    ImGui::EndGroup();
                }  // End of prefilterMipLevels > 0
            }  // End of showPreFilteredViz

            ImGui::EndTabItem();
        }

        // === Tab 3: Environment Map ===
        if (ImGui::BeginTabItem("Environment Map")) {
                ImGui::Text("Source environment cubemap (used by IBL generation)");
                ImGui::Separator();

                // Get environment map from Scene's skybox
                ID3D11ShaderResourceView* envMap = skybox.GetEnvironmentMap();
                if (!envMap) {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "No environment map loaded!");
                    ImGui::Text("Load a skybox first.");
                    ImGui::EndTabItem();
                }
                else {
                    // Get texture info
                    ID3D11Resource* resource;
                    envMap->GetResource(&resource);
                    ComPtr<ID3D11Texture2D> texture;
                    resource->QueryInterface(texture.GetAddressOf());
                    resource->Release();

                    D3D11_TEXTURE2D_DESC desc;
                    texture->GetDesc(&desc);

                    int maxMipLevel = (int)desc.MipLevels - 1;

                    // Display texture info
                    ImGui::Text("Resolution: %u x %u", desc.Width, desc.Height);
                    ImGui::Text("Format: R16G16B16A16_FLOAT (HDR)");
                    ImGui::Text("Mip Levels: %u", desc.MipLevels);
                    ImGui::Separator();

                    // Mip level selection
                    static int selectedEnvMip = 0;
                    if (selectedEnvMip > maxMipLevel) selectedEnvMip = maxMipLevel;

                    ImGui::SliderInt("Mip Level", &selectedEnvMip, 0, maxMipLevel);

                    int mipSize = desc.Width >> selectedEnvMip;  // size / 2^mip
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

                    // Row 1: +Y (Top)
                    ImGui::Dummy(ImVec2(displaySize, 0));
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("+Y");
                    ID3D11ShaderResourceView* envTopSRV = GetEnvironmentFaceSRV(envMap, 2, selectedEnvMip);
                    if (envTopSRV) {
                        ImGui::Image((void*)envTopSRV, ImVec2(displaySize, displaySize));
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
                    ID3D11ShaderResourceView* envLeftSRV = GetEnvironmentFaceSRV(envMap, 1, selectedEnvMip);
                    if (envLeftSRV) {
                        ImGui::Image((void*)envLeftSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Error");
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    // +Z (Front)
                    ImGui::BeginGroup();
                    ImGui::Text("+Z");
                    ID3D11ShaderResourceView* envFrontSRV = GetEnvironmentFaceSRV(envMap, 4, selectedEnvMip);
                    if (envFrontSRV) {
                        ImGui::Image((void*)envFrontSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Error");
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    // +X (Right)
                    ImGui::BeginGroup();
                    ImGui::Text("+X");
                    ID3D11ShaderResourceView* envRightSRV = GetEnvironmentFaceSRV(envMap, 0, selectedEnvMip);
                    if (envRightSRV) {
                        ImGui::Image((void*)envRightSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Error");
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();

                    // -Z (Back)
                    ImGui::BeginGroup();
                    ImGui::Text("-Z");
                    ID3D11ShaderResourceView* envBackSRV = GetEnvironmentFaceSRV(envMap, 5, selectedEnvMip);
                    if (envBackSRV) {
                        ImGui::Image((void*)envBackSRV, ImVec2(displaySize, displaySize));
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
                    ID3D11ShaderResourceView* envBottomSRV = GetEnvironmentFaceSRV(envMap, 3, selectedEnvMip);
                    if (envBottomSRV) {
                        ImGui::Image((void*)envBottomSRV, ImVec2(displaySize, displaySize));
                    }
                    else {
                        ImGui::Text("Error");
                    }
                    ImGui::EndGroup();

                    ImGui::EndTabItem();
                }
            }

            // === Tab 4: BRDF LUT ===
            if (ImGui::BeginTabItem("BRDF LUT")) {
                ImGui::Text("BRDF lookup table for Split Sum Approximation");
                ImGui::Text("512x512 2D texture, auto-generated on startup");
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

                    ID3D11ShaderResourceView* brdfLutSRV = iblGen.GetBrdfLutSRV();
                    if (brdfLutSRV) {
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

                        ImGui::Image((void*)brdfLutSRV, ImVec2(lutDisplaySize, lutDisplaySize));

                        ImGui::Separator();
                        ImGui::TextWrapped("Expected appearance: Bright in top-left (smooth + perpendicular), "
                            "dark in bottom-right (rough + grazing). "
                            "Red and green channels should have similar but slightly different gradients.");
                    }
                }  // End of showBrdfLutViz

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
