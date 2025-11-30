#pragma once
#include "../Engine/Camera.h"

// CEditorContext - 编辑器状态管理（单例）
// 职责：管理编辑器交互状态（相机控制、Gizmo 模式、Grid 显示等）
//
// 设计原则：
// - 编辑器层（Editor）不污染引擎层（Engine）
// - 交互逻辑与渲染逻辑分离
// - 可扩展：未来可添加 Gizmo 模式、Snapping、Grid 等编辑器状态
class CEditorContext {
public:
    static CEditorContext& Instance();

    // === 相机交互 ===
    void OnRButton(bool down);
    void OnMouseDelta(int dx, int dy, CCamera& camera);
    void Update(float dt, CCamera& camera);

    // === 参数配置 ===
    void SetMouseSensitivity(float sens) { m_mouseSensitivity = sens; }
    void SetMoveSpeed(float speed) { m_moveSpeed = speed; }
    float GetMouseSensitivity() const { return m_mouseSensitivity; }
    float GetMoveSpeed() const { return m_moveSpeed; }

private:
    CEditorContext() = default;  // 单例：私有构造函数
    CEditorContext(const CEditorContext&) = delete;
    CEditorContext& operator=(const CEditorContext&) = delete;

    // === 相机控制状态 ===
    bool m_rmbLook = false;  // 右键按下状态
    float m_mouseSensitivity = 0.0022f;
    float m_moveSpeed = 5.0f;

    // Phase 5 可扩展：
    // enum class GizmoMode { Translate, Rotate, Scale };
    // GizmoMode m_gizmoMode = GizmoMode::Translate;
    // bool m_showGrid = true;
    // float m_snapValue = 1.0f;
};
