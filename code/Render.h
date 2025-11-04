
#pragma once
#include <windows.h>
#include <cstdint>
#include <DirectXMath.h>

class Renderer; // from user's code
class Camera;

class Render {
public:
    bool Initialize(HWND hwnd, uint32_t w, uint32_t h);
    void Shutdown();
    void SetCamera(Camera* cam); // currently not wiring into legacy; placeholder
    void OnRButton(bool down);
    void OnMouseDelta(int dx, int dy);
    void Frame(float dt);
private:
    Renderer* m_impl = nullptr; // legacy renderer
};
