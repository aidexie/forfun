#pragma once
struct Scene;
struct EditorCamera;
struct ImVec2;
struct ID3D11ShaderResourceView;

namespace Panels {
    ImVec2 GetViewportLastSize();

    void DrawDockspace(bool* pOpen, Scene& scene);
    void DrawHierarchy(Scene& scene);
    void DrawInspector(Scene& scene);
    void DrawViewport(Scene& scene, EditorCamera& editorCam,
        ID3D11ShaderResourceView* srv,
        size_t srcWidth, size_t srcHeight);
}
