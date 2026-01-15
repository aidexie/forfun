#include "Panels.h"
#include "EditorContext.h"
#include "imgui.h"
#include "Engine/Scene.h"
#include "Engine/SceneLightSettings.h"
#include "Engine/Rendering/RenderPipeline.h"
#include "Engine/Rendering/ClusteredLightingPass.h"
#include "Engine/Rendering/SSAOPass.h"
#include "Engine/Rendering/SSRPass.h"
#include "Engine/Rendering/HiZPass.h"
#include "Engine/Rendering/Deferred/DeferredRenderPipeline.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include "Engine/Rendering/Lightmap/LightmapBaker.h"
#include "Engine/Rendering/Lightmap/Lightmap2DManager.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <windows.h>
#include <commdlg.h>

// Window visibility state
static bool s_showWindow = true;

// Baking state flags
static bool s_isBaking = false;
static bool s_is2DLightmapBaking = false;

// Configuration state (persisted across frames)
static CLightmapBaker::Config s_lightmap2DConfig;
static SLightmapBakeConfig s_bakeConfig;

// Deferred bake requests (executed at start of next frame)
static bool s_pendingGPUBake = false;
static bool s_pending2DLightmapBake = false;
static CVolumetricLightmap::Config s_pendingBakeVLConfig;

// ============================================
// Helper Functions
// ============================================

static void SectionHeader(const char* label) {
    ImGui::Text("%s", label);
    ImGui::Separator();
}

static void DoubleSpacing() {
    ImGui::Spacing();
    ImGui::Spacing();
}

static void HelpTooltip(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

// ============================================
// Section Drawing Functions
// ============================================

static void DrawEnvironmentSection(CSceneLightSettings& settings) {
    SectionHeader("Environment");

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
            std::string normalizedPath = FFPath::Normalize(fileName);
            settings.skyboxAssetPath = normalizedPath;
            CScene::Instance().ReloadEnvironment(settings.skyboxAssetPath);
        }
    }

    DoubleSpacing();
}

static void DrawDiffuseGISection(CSceneLightSettings& settings) {
    SectionHeader("Diffuse Global Illumination");

    const char* diffuseGIModes[] = { "Volumetric Lightmap", "Global IBL", "None", "2D Lightmap" };
    int currentMode = static_cast<int>(settings.diffuseGIMode);

    ImGui::PushItemWidth(200);
    if (ImGui::Combo("Diffuse GI Mode", &currentMode, diffuseGIModes, IM_ARRAYSIZE(diffuseGIModes))) {
        settings.diffuseGIMode = static_cast<EDiffuseGIMode>(currentMode);
        CFFLog::Info("[LightSettings] Diffuse GI Mode: %s", diffuseGIModes[currentMode]);
    }
    ImGui::PopItemWidth();

    HelpTooltip(
        "Volumetric Lightmap: Per-pixel GI from baked 3D lightmap\n"
        "Global IBL: Use skybox irradiance (ambient)\n"
        "None: Disable diffuse GI (for baking first pass)\n"
        "2D Lightmap: UV2-based baked diffuse GI texture");

    DoubleSpacing();
}

static void DrawVolumetricLightmapSection(CSceneLightSettings& settings) {
    SectionHeader("Volumetric Lightmap");

    auto& vlConfig = settings.volumetricLightmap;
    auto& volumetricLightmap = CScene::Instance().GetVolumetricLightmap();

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

    HelpTooltip(
        "Minimum size of the finest bricks.\n"
        "Smaller = more precision, more memory.\n"
        "Recommended: 1.0 - 4.0 meters.");

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

    // Bake settings
    ImGui::Text("Bake Settings:");

    const char* backends[] = { "CPU (Path Trace)", "GPU (DXR Ray Tracing)" };
    int currentBackend = static_cast<int>(s_bakeConfig.backend);

    ImGui::PushItemWidth(200);
    if (ImGui::Combo("Backend##VLBake", &currentBackend, backends, IM_ARRAYSIZE(backends))) {
        s_bakeConfig.backend = static_cast<ELightmapBakeBackend>(currentBackend);
    }
    ImGui::PopItemWidth();

    bool dxrAvailable = volumetricLightmap.IsDXRBakingAvailable();
    if (s_bakeConfig.backend == ELightmapBakeBackend::GPU_DXR && !dxrAvailable) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "(DXR not available - will fallback to CPU)");
    }

    ImGui::Spacing();

    // Backend-specific settings
    ImGui::PushItemWidth(150);
    if (s_bakeConfig.backend == ELightmapBakeBackend::GPU_DXR) {
        ImGui::Text("GPU Settings:");
        ImGui::SliderInt("Samples/Pass##GPU", &s_bakeConfig.gpuSamplesPerVoxel, 64, 512);
        ImGui::SliderInt("Accumulation Passes##GPU", &s_bakeConfig.gpuAccumulationPasses, 1, 64);
        ImGui::SliderInt("Max Bounces##GPU", &s_bakeConfig.gpuMaxBounces, 1, 8);
        ImGui::SliderFloat("Sky Intensity##GPU", &s_bakeConfig.gpuSkyIntensity, 0.0f, 5.0f, "%.2f");

        int totalSamples = s_bakeConfig.gpuSamplesPerVoxel * s_bakeConfig.gpuAccumulationPasses;
        ImGui::TextDisabled("Total samples/voxel: %d", totalSamples);
    } else {
        ImGui::Text("CPU Settings:");
        ImGui::SliderInt("Samples/Voxel##CPU", &s_bakeConfig.cpuSamplesPerVoxel, 64, 16384);
        ImGui::SliderInt("Max Bounces##CPU", &s_bakeConfig.cpuMaxBounces, 1, 8);
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();

    // Bake buttons
    if (s_isBaking || s_pendingGPUBake) {
        ImGui::BeginDisabled();
        const char* statusText = s_pendingGPUBake ? "Bake pending (next frame)..." : "Baking...";
        ImGui::Button(statusText, ImVec2(250, 30));
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Build & Bake Volumetric Lightmap", ImVec2(250, 30))) {
            s_pendingBakeVLConfig.volumeMin = vlConfig.volumeMin;
            s_pendingBakeVLConfig.volumeMax = vlConfig.volumeMax;
            s_pendingBakeVLConfig.minBrickWorldSize = vlConfig.minBrickWorldSize;
            s_pendingGPUBake = true;
            CFFLog::Info("[VolumetricLightmap] bake requested - will execute at start of next frame");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear##VL")) {
        CScene::Instance().GetVolumetricLightmap().Shutdown();
        vlConfig.enabled = false;
        CFFLog::Info("[VolumetricLightmap] Cleared.");
    }

    ImGui::Spacing();

    // Debug visualization
    if (volumetricLightmap.HasBakedData()) {
        bool debugDraw = volumetricLightmap.IsDebugDrawEnabled();
        if (ImGui::Checkbox("Show Octree Debug##VL", &debugDraw)) {
            volumetricLightmap.SetDebugDrawEnabled(debugDraw);
        }
        HelpTooltip(
            "Visualize the octree brick structure.\n"
            "Colors indicate subdivision levels:\n"
            "Red=0, Orange=1, Yellow=2, Green=3, etc.");
    }

    DoubleSpacing();
}

static void DrawLightmap2DSection() {
    SectionHeader("2D Lightmap (UV2-based)");

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
            s_pending2DLightmapBake = true;
            CFFLog::Info("[Lightmap2D] Bake requested - will execute at start of next frame");
        }
    }

    // Reload button
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

    HelpTooltip(
        "Bakes diffuse GI into a 2D texture atlas.\n"
        "Requires UV2 coordinates on meshes.\n"
        "Uses GPU DXR path tracing for irradiance calculation.");

    // Show loaded status
    if (lightmap2D.IsLoaded()) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Loaded: %s", lightmap2D.GetLoadedPath().c_str());
        ImGui::TextDisabled("Infos: %d entries", lightmap2D.GetLightmapInfoCount());
    }

    DoubleSpacing();
}

static void DrawClusteredLightingDebugSection(CRenderPipeline* pipeline) {
    SectionHeader("Clustered Lighting Debug");

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

static void DrawSSAOSection(CDeferredRenderPipeline* deferredPipeline) {
    SectionHeader("Screen-Space Ambient Occlusion (SSAO)");

    auto& ssaoSettings = deferredPipeline->GetSSAOPass().GetSettings();
    auto& showFlags = CEditorContext::Instance().GetShowFlags();

    ImGui::Checkbox("Enable##SSAO", &showFlags.SSAO);

    if (showFlags.SSAO) {
        ImGui::PushItemWidth(150);
        ImGui::SliderFloat("Radius##SSAO", &ssaoSettings.radius, 0.1f, 2.0f, "%.2f");
        ImGui::SliderFloat("Intensity##SSAO", &ssaoSettings.intensity, 0.0f, 13.0f, "%.2f");
        ImGui::SliderFloat("Falloff Start##SSAO", &ssaoSettings.falloffStart, 0.0f, 1.0f, "%.2f");
        ImGui::SliderInt("Slices##SSAO", &ssaoSettings.numSlices,
            SSAOConfig::MIN_SLICES, SSAOConfig::MAX_SLICES);
        ImGui::SliderInt("Steps##SSAO", &ssaoSettings.numSteps, 2, 8);
        ImGui::SliderInt("Blur Radius##SSAO", &ssaoSettings.blurRadius, 1,
            SSAOConfig::MAX_BLUR_RADIUS);
        ImGui::PopItemWidth();

        HelpTooltip(
            "Radius: View-space AO radius (larger = more spread)\n"
            "Intensity: AO strength multiplier\n"
            "Falloff Start: Distance falloff start (0-1 of radius)\n"
            "Slices: Number of direction slices (quality)\n"
            "Steps: Ray march steps per direction\n"
            "Blur Radius: Bilateral blur radius (edge-preserving)");
    }

    DoubleSpacing();
}

static void DrawSSRSection(CDeferredRenderPipeline* deferredPipeline) {
    SectionHeader("Screen-Space Reflections (SSR)");

    auto& ssrSettings = deferredPipeline->GetSSRPass().GetSettings();
    auto& showFlags = CEditorContext::Instance().GetShowFlags();

    // SSR requires Hi-Z
    if (!showFlags.HiZ) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "SSR requires Hi-Z to be enabled");
        if (ImGui::Button("Enable Hi-Z##SSR")) {
            showFlags.HiZ = true;
        }
        return;
    }

    ImGui::Checkbox("Enable##SSR", &showFlags.SSR);

    if (showFlags.SSR) {
        // Quality preset dropdown
        static const char* qualityNames[] = { "Low", "Medium", "High", "Ultra", "Custom" };
        int currentQuality = static_cast<int>(ssrSettings.quality);

        ImGui::PushItemWidth(150);
        if (ImGui::Combo("Quality##SSR", &currentQuality, qualityNames, 5)) {
            ssrSettings.ApplyPreset(static_cast<ESSRQuality>(currentQuality));
        }

        // Mode dropdown (ordered simple to complex)
        static const char* modeNames[] = { "Simple Linear", "HiZ Trace", "Stochastic", "Temporal" };
        int currentMode = static_cast<int>(ssrSettings.mode);
        if (ImGui::Combo("Mode##SSR", &currentMode, modeNames, 4)) {
            ssrSettings.mode = static_cast<ESSRMode>(currentMode);
        }

        HelpTooltip(
            "Simple Linear: Basic ray march (educational/debug)\n"
            "HiZ Trace: Single ray with Hi-Z acceleration (default)\n"
            "Stochastic: Multiple rays with GGX sampling\n"
            "Temporal: Stochastic + history accumulation (best quality)");

        // Intensity slider (always visible)
        ImGui::SliderFloat("Intensity##SSR", &ssrSettings.intensity, 0.0f, 2.0f, "%.2f");

        // Resolution scale slider
        ImGui::SliderFloat("Resolution Scale##SSR", &ssrSettings.resolutionScale, 0.25f, 1.0f, "%.2f");
        HelpTooltip("Render SSR at lower resolution for better performance.\n1.0 = Full resolution\n0.5 = Half resolution\n0.25 = Quarter resolution");

        // Stochastic/Temporal settings (only for modes that use multiple rays)
        if (ssrSettings.mode == ESSRMode::Stochastic || ssrSettings.mode == ESSRMode::Temporal) {
            if (ImGui::TreeNode("Stochastic Settings##SSR")) {
                ImGui::SliderInt("Rays/Pixel##SSR", &ssrSettings.numRays, 1, 8);
                ImGui::SliderFloat("BRDF Bias##SSR", &ssrSettings.brdfBias, 0.0f, 1.0f, "%.2f");
                HelpTooltip(
                    "Rays/Pixel: More rays = better quality, slower\n"
                    "BRDF Bias: 0=uniform sampling, 1=full GGX importance sampling");
                ImGui::TreePop();
            }
        }

        if (ssrSettings.mode == ESSRMode::Temporal) {
            if (ImGui::TreeNode("Temporal Settings##SSR")) {
                ImGui::SliderFloat("History Blend##SSR", &ssrSettings.temporalBlend, 0.0f, 0.98f, "%.2f");
                ImGui::SliderFloat("Motion Threshold##SSR", &ssrSettings.motionThreshold, 0.001f, 0.1f, "%.3f");
                HelpTooltip(
                    "History Blend: Higher = smoother but more ghosting\n"
                    "Motion Threshold: Higher = accept more motion before rejection");
                ImGui::TreePop();
            }
        }

        // Advanced settings (collapsible)
        if (ImGui::TreeNode("Advanced Settings##SSR")) {
            // Mark as custom when user changes advanced settings
            if (ImGui::SliderFloat("Max Distance##SSR", &ssrSettings.maxDistance, 10.0f, 200.0f, "%.1f"))
                ssrSettings.quality = ESSRQuality::Custom;
            if (ImGui::SliderFloat("Thickness##SSR", &ssrSettings.thickness, 0.01f, 2.0f, "%.2f"))
                ssrSettings.quality = ESSRQuality::Custom;
            if (ImGui::SliderFloat("Stride##SSR", &ssrSettings.stride, 0.5f, 4.0f, "%.1f"))
                ssrSettings.quality = ESSRQuality::Custom;
            if (ImGui::SliderInt("Max Steps##SSR", &ssrSettings.maxSteps, 16, 128))
                ssrSettings.quality = ESSRQuality::Custom;
            if (ImGui::SliderInt("Binary Steps##SSR", &ssrSettings.binarySearchSteps, 0, 16))
                ssrSettings.quality = ESSRQuality::Custom;
            if (ImGui::SliderFloat("Roughness Fade##SSR", &ssrSettings.roughnessFade, 0.1f, 1.0f, "%.2f"))
                ssrSettings.quality = ESSRQuality::Custom;

            ImGui::TreePop();
        }
        ImGui::PopItemWidth();

        HelpTooltip(
            "Quality: Preset balancing quality vs performance\n"
            "Mode: Algorithm for SSR computation\n"
            "Intensity: SSR reflection brightness multiplier\n"
            "Max Distance: Maximum ray travel distance (view-space)\n"
            "Thickness: Surface thickness for hit detection\n"
            "Stride: Initial ray step size (pixels)\n"
            "Max Steps: Maximum ray march iterations\n"
            "Binary Steps: Refinement iterations for hit accuracy\n"
            "Roughness Fade: Skip SSR above this roughness");
    }

    DoubleSpacing();
}

static void DrawGBufferDebugSection(CSceneLightSettings& settings) {
    SectionHeader("G-Buffer Debug Visualization");

    int currentDebugMode = static_cast<int>(settings.gBufferDebugMode);

    ImGui::PushItemWidth(200);
    if (ImGui::Combo("Debug Mode##GBuffer", &currentDebugMode,
                   GetGBufferDebugModeNames(),
                   GetGBufferDebugModeCount())) {
        settings.gBufferDebugMode = static_cast<EGBufferDebugMode>(currentDebugMode);
        CFFLog::Info("G-Buffer debug mode: %s", GetGBufferDebugModeNames()[currentDebugMode]);
    }
    ImGui::PopItemWidth();

    HelpTooltip("Visualize G-Buffer contents for debugging.\nSelect 'None' for normal rendering.");

    DoubleSpacing();
}

static void DrawBloomSection(CSceneLightSettings& settings) {
    SectionHeader("Post-Processing: Bloom");

    auto& bloom = settings.bloom;
    auto& showFlags = CEditorContext::Instance().GetShowFlags();

    ImGui::Checkbox("Enable##Bloom", &showFlags.Bloom);

    if (showFlags.Bloom) {
        ImGui::PushItemWidth(150);
        ImGui::SliderFloat("Threshold##Bloom", &bloom.threshold, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Intensity##Bloom", &bloom.intensity, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Scatter##Bloom", &bloom.scatter, 0.0f, 1.0f, "%.2f");
        ImGui::PopItemWidth();

        HelpTooltip(
            "Threshold: Luminance cutoff for bloom extraction\n"
            "Intensity: Bloom brightness multiplier\n"
            "Scatter: Blend factor between blur levels (higher = more diffuse glow)");
    }

    ImGui::Spacing();
}

// ============================================
// Main Panel Function
// ============================================

void Panels::DrawSceneLightSettings(CRenderPipeline* pipeline) {
    if (!s_showWindow) return;

    if (!ImGui::Begin("Scene Light Settings", &s_showWindow)) {
        ImGui::End();
        return;
    }

    auto& settings = CScene::Instance().GetLightSettings();

    DrawEnvironmentSection(settings);
    DrawDiffuseGISection(settings);
    DrawVolumetricLightmapSection(settings);
    DrawLightmap2DSection();

    if (pipeline) {
        DrawClusteredLightingDebugSection(pipeline);
    }

    CDeferredRenderPipeline* deferredPipeline = dynamic_cast<CDeferredRenderPipeline*>(pipeline);
    if (deferredPipeline) {
        DrawSSAOSection(deferredPipeline);
        DrawSSRSection(deferredPipeline);
        DrawGBufferDebugSection(settings);
    }

    DrawBloomSection(settings);

    // Apply button
    if (ImGui::Button("Apply Settings")) {
        if (!settings.skyboxAssetPath.empty()) {
            CScene::Instance().ReloadEnvironment(settings.skyboxAssetPath);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Settings auto-apply on change)");

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

    CScene& scene = CScene::Instance();
    std::string lightmapPath = scene.GetLightmapPath();
    CLightmapBaker& baker = scene.GetLightmapBaker();

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
