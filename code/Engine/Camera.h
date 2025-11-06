#pragma once
#include <DirectXMath.h>

struct EditorCamera { float aspect = 16.f/9.f; };
class Camera
{
public:
    Camera() = default;

    void SetLookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);
    void MoveForward(float d);
    void MoveRight(float d);
    void MoveUp(float d);
    void RotateYawPitch(float dy, float dp);
    DirectX::XMMATRIX GetView() const;

private:
    DirectX::XMFLOAT3 m_pos{ 0,0,-5 };
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
};
