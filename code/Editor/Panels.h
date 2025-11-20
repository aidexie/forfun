#pragma once
class Scene;
struct EditorCamera;
struct ImVec2;
struct ID3D11ShaderResourceView;
class MainPass;

namespace Panels {
    ImVec2 GetViewportLastSize();

    void DrawDockspace(bool* pOpen, Scene& scene, MainPass* mainPass);
    void DrawHierarchy(Scene& scene);
    void DrawInspector(Scene& scene);
    void DrawViewport(Scene& scene, EditorCamera& editorCam,
        ID3D11ShaderResourceView* srv,
        size_t srcWidth, size_t srcHeight);
    void DrawIrradianceDebug();  // Uses Scene::Instance() internally
}
