#pragma once
#include <string>

class CScene;
class CCamera;  // ✅ 改用 CCamera
struct ImVec2;
class CRenderPipeline;

namespace Panels {
    ImVec2 GetViewportLastSize();

    void DrawDockspace(bool* pOpen, CScene& scene, CRenderPipeline* pipeline);
    void DrawHierarchy(CScene& scene);
    void DrawInspector(CScene& scene);
    void DrawViewport(CScene& scene, CCamera& editorCam,  // ✅ 改用 CCamera
        void* srv,  // ID3D11ShaderResourceView* or equivalent (passed to ImGui)
        size_t srcWidth, size_t srcHeight,
        CRenderPipeline* pipeline = nullptr);
    void DrawIrradianceDebug();  // Uses CScene::Instance() internally
    void ShowIrradianceDebug(bool show);
    bool IsIrradianceDebugVisible();

    // HDR Export Window
    void ShowHDRExportWindow(bool show);
    void DrawHDRExportWindow();

    // Scene Light Settings Window
    void ShowSceneLightSettings(bool show);
    void DrawSceneLightSettings(CRenderPipeline* pipeline = nullptr);
    bool IsSceneLightSettingsVisible();

    // Material Editor Window
    void OpenMaterialEditor(const std::string& materialPath);
    void DrawMaterialEditor();

    // Deferred GPU Bake (call at start of frame, before scene rendering)
    // Returns true if a bake was executed this frame
    bool ExecutePendingGPUBake();
    bool ExecutePending2DLightmapBake();
}
