
#pragma once
#include <cstdint>
class Camera;
class Update {
public:
    void BindCamera(Camera* cam){ m_cam = cam; }
    void OnKeyDown(uint32_t vk);
    void OnKeyUp(uint32_t vk);
    void OnRButton(bool down);
    void OnMouseDelta(int dx, int dy);
    void Tick(float dt);
private:
    bool m_keys[256]{};
    bool m_rmb=false;
    Camera* m_cam=nullptr;
};
