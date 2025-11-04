
#pragma once
#include <DirectXMath.h>

class Camera {
public:
    void SetLookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);
    void MoveForward(float d);
    void MoveRight(float d);
    void MoveUp(float d);
    void RotateYawPitch(float dyaw, float dpitch);
    DirectX::XMMATRIX GetView() const;
    DirectX::XMFLOAT3 Position() const { return m_pos; }
private:
    DirectX::XMFLOAT3 m_pos{ -6.0f, 0.8f, 0.0f };
    float m_yaw{0.0f}, m_pitch{-0.1f};
};
