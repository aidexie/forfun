#pragma once
class CScene;
struct EditorCamera;
struct ImVec2;
struct ID3D11ShaderResourceView;
class CMainPass;

namespace Panels {
    ImVec2 GetViewportLastSize();

    void DrawDockspace(bool* pOpen, CScene& scene, CMainPass* mainPass);
    void DrawHierarchy(CScene& scene);
    void DrawInspector(CScene& scene);
    void DrawViewport(CScene& scene, EditorCamera& editorCam,
        ID3D11ShaderResourceView* srv,
        size_t srcWidth, size_t srcHeight,
        CMainPass* mainPass = nullptr);
    void DrawIrradianceDebug();  // Uses CScene::Instance() internally

    // HDR Export Window
    void ShowHDRExportWindow(bool show);
    void DrawHDRExportWindow();

    // Scene Light Settings Window
    void ShowSceneLightSettings(bool show);
    void DrawSceneLightSettings();
    bool IsSceneLightSettingsVisible();
}
