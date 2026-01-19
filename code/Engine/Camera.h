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

    // ============================================
    // TAA Jitter Support
    // ============================================

    // Get projection matrix with sub-pixel jitter applied (for TAA)
    DirectX::XMMATRIX GetJitteredProjectionMatrix(uint32_t screenWidth, uint32_t screenHeight) const;

    // Get projection matrix with custom jitter offset (for FSR2 or external jitter)
    // jitterNDC: Jitter offset in NDC space (-1 to 1 range)
    DirectX::XMMATRIX GetJitteredProjectionMatrix(const DirectX::XMFLOAT2& jitterNDC) const;

    // Get current jitter offset in pixels (centered at 0)
    DirectX::XMFLOAT2 GetJitterOffset() const;

    // Advance to next jitter sample (call once per frame)
    void AdvanceJitter();

    // Enable/disable TAA jitter
    void SetTAAEnabled(bool enabled);
    bool IsTAAEnabled() const { return m_taaEnabled; }

    // Set number of jitter samples (4, 8, or 16)
    void SetJitterSampleCount(uint32_t count);
    uint32_t GetJitterSampleCount() const { return m_jitterSampleCount; }

    // Get current frame index (for shader)
    uint32_t GetJitterFrameIndex() const { return m_jitterFrameIndex; }

private:
    // ============================================
    // 内部表示：只用 Quaternion
    // ============================================
    DirectX::XMFLOAT4 m_rotation{0, 0, 0, 1};  // identity quaternion

    // 缓存的 yaw/pitch（用于增量旋转 Rotate()）
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;

    // ============================================
    // TAA Jitter State
    // ============================================
    bool m_taaEnabled = false;
    uint32_t m_jitterFrameIndex = 0;
    uint32_t m_jitterSampleCount = 8;  // Default: 8 samples (Halton 2,3)

    // Halton sequence generator
    static float HaltonSequence(uint32_t index, uint32_t base);
};
