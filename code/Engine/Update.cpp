#include "Update.h"
#include "Camera.h"

void Update::OnKeyDown(uint32_t vk){ if(vk<256) m_keys[vk]=true; }
void Update::OnKeyUp(uint32_t vk){ if(vk<256) m_keys[vk]=false; }
void Update::OnRButton(bool down){ m_rmb = down; }

void Update::OnMouseDelta(int dx,int dy){
    if(!m_rmb || !m_cam) return;
    const float sens=0.0022f;
    m_cam->Rotate(-dx*sens, -dy*sens);  // ✅ 使用新的 Rotate 方法
}

void Update::Tick(float dt){
    if(!m_cam) return;
    const float sp=2.0f;
    if(m_keys['W']) m_cam->MoveForward(+sp*dt);
    if(m_keys['S']) m_cam->MoveForward(-sp*dt);
    if(m_keys['A']) m_cam->MoveRight(+sp*dt);
    if(m_keys['D']) m_cam->MoveRight(-sp*dt);
    if(m_keys['Q']) m_cam->MoveUp(-sp*dt);
    if(m_keys['E']) m_cam->MoveUp(+sp*dt);
}
