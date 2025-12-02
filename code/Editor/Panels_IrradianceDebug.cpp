#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/Skybox.h"
#include "Core/DX11Context.h"
#include "Core/FFLog.h"
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
            ID3D11ShaderResourceView* envMap = skybox.GetEnvironmentMap();
            if (!envMap) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "No environment map loaded!");
                ImGui::Text("Load a skybox first.");
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

                ID3D11ShaderResourceView* brdfLutSRV = CScene::Instance().GetProbeManager().GetBrdfLutSRV();
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
