#include "EditorContext.h"
#include <Windows.h>  // GetAsyncKeyState

// ============================================
// 单例实例
// ============================================
CEditorContext& CEditorContext::Instance() {
    static CEditorContext instance;
    return instance;
}

// ============================================
// 相机交互
// ============================================
void CEditorContext::OnRButton(bool down) {
    m_rmbLook = down;
}

void CEditorContext::OnMouseDelta(int dx, int dy, CCamera& camera) {
    if (!m_rmbLook) return;

    // 立即应用鼠标旋转
    camera.Rotate(-dx * m_mouseSensitivity, -dy * m_mouseSensitivity);
}

void CEditorContext::Update(float dt, CCamera& camera) {
    // 检测按键状态
    auto down = [](int vk){ return (GetAsyncKeyState(vk) & 0x8000) != 0; };

    // WASD 移动
    if (down('W')) camera.MoveForward(m_moveSpeed * dt);
    if (down('S')) camera.MoveForward(-m_moveSpeed * dt);
    if (down('D')) camera.MoveRight(m_moveSpeed * dt);
    if (down('A')) camera.MoveRight(-m_moveSpeed * dt);

    // QE 上下移动（可选）
    if (down('Q')) camera.MoveUp(-m_moveSpeed * dt);
    if (down('E')) camera.MoveUp(m_moveSpeed * dt);

    // R 键重置相机（回到默认位置）
    if (down('R')) {
        camera.SetLookAt({-6.0f, 0.8f, 0.0f}, {0, 0, 0});
    }
}
