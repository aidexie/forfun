#pragma once
#include <cstdint>
class CCamera;  // ✅ 使用新的 CCamera 类
class Update {
public:
    void BindCamera(CCamera* cam){ m_cam = cam; }
    void OnKeyDown(uint32_t vk);
    void OnKeyUp(uint32_t vk);
    void OnRButton(bool down);
    void OnMouseDelta(int dx, int dy);
    void Tick(float dt);
private:
    bool m_keys[256]{};
    bool m_rmb=false;
    CCamera* m_cam=nullptr;
};
