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

// ============================================
// TAA Jitter Support
// ============================================

// Halton sequence for low-discrepancy sampling
// Returns values in [0, 1)
float CCamera::HaltonSequence(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float f = 1.0f;
    uint32_t i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
        i /= base;
    }
    return result;
}

XMFLOAT2 CCamera::GetJitterOffset() const {
    if (!m_taaEnabled) {
        return XMFLOAT2(0.0f, 0.0f);
    }

    // Use 1-indexed for better distribution (avoid 0,0 at start)
    uint32_t index = (m_jitterFrameIndex % m_jitterSampleCount) + 1;

    // Halton(2,3) sequence, centered at 0 (range: -0.5 to 0.5)
    return XMFLOAT2(
        HaltonSequence(index, 2) - 0.5f,
        HaltonSequence(index, 3) - 0.5f
    );
}

XMMATRIX CCamera::GetJitteredProjectionMatrix(uint32_t screenWidth, uint32_t screenHeight) const {
    XMMATRIX proj = GetProjectionMatrix();

    if (!m_taaEnabled || screenWidth == 0 || screenHeight == 0) {
        return proj;
    }

    XMFLOAT2 jitter = GetJitterOffset();

    // Convert pixel offset to NDC offset
    // NDC range is [-1, 1], so 2.0 / width gives the size of one pixel in NDC
    float jitterX = (2.0f * jitter.x) / static_cast<float>(screenWidth);
    float jitterY = (2.0f * jitter.y) / static_cast<float>(screenHeight);

    // Apply jitter to projection matrix
    // For row-major matrix, modify elements [2][0] and [2][1]
    // This shifts the entire frustum by sub-pixel amount
    XMFLOAT4X4 projF;
    XMStoreFloat4x4(&projF, proj);
    projF._31 += jitterX;
    projF._32 += jitterY;

    return XMLoadFloat4x4(&projF);
}

XMMATRIX CCamera::GetJitteredProjectionMatrix(const XMFLOAT2& jitterNDC) const {
    XMMATRIX proj = GetProjectionMatrix();

    // Apply jitter to projection matrix
    // For row-major matrix, modify elements [2][0] and [2][1]
    // This shifts the entire frustum by sub-pixel amount
    XMFLOAT4X4 projF;
    XMStoreFloat4x4(&projF, proj);
    projF._31 += jitterNDC.x;
    projF._32 += jitterNDC.y;

    return XMLoadFloat4x4(&projF);
}

void CCamera::AdvanceJitter() {
    m_jitterFrameIndex++;
}

void CCamera::SetTAAEnabled(bool enabled) {
    m_taaEnabled = enabled;
    if (!enabled) {
        m_jitterFrameIndex = 0;
    }
}

void CCamera::SetJitterSampleCount(uint32_t count) {
    // Only allow 4, 8, or 16 samples
    if (count <= 4) {
        m_jitterSampleCount = 4;
    } else if (count <= 8) {
        m_jitterSampleCount = 8;
    } else {
        m_jitterSampleCount = 16;
    }
}
