
#include "Render.h"
#include "Renderer.h"
#include "Camera.h"

bool Render::Initialize(HWND hwnd, uint32_t w, uint32_t h){
    m_impl = new Renderer();
    return m_impl->Initialize(hwnd, w, h);
}
void Render::Shutdown(){
    if(m_impl){ m_impl->Shutdown(); delete m_impl; m_impl=nullptr; }
}
void Render::SetCamera(Camera*){
    // NOTE: Legacy Renderer keeps its own camera (pos/yaw/pitch). To fully adopt Engine.Camera,
    // you can expose a SetCameraPose on Renderer and call it here.
}
void Render::OnRButton(bool down){
    if(!m_impl) return;
    m_impl->OnRButton(down);
}
void Render::OnMouseDelta(int dx, int dy){
    if(!m_impl) return;
    m_impl->OnMouseDelta(dx, dy);
}
void Render::Frame(float dt){
    if(!m_impl) return;
    m_impl->Render(); // legacy render drives per-frame draw
}
