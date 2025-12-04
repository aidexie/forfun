#include "SphericalHarmonics.h"
#include <cmath>

using namespace DirectX;

namespace SphericalHarmonics
{
    // ============================================
    // SH Basis Functions (L2)
    // ============================================
    // 使用直角坐标形式（更适合实现）
    // 参考：Sloan, "Stupid Spherical Harmonics Tricks"

    void EvaluateBasis(const XMFLOAT3& dir, float basis[9])
    {
        float x = dir.x;
        float y = dir.y;
        float z = dir.z;

        // L0 (band 0, 1 个系数)
        basis[0] = 0.282095f;  // 1 / (2 * sqrt(π))

        // L1 (band 1, 3 个系数)
        basis[1] = 0.488603f * y;  // sqrt(3 / (4π)) * y
        basis[2] = 0.488603f * z;  // sqrt(3 / (4π)) * z
        basis[3] = 0.488603f * x;  // sqrt(3 / (4π)) * x

        // L2 (band 2, 5 个系数)
        basis[4] = 1.092548f * x * y;              // sqrt(15 / (4π)) * x * y
        basis[5] = 1.092548f * y * z;              // sqrt(15 / (4π)) * y * z
        basis[6] = 0.315392f * (3.0f * z * z - 1.0f);  // sqrt(5 / (16π)) * (3z² - 1)
        basis[7] = 1.092548f * x * z;              // sqrt(15 / (4π)) * x * z
        basis[8] = 0.546274f * (x * x - y * y);    // sqrt(15 / (16π)) * (x² - y²)
    }

    // ============================================
    // Cubemap Utilities
    // ============================================

    XMFLOAT3 CubemapTexelToDirection(int face, int x, int y, int size)
    {
        // 将 texel 坐标转换为 UV [-1, 1]
        float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
        float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;

        XMFLOAT3 dir;

        // 根据 face 计算方向向量
        // DirectX 左手坐标系：+X=Right, +Y=Up, +Z=Forward
        switch (face)
        {
            case 0: // +X (Right)
                dir = XMFLOAT3(1.0f, -v, -u);
                break;
            case 1: // -X (Left)
                dir = XMFLOAT3(-1.0f, -v, u);
                break;
            case 2: // +Y (Up)
                dir = XMFLOAT3(u, 1.0f, v);
                break;
            case 3: // -Y (Down)
                dir = XMFLOAT3(u, -1.0f, -v);
                break;
            case 4: // +Z (Forward)
                dir = XMFLOAT3(u, -v, 1.0f);
                break;
            case 5: // -Z (Back)
                dir = XMFLOAT3(-u, -v, -1.0f);
                break;
            default:
                dir = XMFLOAT3(0, 0, 1);
                break;
        }

        // 归一化
        XMVECTOR vec = XMLoadFloat3(&dir);
        vec = XMVector3Normalize(vec);
        XMStoreFloat3(&dir, vec);

        return dir;
    }

    float ComputeSolidAngleWeight(float u, float v)
    {
        // 立体角权重公式（从 Cubemap 参数化推导）
        // dω = du · dv / (1 + u² + v²)^(3/2)
        float temp = 1.0f + u * u + v * v;
        return 4.0f / (temp * std::sqrt(temp));
    }

    // ============================================
    // SH Projection
    // ============================================

    void ProjectCubemapToSH(
        const XMFLOAT4* cubemapData[6],
        int size,
        XMFLOAT3 outCoeffs[9])
    {
        // 初始化系数为 0
        for (int i = 0; i < 9; i++)
        {
            outCoeffs[i] = XMFLOAT3(0, 0, 0);
        }

        float totalWeight = 0.0f;

        // 遍历 6 个面
        for (int face = 0; face < 6; face++)
        {
            const XMFLOAT4* faceData = cubemapData[face];

            // 遍历该面的所有像素
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    // 1. 计算 UV 和立体角权重
                    float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    float weight = ComputeSolidAngleWeight(u, v);

                    // 2. 计算方向向量
                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);

                    // 3. 采样颜色
                    int pixelIndex = y * size + x;
                    XMFLOAT3 color(
                        faceData[pixelIndex].x,
                        faceData[pixelIndex].y,
                        faceData[pixelIndex].z
                    );

                    // 4. 计算 SH 基函数
                    float basis[9];
                    EvaluateBasis(dir, basis);

                    // 5. 累加到 SH 系数（Riemann 求和）
                    for (int i = 0; i < 9; i++)
                    {
                        outCoeffs[i].x += color.x * basis[i] * weight;
                        outCoeffs[i].y += color.y * basis[i] * weight;
                        outCoeffs[i].z += color.z * basis[i] * weight;
                    }

                    totalWeight += weight;
                }
            }
        }

        // 6. 归一化
        if (totalWeight > 0.0f)
        {
            for (int i = 0; i < 9; i++)
            {
                outCoeffs[i].x /= totalWeight;
                outCoeffs[i].y /= totalWeight;
                outCoeffs[i].z /= totalWeight;
            }
        }
    }

    // ============================================
    // SH Evaluation (Reconstruction)
    // ============================================

    XMFLOAT3 EvaluateSH(
        const XMFLOAT3 coeffs[9],
        const XMFLOAT3& dir)
    {
        // 计算 SH 基函数
        float basis[9];
        EvaluateBasis(dir, basis);

        // 重建辐照度
        XMVECTOR result = XMVectorZero();

        for (int i = 0; i < 9; i++)
        {
            XMVECTOR coeff = XMLoadFloat3(&coeffs[i]);
            result = XMVectorAdd(result, XMVectorScale(coeff, basis[i]));
        }

        // Clamp 负值（SH ringing artifact）
        result = XMVectorMax(result, XMVectorZero());

        XMFLOAT3 output;
        XMStoreFloat3(&output, result);
        return output;
    }

} // namespace SphericalHarmonics
