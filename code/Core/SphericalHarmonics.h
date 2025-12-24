#pragma once
#include <DirectXMath.h>
#include <array>
#include <vector>
#include <string>

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
    constexpr int L1_COEFF_COUNT = 4;   // L0(1) + L1(3) = 4
    constexpr int L2_COEFF_COUNT = 9;   // L0(1) + L1(3) + L2(5) = 9
    constexpr int L3_COEFF_COUNT = 16;  // L0(1) + L1(3) + L2(5) + L3(7) = 16
    constexpr int L4_COEFF_COUNT = 25;  // L0(1) + L1(3) + L2(5) + L3(7) + L4(9) = 25

    // ============================================
    // SH Basis Functions (L2 / L4)
    // ============================================
    // 计算给定方向的 L2 球谐基函数值
    // 输入：dir - 归一化方向向量
    // 输出：basis - 9 个基函数的值（标量）
    void EvaluateBasis(const DirectX::XMFLOAT3& dir, std::array<float, 9>& basis);

    // 计算给定方向的 L1 球谐基函数值（4 个系数）
    void EvaluateBasisL1(const DirectX::XMFLOAT3& dir, std::array<float, 4>& basis);

    // 计算给定方向的 L3 球谐基函数值（16 个系数）
    void EvaluateBasisL3(const DirectX::XMFLOAT3& dir, std::array<float, 16>& basis);

    // 计算给定方向的 L4 球谐基函数值（25 个系数）
    void EvaluateBasisL4(const DirectX::XMFLOAT3& dir, std::array<float, 25>& basis);

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
    // outCoeffs: 输出的 L2 球谐系数（RGB，9 个）
    void ProjectCubemapToSH(
        const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<DirectX::XMFLOAT3, 9>& outCoeffs
    );

    // 从平坦数组格式的 Cubemap 投影到 L2 球谐系数
    // flatCubemapData: 连续存储的 6 面像素数据 [face * pixelsPerFace + y * size + x]
    // size: cubemap 分辨率
    // outCoeffs: 输出的 L2 球谐系数（RGB，9 个）
    void ProjectCubemapToSH(
        const DirectX::XMFLOAT4* flatCubemapData,
        int size,
        std::array<DirectX::XMFLOAT3, 9>& outCoeffs
    );

    // ============================================
    // SH Evaluation (Reconstruction)
    // ============================================

    // 从 L2 球谐系数重建给定方向的辐照度
    // coeffs: L2 球谐系数（RGB，9 个）
    // dir: 归一化方向向量
    // 返回：重建的 RGB 辐照度
    DirectX::XMFLOAT3 EvaluateSH(
        const std::array<DirectX::XMFLOAT3, 9>& coeffs,
        const DirectX::XMFLOAT3& dir
    );

    // ============================================
    // SH Reconstruction to Cubemap (Debug)
    // ============================================

    // 从 L2 球谐系数重建 Cubemap
    // coeffs: L2 球谐系数（RGB，9 个）
    // size: 输出 cubemap 分辨率
    // outCubemapData: 输出的 6 个面的像素数据（RGBA float）
    void ProjectSHToCubemap(
        const std::array<DirectX::XMFLOAT3, 9>& coeffs,
        int size,
        std::array<std::vector<DirectX::XMFLOAT4>, 6>& outCubemapData
    );

    // Debug: 将 SH 系数导出为 PNG cubemap 图片
    // coeffs: L2 球谐系数（RGB，9 个）
    // size: 输出 cubemap 分辨率
    // outputDir: 输出目录路径
    // prefix: 文件名前缀（如 "sh_reconstructed"）
    void DebugExportSHAsCubemap(
        const std::array<DirectX::XMFLOAT3, 9>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix = "sh_reconstructed"
    );

    // ============================================
    // L1 SH Projection (Debug/Comparison)
    // ============================================

    // 从 Cubemap 投影到 L1 球谐系数（4 个系数，用于对比）
    void ProjectCubemapToSH_L1(
        const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<DirectX::XMFLOAT3, 4>& outCoeffs
    );

    // 从 L1 球谐系数重建给定方向的辐照度
    DirectX::XMFLOAT3 EvaluateSH_L1(
        const std::array<DirectX::XMFLOAT3, 4>& coeffs,
        const DirectX::XMFLOAT3& dir
    );

    // 从 L1 球谐系数重建 Cubemap
    void ProjectSHToCubemap_L1(
        const std::array<DirectX::XMFLOAT3, 4>& coeffs,
        int size,
        std::array<std::vector<DirectX::XMFLOAT4>, 6>& outCubemapData
    );

    // Debug: 将 L1 SH 系数导出为 KTX2 cubemap
    void DebugExportSHAsCubemap_L1(
        const std::array<DirectX::XMFLOAT3, 4>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix = "sh_reconstructed_L1"
    );

    // ============================================
    // L3 SH Projection (Debug/Comparison)
    // ============================================

    // 从 Cubemap 投影到 L3 球谐系数（16 个系数，用于对比）
    void ProjectCubemapToSH_L3(
        const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<DirectX::XMFLOAT3, 16>& outCoeffs
    );

    // 从 L3 球谐系数重建给定方向的辐照度
    DirectX::XMFLOAT3 EvaluateSH_L3(
        const std::array<DirectX::XMFLOAT3, 16>& coeffs,
        const DirectX::XMFLOAT3& dir
    );

    // 从 L3 球谐系数重建 Cubemap
    void ProjectSHToCubemap_L3(
        const std::array<DirectX::XMFLOAT3, 16>& coeffs,
        int size,
        std::array<std::vector<DirectX::XMFLOAT4>, 6>& outCubemapData
    );

    // Debug: 将 L3 SH 系数导出为 KTX2 cubemap
    void DebugExportSHAsCubemap_L3(
        const std::array<DirectX::XMFLOAT3, 16>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix = "sh_reconstructed_L3"
    );

    // ============================================
    // L4 SH Projection (Debug/Comparison)
    // ============================================

    // 从 Cubemap 投影到 L4 球谐系数（25 个系数，用于对比）
    void ProjectCubemapToSH_L4(
        const std::array<std::vector<DirectX::XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<DirectX::XMFLOAT3, 25>& outCoeffs
    );

    // 从 L4 球谐系数重建给定方向的辐照度
    DirectX::XMFLOAT3 EvaluateSH_L4(
        const std::array<DirectX::XMFLOAT3, 25>& coeffs,
        const DirectX::XMFLOAT3& dir
    );

    // 从 L4 球谐系数重建 Cubemap
    void ProjectSHToCubemap_L4(
        const std::array<DirectX::XMFLOAT3, 25>& coeffs,
        int size,
        std::array<std::vector<DirectX::XMFLOAT4>, 6>& outCubemapData
    );

    // Debug: 将 L4 SH 系数导出为 KTX2 cubemap
    void DebugExportSHAsCubemap_L4(
        const std::array<DirectX::XMFLOAT3, 25>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix = "sh_reconstructed_L4"
    );

} // namespace SphericalHarmonics
