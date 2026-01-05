#include "Panels.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/RenderPipeline.h"
#include "Engine/Rendering/ClusteredLightingPass.h"
#include "Engine/Rendering/SSAOPass.h"
#include "Engine/Rendering/Deferred/DeferredRenderPipeline.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include "Engine/Rendering/Lightmap/LightmapBaker.h"
#include "Engine/Rendering/Lightmap/Lightmap2DManager.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <windows.h>
#include <commdlg.h>

static bool s_showWindow = true;  // Default: hidden (user opens via menu)
static bool s_isBaking = false;    // Volumetric Lightmap baking state
static bool s_is2DLightmapBaking = false;  // 2D Lightmap baking state

// 2D Lightmap config (baker is now owned by CScene to avoid static destruction order issues)
static CLightmapBaker::Config s_lightmap2DConfig;

// Bake configuration state (persisted across frames)
static SLightmapBakeConfig s_bakeConfig;

// Deferred GPU bake request (executed at start of next frame)
static bool s_pendingGPUBake = false;
static CVolumetricLightmap::Config s_pendingBakeVLConfig;

// Deferred 2D Lightmap bake request
static bool s_pending2DLightmapBake = false;

void Panels::DrawSceneLightSettings(CRenderPipeline* pipeline) {
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
        // Diffuse GI Section
        // ============================================
        ImGui::Text("Diffuse Global Illumination");
        ImGui::Separator();

        // Diffuse GI Mode dropdown
        const char* diffuseGIModes[] = { "Volumetric Lightmap", "Global IBL", "None", "2D Lightmap" };
        int currentMode = static_cast<int>(settings.diffuseGIMode);
        ImGui::PushItemWidth(200);
        if (ImGui::Combo("Diffuse GI Mode", &currentMode, diffuseGIModes, IM_ARRAYSIZE(diffuseGIModes))) {
            settings.diffuseGIMode = static_cast<EDiffuseGIMode>(currentMode);
            CFFLog::Info("[LightSettings] Diffuse GI Mode: %s", diffuseGIModes[currentMode]);
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Volumetric Lightmap: Per-pixel GI from baked 3D lightmap\n"
                "Global IBL: Use skybox irradiance (ambient)\n"
                "None: Disable diffuse GI (for baking first pass)\n"
                "2D Lightmap: UV2-based baked diffuse GI texture");
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
        ImGui::Separator();

        // ============================================
        // Bake Backend Selection
        // ============================================
        ImGui::Text("Bake Settings:");

        // Backend selection
        const char* backends[] = { "CPU (Path Trace)", "GPU (DXR Ray Tracing)" };
        int currentBackend = static_cast<int>(s_bakeConfig.backend);
        ImGui::PushItemWidth(200);
        if (ImGui::Combo("Backend##VLBake", &currentBackend, backends, IM_ARRAYSIZE(backends))) {
            s_bakeConfig.backend = static_cast<ELightmapBakeBackend>(currentBackend);
        }
        ImGui::PopItemWidth();

        // Check DXR availability
        bool dxrAvailable = volumetricLightmap.IsDXRBakingAvailable();
        if (s_bakeConfig.backend == ELightmapBakeBackend::GPU_DXR && !dxrAvailable) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "(DXR not available - will fallback to CPU)");
        }

        ImGui::Spacing();

        // Backend-specific settings
        if (s_bakeConfig.backend == ELightmapBakeBackend::GPU_DXR) {
            // GPU settings
            ImGui::Text("GPU Settings:");
            ImGui::PushItemWidth(150);
            ImGui::SliderInt("Samples/Pass##GPU", &s_bakeConfig.gpuSamplesPerVoxel, 64, 512);
            ImGui::SliderInt("Accumulation Passes##GPU", &s_bakeConfig.gpuAccumulationPasses, 1, 64);
            ImGui::SliderInt("Max Bounces##GPU", &s_bakeConfig.gpuMaxBounces, 1, 8);
            ImGui::SliderFloat("Sky Intensity##GPU", &s_bakeConfig.gpuSkyIntensity, 0.0f, 5.0f, "%.2f");
            ImGui::PopItemWidth();

            int totalSamples = s_bakeConfig.gpuSamplesPerVoxel * s_bakeConfig.gpuAccumulationPasses;
            ImGui::TextDisabled("Total samples/voxel: %d", totalSamples);
        } else {
            // CPU settings
            ImGui::Text("CPU Settings:");
            ImGui::PushItemWidth(150);
            ImGui::SliderInt("Samples/Voxel##CPU", &s_bakeConfig.cpuSamplesPerVoxel, 64, 16384);
            ImGui::SliderInt("Max Bounces##CPU", &s_bakeConfig.cpuMaxBounces, 1, 8);
        }

        ImGui::Spacing();

        // Bake buttons
        if (s_isBaking || s_pendingGPUBake) {
            ImGui::BeginDisabled();
            const char* statusText = s_pendingGPUBake ? "Bake pending (next frame)..." : "Baking...";
            ImGui::Button(statusText, ImVec2(250, 30));
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Build & Bake Volumetric Lightmap", ImVec2(250, 30))) {
                // Prepare config
                s_pendingBakeVLConfig.volumeMin = vlConfig.volumeMin;
                s_pendingBakeVLConfig.volumeMax = vlConfig.volumeMax;
                s_pendingBakeVLConfig.minBrickWorldSize = vlConfig.minBrickWorldSize;

                s_pendingGPUBake = true;
                CFFLog::Info("[VolumetricLightmap] bake requested - will execute at start of next frame");
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
        // 2D Lightmap Section
        // ============================================
        ImGui::Text("2D Lightmap (UV2-based)");
        ImGui::Separator();

        // Atlas settings
        ImGui::Text("Atlas Settings:");
        ImGui::PushItemWidth(150);
        ImGui::SliderInt("Resolution##LM2D", &s_lightmap2DConfig.atlasConfig.resolution, 256, 4096);
        ImGui::SliderInt("Texels/Unit##LM2D", &s_lightmap2DConfig.atlasConfig.texelsPerUnit, 4, 64);
        ImGui::SliderInt("Padding##LM2D", &s_lightmap2DConfig.atlasConfig.padding, 1, 8);
        ImGui::PopItemWidth();

        ImGui::Spacing();

        // Bake settings
        ImGui::Text("Bake Settings:");
        ImGui::PushItemWidth(150);
        ImGui::SliderInt("Samples/Texel##LM2D", &s_lightmap2DConfig.bakeConfig.samplesPerTexel, 16, 512);
        ImGui::SliderInt("Max Bounces##LM2D", &s_lightmap2DConfig.bakeConfig.maxBounces, 1, 8);
        ImGui::SliderFloat("Sky Intensity##LM2D", &s_lightmap2DConfig.bakeConfig.skyIntensity, 0.0f, 5.0f, "%.2f");
        ImGui::Checkbox("Enable OIDN Denoiser##LM2D", &s_lightmap2DConfig.bakeConfig.enableDenoiser);
        ImGui::PopItemWidth();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Intel Open Image Denoise - AI-based denoising\nfor cleaner lightmaps with fewer samples.");
        }

        ImGui::Spacing();

        // Bake button
        if (s_is2DLightmapBaking || s_pending2DLightmapBake) {
            ImGui::BeginDisabled();
            const char* statusText = s_pending2DLightmapBake ? "Bake pending (next frame)..." : "Baking 2D Lightmap...";
            ImGui::Button(statusText, ImVec2(200, 30));
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Bake 2D Lightmap", ImVec2(200, 30))) {
                // Defer bake to start of next frame to avoid command list conflicts
                s_pending2DLightmapBake = true;
                CFFLog::Info("[Lightmap2D] Bake requested - will execute at start of next frame");
            }
        }

        // Reload button (only if lightmap is loaded)
        auto& lightmap2D = CScene::Instance().GetLightmap2D();
        if (lightmap2D.IsLoaded()) {
            ImGui::SameLine();
            if (ImGui::Button("Reload##LM2D")) {
                if (lightmap2D.ReloadLightmap()) {
                    CFFLog::Info("[Lightmap2D] Reloaded successfully");
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reload lightmap from disk:\n%s", lightmap2D.GetLoadedPath().c_str());
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Bakes diffuse GI into a 2D texture atlas.\n"
                "Requires UV2 coordinates on meshes.\n"
                "Uses GPU DXR path tracing for irradiance calculation.");
        }

        // Show loaded status
        if (lightmap2D.IsLoaded()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Loaded: %s", lightmap2D.GetLoadedPath().c_str());
            ImGui::TextDisabled("Infos: %d entries", lightmap2D.GetLightmapInfoCount());
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
                auto& clusteredPass = pipeline->GetClusteredLightingPass();
                switch (debugModeIndex) {
                    case 0: clusteredPass.SetDebugMode(CClusteredLightingPass::EDebugMode::None); break;
                    case 1: clusteredPass.SetDebugMode(CClusteredLightingPass::EDebugMode::LightCountHeatmap); break;
                    case 2: clusteredPass.SetDebugMode(CClusteredLightingPass::EDebugMode::ClusterAABB); break;
                }
                CFFLog::Info("Clustered lighting debug mode: %s", debugModes[debugModeIndex]);
            }

            ImGui::Spacing();
        }

        // ============================================
        // SSAO Section (Screen-Space Ambient Occlusion)
        // ============================================
        CDeferredRenderPipeline* deferredPipeline = dynamic_cast<CDeferredRenderPipeline*>(pipeline);
        if (deferredPipeline) {
            ImGui::Text("Screen-Space Ambient Occlusion (SSAO)");
            ImGui::Separator();

            auto& ssaoSettings = deferredPipeline->GetSSAOPass().GetSettings();

            ImGui::Checkbox("Enable##SSAO", &ssaoSettings.enabled);

            if (ssaoSettings.enabled) {
                ImGui::PushItemWidth(150);
                ImGui::SliderFloat("Radius##SSAO", &ssaoSettings.radius, 0.1f, 2.0f, "%.2f");
                ImGui::SliderFloat("Intensity##SSAO", &ssaoSettings.intensity, 0.0f, 3.0f, "%.2f");
                ImGui::SliderFloat("Falloff Start##SSAO", &ssaoSettings.falloffStart, 0.0f, 1.0f, "%.2f");
                ImGui::SliderInt("Slices##SSAO", &ssaoSettings.numSlices,
                    SSAOConfig::MIN_SLICES, SSAOConfig::MAX_SLICES);
                ImGui::SliderInt("Steps##SSAO", &ssaoSettings.numSteps, 2, 8);
                ImGui::SliderInt("Blur Radius##SSAO", &ssaoSettings.blurRadius, 1,
                    SSAOConfig::MAX_BLUR_RADIUS);
                ImGui::PopItemWidth();

                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Radius: View-space AO radius (larger = more spread)\n"
                        "Intensity: AO strength multiplier\n"
                        "Falloff Start: Distance falloff start (0-1 of radius)\n"
                        "Slices: Number of direction slices (quality)\n"
                        "Steps: Ray march steps per direction\n"
                        "Blur Radius: Bilateral blur radius (edge-preserving)");
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();
        }

        // ============================================
        // Post-Processing: Bloom Section
        // ============================================
        ImGui::Text("Post-Processing: Bloom");
        ImGui::Separator();

        auto& bloom = settings.bloom;
        ImGui::Checkbox("Enable##Bloom", &bloom.enabled);

        if (bloom.enabled) {
            ImGui::PushItemWidth(150);
            ImGui::SliderFloat("Threshold##Bloom", &bloom.threshold, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Intensity##Bloom", &bloom.intensity, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Scatter##Bloom", &bloom.scatter, 0.0f, 1.0f, "%.2f");
            ImGui::PopItemWidth();

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Threshold: Luminance cutoff for bloom extraction\n"
                    "Intensity: Bloom brightness multiplier\n"
                    "Scatter: Blend factor between blur levels (higher = more diffuse glow)");
            }
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

bool Panels::ExecutePendingGPUBake() {
    if (!s_pendingGPUBake) {
        return false;
    }

    CFFLog::Info("[VolumetricLightmap] Executing deferred GPU bake at frame start...");
    s_pendingGPUBake = false;
    s_isBaking = true;

    auto& vl = CScene::Instance().GetVolumetricLightmap();
    auto& vlConfig = CScene::Instance().GetLightSettings().volumetricLightmap;

    vl.Shutdown();
    if (vl.Initialize(s_pendingBakeVLConfig)) {
        vl.BuildOctree(CScene::Instance());
        CFFLog::Info("[VolumetricLightmap] Starting bake with GPU (DXR) backend...");
        vl.BakeAllBricks(CScene::Instance(), s_bakeConfig);

        if (vl.CreateGPUResources()) {
            vl.SetEnabled(true);
            vlConfig.enabled = true;
            CFFLog::Info("[VolumetricLightmap] GPU bake complete and resources created!");
        } else {
            CFFLog::Error("[VolumetricLightmap] Failed to create GPU resources!");
        }
    } else {
        CFFLog::Error("[VolumetricLightmap] Failed to initialize!");
    }

    s_isBaking = false;
    return true;
}

bool Panels::ExecutePending2DLightmapBake() {
    if (!s_pending2DLightmapBake) {
        return false;
    }

    CFFLog::Info("[Lightmap2D] Executing deferred 2D lightmap bake at frame start...");
    s_pending2DLightmapBake = false;
    s_is2DLightmapBaking = true;
    // s_lightmap2DConfig.bakeConfig.debugExportImages = true;
    CScene& scene = CScene::Instance();
    std::string lightmapPath = scene.GetLightmapPath();
    CLightmapBaker& baker = scene.GetLightmapBaker();

    // Bake (includes assign indices + save to file + transfer to manager)
    // Note: Baker now calls SetBakedData() directly, no need to reload from disk
    if (baker.Bake(scene, s_lightmap2DConfig, lightmapPath)) {
        CFFLog::Info("[Lightmap2D] Bake complete! Atlas size: %dx%d",
                    baker.GetAtlasWidth(),
                    baker.GetAtlasHeight());
    } else {
        CFFLog::Error("[Lightmap2D] Bake failed!");
    }

    s_is2DLightmapBaking = false;
    return true;
}
