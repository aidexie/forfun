#pragma once
#include <string>

class CScene;
class CCamera;  // ✅ 改用 CCamera
struct ImVec2;
struct ID3D11ShaderResourceView;
class CMainPass;

namespace Panels {
    ImVec2 GetViewportLastSize();

    void DrawDockspace(bool* pOpen, CScene& scene, CMainPass* mainPass);
    void DrawHierarchy(CScene& scene);
    void DrawInspector(CScene& scene);
    void DrawViewport(CScene& scene, CCamera& editorCam,  // ✅ 改用 CCamera
        ID3D11ShaderResourceView* srv,
        size_t srcWidth, size_t srcHeight,
        CMainPass* mainPass = nullptr);
    void DrawIrradianceDebug();  // Uses CScene::Instance() internally
    void ShowIrradianceDebug(bool show);
    bool IsIrradianceDebugVisible();

    // HDR Export Window
    void ShowHDRExportWindow(bool show);
    void DrawHDRExportWindow();

    // Scene Light Settings Window
    void ShowSceneLightSettings(bool show);
    void DrawSceneLightSettings(CMainPass* mainPass = nullptr);
    bool IsSceneLightSettingsVisible();

    // Material Editor Window
    void OpenMaterialEditor(const std::string& materialPath);
    void DrawMaterialEditor();
}
