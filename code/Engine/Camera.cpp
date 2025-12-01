#include "Camera.h"
#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

// ============================================
// 构造函数
// ============================================
CCamera::CCamera() {
    // 从默认 yaw/pitch 初始化 quaternion
    UpdateQuaternionFromYawPitch();
}

// ============================================
// 内部辅助函数：yaw/pitch → quaternion
// ============================================
void CCamera::UpdateQuaternionFromYawPitch() {
    // 从 yaw/pitch 构建 Quaternion（roll = 0）
    // 左手坐标系：Yaw (Y 轴), Pitch (X 轴), Roll (Z 轴)
    XMVECTOR quat = XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f);
    XMStoreFloat4(&m_rotation, quat);
}

// ============================================
// 内部辅助函数：quaternion → yaw/pitch（近似）
// ============================================
void CCamera::UpdateYawPitchFromQuaternion() {
    // 从 Quaternion 提取 yaw/pitch（用于编辑器显示）
    XMVECTOR quat = XMLoadFloat4(&m_rotation);

    // 提取 forward 向量
    XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), quat);
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, forward);

    // 反推 yaw/pitch
    pitch = asinf(fwd.y);
    yaw = atan2f(fwd.z, fwd.x);
}

// ============================================
// 矩阵计算
// ============================================
XMMATRIX CCamera::GetViewMatrix() const {
    XMVECTOR pos = XMLoadFloat3(&position);

    if (m_useQuaternion) {
        // 使用 Quaternion（精确，无万向锁）
        XMVECTOR quat = XMLoadFloat4(&m_rotation);
        XMMATRIX rotMatrix = XMMatrixRotationQuaternion(quat);
        XMMATRIX transMatrix = XMMatrixTranslationFromVector(pos);

        // View Matrix = Inverse(Rotation * Translation)
        return XMMatrixInverse(nullptr, rotMatrix * transMatrix);
    } else {
        // 使用 yaw/pitch（兼容旧代码，但有万向锁）
        float cy = cosf(yaw);
        float sy = sinf(yaw);
        float cp = cosf(pitch);
        float sp = sinf(pitch);

        XMVECTOR forward = XMVector3Normalize(XMVectorSet(cy * cp, sp, sy * cp, 0));
        XMVECTOR up = XMVectorSet(0, 1, 0, 0);

        return XMMatrixLookToLH(pos, forward, up);
    }
}

XMMATRIX CCamera::GetProjectionMatrix() const {
    return XMMatrixPerspectiveFovLH(fovY, aspectRatio, nearZ, farZ);
}

XMMATRIX CCamera::GetViewProjectionMatrix() const {
    return GetViewMatrix() * GetProjectionMatrix();
}

// ============================================
// SetLookAt - 设置相机看向目标点（完美支持任意 up 向量）
// ============================================
void CCamera::SetLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up) {
    position = eye;

    // 构建 View Matrix
    XMMATRIX viewMatrix = XMMatrixLookAtLH(
        XMLoadFloat3(&eye),
        XMLoadFloat3(&target),
        XMLoadFloat3(&up)
    );

    // 提取 Rotation（View Matrix 的逆）
    XMMATRIX worldMatrix = XMMatrixInverse(nullptr, viewMatrix);
    XMVECTOR scale, rotQuat, trans;
    XMMatrixDecompose(&scale, &rotQuat, &trans, worldMatrix);

    // 存储 Quaternion
    XMStoreFloat4(&m_rotation, rotQuat);
    m_useQuaternion = true;

    // 同时更新 yaw/pitch（用于编辑器显示，但不精确）
    UpdateYawPitchFromQuaternion();
}

// ============================================
// SetYawPitch - 设置 yaw/pitch（编辑器相机控制）
// ============================================
void CCamera::SetYawPitch(float newYaw, float newPitch) {
    yaw = newYaw;
    pitch = newPitch;

    // 更新内部 Quaternion
    UpdateQuaternionFromYawPitch();
    m_useQuaternion = false;  // 使用 yaw/pitch 模式（编辑器相机）
}

// ============================================
// 获取相机方向向量（从 Quaternion 提取）
// ============================================
XMFLOAT3 CCamera::GetForward() const {
    XMVECTOR quat = XMLoadFloat4(&m_rotation);
    XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), quat);  // +Z = Forward

    XMFLOAT3 result;
    XMStoreFloat3(&result, forward);
    return result;
}

XMFLOAT3 CCamera::GetRight() const {
    XMVECTOR quat = XMLoadFloat4(&m_rotation);
    XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), quat);  // +X = Right

    XMFLOAT3 result;
    XMStoreFloat3(&result, right);
    return result;
}

XMFLOAT3 CCamera::GetUp() const {
    XMVECTOR quat = XMLoadFloat4(&m_rotation);
    XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), quat);  // +Y = Up

    XMFLOAT3 result;
    XMStoreFloat3(&result, up);
    return result;
}

// ============================================
// 移动和旋转
// ============================================
void CCamera::MoveForward(float distance) {
    XMFLOAT3 forward = GetForward();
    position.x += forward.x * distance;
    position.y += forward.y * distance;
    position.z += forward.z * distance;
}

void CCamera::MoveRight(float distance) {
    XMFLOAT3 right = GetRight();
    position.x += right.x * distance;
    position.y += right.y * distance;
    position.z += right.z * distance;
}

void CCamera::MoveUp(float distance) {
    position.y += distance;  // 世界空间 Y 轴（不受 pitch 影响）
}

void CCamera::Rotate(float deltaYaw, float deltaPitch) {
    const float pitchLimit = 1.5533f;  // ~89° (避免视觉上的万向锁)

    yaw += deltaYaw;
    pitch += deltaPitch;

    // 限制 pitch 范围（避免翻转）
    if (pitch > pitchLimit) pitch = pitchLimit;
    if (pitch < -pitchLimit) pitch = -pitchLimit;

    // 更新内部 Quaternion
    UpdateQuaternionFromYawPitch();
    m_useQuaternion = false;  // 编辑器相机模式
}
