#pragma once
#include <DirectXMath.h>

// ============================================
// SphericalHarmonics - L2 球谐函数工具类
// ============================================
// 提供 L2 球谐函数的计算和投影功能
//
// 参考：
// - "Stupid Spherical Harmonics Tricks" (Peter-Pike Sloan, GDC 2008)
// - "An Efficient Representation for Irradiance Environment Maps" (Ramamoorthi & Hanrahan, SIGGRAPH 2001)
// ============================================

namespace SphericalHarmonics
{
    // ============================================
    // Constants
    // ============================================
    constexpr int L2_COEFF_COUNT = 9;  // L0(1) + L1(3) + L2(5) = 9

    // ============================================
    // SH Basis Functions (L2)
    // ============================================
    // 计算给定方向的 L2 球谐基函数值
    // 输入：dir - 归一化方向向量
    // 输出：basis[9] - 9 个基函数的值（标量）
    void EvaluateBasis(const DirectX::XMFLOAT3& dir, float basis[9]);

    // ============================================
    // Cubemap Utilities
    // ============================================

    // 将 Cubemap 的 texel 坐标转换为方向向量
    // face: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
    // x, y: texel 坐标 [0, size-1]
    // size: cubemap 分辨率
    DirectX::XMFLOAT3 CubemapTexelToDirection(int face, int x, int y, int size);

    // 计算 Cubemap texel 对应的立体角权重
    // u, v: 归一化 UV 坐标 [-1, 1]
    // 返回：立体角权重（用于积分）
    float ComputeSolidAngleWeight(float u, float v);

    // ============================================
    // SH Projection
    // ============================================

    // 从 Cubemap 投影到 L2 球谐系数
    // cubemapData: 6 个面的像素数据（RGBA float，行优先）
    // size: cubemap 分辨率（假设正方形）
    // outCoeffs[9]: 输出的 L2 球谐系数（RGB）
    void ProjectCubemapToSH(
        const DirectX::XMFLOAT4* cubemapData[6],
        int size,
        DirectX::XMFLOAT3 outCoeffs[9]
    );

    // ============================================
    // SH Evaluation (Reconstruction)
    // ============================================

    // 从 L2 球谐系数重建给定方向的辐照度
    // coeffs[9]: L2 球谐系数（RGB）
    // dir: 归一化方向向量
    // 返回：重建的 RGB 辐照度
    DirectX::XMFLOAT3 EvaluateSH(
        const DirectX::XMFLOAT3 coeffs[9],
        const DirectX::XMFLOAT3& dir
    );

} // namespace SphericalHarmonics
