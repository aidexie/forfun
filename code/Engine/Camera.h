#pragma once
#include <DirectXMath.h>
#include <numbers>
// CCamera: 相机数据 + 矩阵计算（Pure Data）
// 职责：管理相机的 Transform 和 Projection 参数，计算 View/Projection 矩阵
// 不包含交互逻辑（鼠标、键盘控制由外部处理）
class CCamera {
public:
    // === Transform 参数 ===
    DirectX::XMFLOAT3 position{0, 5, -5};  // 相机位置
    float yaw = 0.5f*std::numbers::pi;    // 水平旋转（弧度，绕 Y 轴）
    float pitch = -0.1f*std::numbers::pi;  // 垂直旋转（弧度，绕 X 轴）

    // === Projection 参数 ===
    float fovY = DirectX::XM_PIDIV4;  // 垂直视野角度（弧度，默认 45°）
    float aspectRatio = 16.0f / 9.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;

    // === 构造函数 ===
    CCamera() = default;

    // === 矩阵计算 ===
    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMMATRIX GetViewProjectionMatrix() const;

    // === 辅助函数 ===
    // 设置相机看向目标点（LookAt 模式）
    void SetLookAt(
        const DirectX::XMFLOAT3& eye,
        const DirectX::XMFLOAT3& target,
        const DirectX::XMFLOAT3& up = {0, 1, 0}
    );

    // 获取相机方向向量
    DirectX::XMFLOAT3 GetForward() const;
    DirectX::XMFLOAT3 GetRight() const;
    DirectX::XMFLOAT3 GetUp() const;

    // === 移动和旋转（便于手动调用） ===
    void MoveForward(float distance);
    void MoveRight(float distance);
    void MoveUp(float distance);
    void Rotate(float deltaYaw, float deltaPitch);

private:
    // 内部辅助函数：根据 yaw/pitch 计算前向向量
    static DirectX::XMVECTOR CalculateForwardVector(float yaw, float pitch);
};
// ✅ EditorCamera 已被 CCamera 取代（现在由 CScene 管理）
