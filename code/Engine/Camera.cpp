#include "Camera.h"
#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

// ============================================
// 内部辅助函数：根据 yaw/pitch 计算前向向量
// ============================================
XMVECTOR CCamera::CalculateForwardVector(float yaw, float pitch) {
    float cy = cosf(yaw);
    float sy = sinf(yaw);
    float cp = cosf(pitch);
    float sp = sinf(pitch);

    // 左手坐标系：+X=Right, +Y=Up, +Z=Forward
    return XMVector3Normalize(XMVectorSet(cy * cp, sp, sy * cp, 0));
}

// ============================================
// 矩阵计算
// ============================================
XMMATRIX CCamera::GetViewMatrix() const {
    XMVECTOR forward = CalculateForwardVector(yaw, pitch);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    return XMMatrixLookToLH(XMLoadFloat3(&position), forward, up);
}

XMMATRIX CCamera::GetProjectionMatrix() const {
    return XMMatrixPerspectiveFovLH(fovY, aspectRatio, nearZ, farZ);
}

XMMATRIX CCamera::GetViewProjectionMatrix() const {
    return GetViewMatrix() * GetProjectionMatrix();
}

// ============================================
// SetLookAt - 设置相机看向目标点
// ============================================
void CCamera::SetLookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up) {
    position = eye;

    // 计算方向向量
    XMVECTOR direction = XMVector3Normalize(
        XMLoadFloat3(&target) - XMLoadFloat3(&eye)
    );

    // 从方向向量反推 yaw 和 pitch
    XMFLOAT3 dir;
    XMStoreFloat3(&dir, direction);

    pitch = asinf(dir.y);
    yaw = atan2f(dir.z, dir.x);
}

// ============================================
// 获取相机方向向量
// ============================================
XMFLOAT3 CCamera::GetForward() const {
    XMFLOAT3 forward;
    XMStoreFloat3(&forward, CalculateForwardVector(yaw, pitch));
    return forward;
}

XMFLOAT3 CCamera::GetRight() const {
    XMVECTOR forward = CalculateForwardVector(yaw, pitch);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

    XMFLOAT3 rightVec;
    XMStoreFloat3(&rightVec, right);
    return rightVec;
}

XMFLOAT3 CCamera::GetUp() const {
    XMVECTOR forward = CalculateForwardVector(yaw, pitch);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMVECTOR right = XMVector3Cross(up, forward);
    XMVECTOR cameraUp = XMVector3Normalize(XMVector3Cross(forward, right));

    XMFLOAT3 upVec;
    XMStoreFloat3(&upVec, cameraUp);
    return upVec;
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
    const float pitchLimit = 1.5533f;  // ~89° (避免万向锁)

    yaw += deltaYaw;
    pitch += deltaPitch;

    // 限制 pitch 范围
    if (pitch > pitchLimit) pitch = pitchLimit;
    if (pitch < -pitchLimit) pitch = -pitchLimit;
}
