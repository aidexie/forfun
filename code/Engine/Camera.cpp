#include "Camera.h"
#include "Core/RenderConfig.h"
#include <cmath>

using namespace DirectX;

// ============================================
// Reversed-Z Projection Matrix (Left-Hand, DirectX)
// ============================================
// Maps: nearZ -> 1.0, farZ -> 0.0 (opposite of standard projection)
// This provides better depth precision for distant objects.
static XMMATRIX XMMatrixPerspectiveFovLH_ReversedZ(float fovY, float aspect, float nearZ, float farZ) {
    float h = 1.0f / tanf(fovY * 0.5f);
    float w = h / aspect;

    // Standard projection Z component: z' = (z*far) / (z*(far-near)) - near*far / (z*(far-near))
    // After perspective divide: z_ndc = (far*(z-near)) / (z*(far-near))
    // At z=near: z_ndc = 0, at z=far: z_ndc = 1
    //
    // Reversed-Z: swap 0 and 1
    // z_ndc = near*(far-z) / (z*(far-near))
    // At z=near: z_ndc = 1, at z=far: z_ndc = 0
    return XMMatrixSet(
        w,    0.0f, 0.0f,                               0.0f,
        0.0f, h,    0.0f,                               0.0f,
        0.0f, 0.0f, nearZ / (nearZ - farZ),             1.0f,
        0.0f, 0.0f, -farZ * nearZ / (nearZ - farZ),     0.0f
    );
}

// ============================================
// 辅助函数：从 yaw/pitch 构建 Quaternion
// ============================================
// 左手坐标系：yaw=0 看向 +Z，yaw 绕 Y 轴旋转
static XMVECTOR QuaternionFromYawPitch(float yaw, float pitch) {
    return XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f);
}

// ============================================
// 辅助函数：从 Quaternion 提取 yaw/pitch
// ============================================
static void YawPitchFromQuaternion(XMVECTOR quat, float& outYaw, float& outPitch) {
    // 提取 forward 向量
    XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), quat);
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, forward);

    // 反推 yaw/pitch
    // 左手坐标系：yaw=0 看向 +Z
    outPitch = asinf(fwd.y);
    outYaw = atan2f(fwd.x, fwd.z);
}

// ============================================
// 构造函数
// ============================================
CCamera::CCamera() {
    // 默认：yaw=0, pitch=0 → 看向 +Z
    XMStoreFloat4(&m_rotation, QuaternionFromYawPitch(0.0f, 0.0f));
}

// ============================================
// 矩阵计算（统一使用 Quaternion）
// ============================================
XMMATRIX CCamera::GetViewMatrix() const {
    XMVECTOR pos = XMLoadFloat3(&position);
    XMVECTOR quat = XMLoadFloat4(&m_rotation);

    // World Matrix = Rotation * Translation
    XMMATRIX rotMatrix = XMMatrixRotationQuaternion(quat);
    XMMATRIX transMatrix = XMMatrixTranslationFromVector(pos);

    // View Matrix = Inverse(World Matrix)
    return XMMatrixInverse(nullptr, rotMatrix * transMatrix);
}

XMMATRIX CCamera::GetProjectionMatrix() const {
    if (UseReversedZ()) {
        return XMMatrixPerspectiveFovLH_ReversedZ(fovY, aspectRatio, nearZ, farZ);
    }
    return XMMatrixPerspectiveFovLH(fovY, aspectRatio, nearZ, farZ);
}

XMMATRIX CCamera::GetViewProjectionMatrix() const {
    return GetViewMatrix() * GetProjectionMatrix();
}

// ============================================
// 外部接口：设置朝向
// ============================================
void CCamera::SetLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up) {
    position = eye;

    // 构建 View Matrix
    XMMATRIX viewMatrix = XMMatrixLookAtLH(
        XMLoadFloat3(&eye),
        XMLoadFloat3(&target),
        XMLoadFloat3(&up)
    );

    // 提取 Rotation（View Matrix 的逆 = World Matrix）
    XMMATRIX worldMatrix = XMMatrixInverse(nullptr, viewMatrix);
    XMVECTOR scale, rotQuat, trans;
    XMMatrixDecompose(&scale, &rotQuat, &trans, worldMatrix);

    // 存储 Quaternion
    XMStoreFloat4(&m_rotation, rotQuat);

    // 更新缓存的 yaw/pitch
    YawPitchFromQuaternion(rotQuat, m_yaw, m_pitch);
}

void CCamera::SetYawPitch(float yaw, float pitch) {
    m_yaw = yaw;
    m_pitch = pitch;

    // 更新 Quaternion
    XMStoreFloat4(&m_rotation, QuaternionFromYawPitch(yaw, pitch));
}

void CCamera::Rotate(float deltaYaw, float deltaPitch) {
    const float pitchLimit = 1.5533f;  // ~89°

    m_yaw += deltaYaw;
    m_pitch += deltaPitch;

    // 限制 pitch 范围
    if (m_pitch > pitchLimit) m_pitch = pitchLimit;
    if (m_pitch < -pitchLimit) m_pitch = -pitchLimit;

    // 更新 Quaternion
    XMStoreFloat4(&m_rotation, QuaternionFromYawPitch(m_yaw, m_pitch));
}

// ============================================
// 获取方向向量
// ============================================
XMFLOAT3 CCamera::GetForward() const {
    XMVECTOR quat = XMLoadFloat4(&m_rotation);
    XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), quat);  // +Z

    XMFLOAT3 result;
    XMStoreFloat3(&result, forward);
    return result;
}

XMFLOAT3 CCamera::GetRight() const {
    XMVECTOR quat = XMLoadFloat4(&m_rotation);
    XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), quat);  // +X

    XMFLOAT3 result;
    XMStoreFloat3(&result, right);
    return result;
}

XMFLOAT3 CCamera::GetUp() const {
    XMVECTOR quat = XMLoadFloat4(&m_rotation);
    XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), quat);  // +Y

    XMFLOAT3 result;
    XMStoreFloat3(&result, up);
    return result;
}

// ============================================
// 获取 Euler angles（从 Quaternion 反算）
// ============================================
float CCamera::GetYaw() const {
    return m_yaw;
}

float CCamera::GetPitch() const {
    return m_pitch;
}

// ============================================
// 移动
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
    position.y += distance;  // 世界空间 Y 轴
}
