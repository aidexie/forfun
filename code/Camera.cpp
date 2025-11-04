
#include "Camera.h"
using namespace DirectX;
static XMVECTOR Dir(float yaw,float pitch){
    float cy=cosf(yaw), sy=sinf(yaw); float cp=cosf(pitch), sp=sinf(pitch);
    return XMVector3Normalize(XMVectorSet(cy*cp, sp, sy*cp, 0));
}
void Camera::SetLookAt(const XMFLOAT3& eye,const XMFLOAT3& target){
    m_pos=eye; XMVECTOR d=XMVector3Normalize(XMLoadFloat3(&target)-XMLoadFloat3(&eye));
    m_pitch=asinf(d.m128_f32[1]); m_yaw=atan2f(d.m128_f32[2], d.m128_f32[0]);
}
void Camera::MoveForward(float d){ XMFLOAT3 t; XMStoreFloat3(&t,Dir(m_yaw,m_pitch)); m_pos.x+=t.x*d; m_pos.y+=t.y*d; m_pos.z+=t.z*d; }
void Camera::MoveRight(float d){ XMVECTOR r=DirectX::XMVector3Normalize(DirectX::XMVector3Cross(DirectX::XMVectorSet(0,1,0,0),Dir(m_yaw,m_pitch))); DirectX::XMFLOAT3 t; DirectX::XMStoreFloat3(&t,r); m_pos.x+=t.x*d; m_pos.y+=t.y*d; m_pos.z+=t.z*d; }
void Camera::MoveUp(float d){ m_pos.y+=d; }
void Camera::RotateYawPitch(float dy,float dp){ const float lim=1.5533f; m_yaw+=dy; m_pitch+=dp; if(m_pitch>lim)m_pitch=lim; if(m_pitch<-lim)m_pitch=-lim; }
DirectX::XMMATRIX Camera::GetView() const{ return DirectX::XMMatrixLookToLH(DirectX::XMLoadFloat3(&m_pos), Dir(m_yaw,m_pitch), DirectX::XMVectorSet(0,1,0,0)); }
