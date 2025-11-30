#pragma once
#include <string>

class CScene;
class CCamera;  // ✅ 改用 CCamera
struct ImVec2;
struct ID3D11ShaderResourceView;
class CForwardRenderPipeline;

namespace Panels {
    ImVec2 GetViewportLastSize();

    void DrawDockspace(bool* pOpen, CScene& scene, CForwardRenderPipeline* pipeline);
    void DrawHierarchy(CScene& scene);
    void DrawInspector(CScene& scene);
    void DrawViewport(CScene& scene, CCamera& editorCam,  // ✅ 改用 CCamera
        ID3D11ShaderResourceView* srv,
        size_t srcWidth, size_t srcHeight,
        CForwardRenderPipeline* pipeline = nullptr);
    void DrawIrradianceDebug();  // Uses CScene::Instance() internally
    void ShowIrradianceDebug(bool show);
    bool IsIrradianceDebugVisible();

    // HDR Export Window
    void ShowHDRExportWindow(bool show);
    void DrawHDRExportWindow();

    // Scene Light Settings Window
    void ShowSceneLightSettings(bool show);
    void DrawSceneLightSettings(CForwardRenderPipeline* pipeline = nullptr);
    bool IsSceneLightSettingsVisible();

    // Material Editor Window
    void OpenMaterialEditor(const std::string& materialPath);
    void DrawMaterialEditor();
}
