#pragma once
#include <DirectXMath.h>

// ============================================
// CCamera: 相机数据 + 矩阵计算
// ============================================
// 内部表示：**只用 Quaternion**（无万向锁）
// 外部接口：支持 Euler angles (yaw/pitch) 和 LookAt
// ============================================
class CCamera {
public:
    // === Transform ===
    DirectX::XMFLOAT3 position{0, 5, -5};

    // === Projection ===
    float fovY = DirectX::XM_PIDIV4;  // 45°
    float aspectRatio = 16.0f / 9.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;

    // === 构造函数 ===
    CCamera();

    // === 矩阵计算 ===
    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMMATRIX GetViewProjectionMatrix() const;

    // === 外部接口：设置朝向 ===
    // LookAt 模式
    void SetLookAt(
        const DirectX::XMFLOAT3& eye,
        const DirectX::XMFLOAT3& target,
        const DirectX::XMFLOAT3& up = {0, 1, 0}
    );

    // Euler angles 模式（yaw/pitch，弧度）
    void SetYawPitch(float yaw, float pitch);

    // 增量旋转（编辑器相机控制）
    void Rotate(float deltaYaw, float deltaPitch);

    // === 获取方向向量 ===
    DirectX::XMFLOAT3 GetForward() const;
    DirectX::XMFLOAT3 GetRight() const;
    DirectX::XMFLOAT3 GetUp() const;

    // === 获取当前 Euler angles（从 Quaternion 反算，用于 UI 显示）===
    float GetYaw() const;
    float GetPitch() const;

    // === 移动 ===
    void MoveForward(float distance);
    void MoveRight(float distance);
    void MoveUp(float distance);

private:
    // ============================================
    // 内部表示：只用 Quaternion
    // ============================================
    DirectX::XMFLOAT4 m_rotation{0, 0, 0, 1};  // identity quaternion

    // 缓存的 yaw/pitch（用于增量旋转 Rotate()）
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
};
