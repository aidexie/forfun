#include "SphericalHarmonics.h"
#include <cmath>
#include <filesystem>
#include <algorithm>

#include "Exporter/KTXExporter.h"

using namespace DirectX;

namespace SphericalHarmonics
{
    // ============================================
    // SH Basis Functions (L2)
    // ============================================
    // 使用直角坐标形式（更适合实现）
    // 参考：Sloan, "Stupid Spherical Harmonics Tricks"

    void EvaluateBasis(const XMFLOAT3& dir, std::array<float, 9>& basis)
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

    XMFLOAT3  CubemapTexelToDirection(int face, int x, int y, int size)
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

    float ComputeSolidAngle(float u, float v, int size)
    {
        // 立体角公式: dω = (du * dv) / (1 + u² + v²)^(3/2)
        // u, v ∈ [-1, 1]，所以 du = dv = 2/size
        float temp = 1.0f + u * u + v * v;
        float dOmega_per_dudv = 1.0f / (temp * std::sqrt(temp));

        // 乘以实际的 du*dv
        float texelSize = 2.0f / size;
        return dOmega_per_dudv * texelSize * texelSize;
    }

    // ============================================
    // SH Projection
    // ============================================

    void ProjectCubemapToSH(
        const std::array<std::vector<XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<XMFLOAT3, 9>& outCoeffs)
    {
        // 初始化系数为 0
        for (int i = 0; i < 9; i++)
        {
            outCoeffs[i] = XMFLOAT3(0, 0, 0);
        }

        // 遍历 6 个面
        for (int face = 0; face < 6; face++)
        {
            const std::vector<XMFLOAT4>& faceData = cubemapData[face];

            // 遍历该面的所有像素
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    // 1. 计算 UV 和立体角
                    float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    float solidAngle = ComputeSolidAngle(u, v, size);

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
                    std::array<float, 9> basis;
                    EvaluateBasis(dir, basis);

                    // 5. 累加到 SH 系数（Riemann 求和）
                    // c_i = ∫ f(ω) * Y_i(ω) dω ≈ Σ f * Y * dω
                    for (int i = 0; i < 9; i++)
                    {
                        outCoeffs[i].x += color.x * basis[i] * solidAngle;
                        outCoeffs[i].y += color.y * basis[i] * solidAngle;
                        outCoeffs[i].z += color.z * basis[i] * solidAngle;
                    }
                }
            }
        }
        // 不需要额外归一化，因为 solidAngle 已经是正确的 dω
    }

    // Flat buffer overload for GPU output format
    void ProjectCubemapToSH(
        const XMFLOAT4* flatCubemapData,
        int size,
        std::array<XMFLOAT3, 9>& outCoeffs)
    {
        // Initialize coefficients to zero
        for (int i = 0; i < 9; i++)
        {
            outCoeffs[i] = XMFLOAT3(0, 0, 0);
        }

        int pixelsPerFace = size * size;

        // Iterate over 6 faces
        for (int face = 0; face < 6; face++)
        {
            // Iterate over all pixels in this face
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    // 1. Compute UV and solid angle
                    float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    float solidAngle = ComputeSolidAngle(u, v, size);

                    // 2. Compute direction vector
                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);

                    // 3. Sample color from flat buffer
                    int pixelIndex = face * pixelsPerFace + y * size + x;
                    const XMFLOAT4& pixel = flatCubemapData[pixelIndex];
                    XMFLOAT3 color(pixel.x, pixel.y, pixel.z);

                    // 4. Compute SH basis functions
                    std::array<float, 9> basis;
                    EvaluateBasis(dir, basis);

                    // 5. Accumulate to SH coefficients (Riemann sum)
                    for (int i = 0; i < 9; i++)
                    {
                        outCoeffs[i].x += color.x * basis[i] * solidAngle;
                        outCoeffs[i].y += color.y * basis[i] * solidAngle;
                        outCoeffs[i].z += color.z * basis[i] * solidAngle;
                    }
                }
            }
        }
    }

    // ============================================
    // SH Evaluation (Reconstruction)
    // ============================================

    XMFLOAT3 EvaluateSH(
        const std::array<XMFLOAT3, 9>& coeffs,
        const XMFLOAT3& dir)
    {
        // 计算 SH 基函数
        std::array<float, 9> basis;
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

    // ============================================
    // SH Reconstruction to Cubemap
    // ============================================

    void ProjectSHToCubemap(
        const std::array<XMFLOAT3, 9>& coeffs,
        int size,
        std::array<std::vector<XMFLOAT4>, 6>& outCubemapData)
    {
        // 为每个面分配内存
        for (int face = 0; face < 6; face++)
        {
            outCubemapData[face].resize(size * size);
        }

        // 遍历 6 个面重建颜色
        for (int face = 0; face < 6; face++)
        {
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    // 计算方向向量
                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);

                    // 从 SH 重建颜色
                    XMFLOAT3 color = EvaluateSH(coeffs, dir);

                    // 存储到 cubemap
                    int pixelIndex = y * size + x;
                    outCubemapData[face][pixelIndex] = XMFLOAT4(color.x, color.y, color.z, 1.0f);
                }
            }
        }
    }

    // ============================================
    // Debug Export
    // ============================================

    void DebugExportSHAsCubemap(
        const std::array<XMFLOAT3, 9>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix)
    {
        // 重建 cubemap
        std::array<std::vector<XMFLOAT4>, 6> cubemapData;
        ProjectSHToCubemap(coeffs, size, cubemapData);

        // 确保输出目录存在
        std::filesystem::create_directories(outputDir);

        // 导出为 KTX2 cubemap (HDR format: R16G16B16A16_FLOAT)
        std::string ktxPath = outputDir + "/" + prefix + ".ktx2";
        CKTXExporter::ExportCubemapFromCPUData(cubemapData, size, ktxPath, true);
    }

    // ============================================
    // L1 SH Basis Functions (4 coefficients)
    // ============================================
    // L0: 1, L1: 3 = 4 total

    void EvaluateBasisL1(const XMFLOAT3& dir, std::array<float, 4>& basis)
    {
        float x = dir.x;
        float y = dir.y;
        float z = dir.z;

        // L0 (1 coeff)
        basis[0] = 0.282095f;  // Y_0^0

        // L1 (3 coeffs)
        basis[1] = 0.488603f * y;   // Y_1^-1
        basis[2] = 0.488603f * z;   // Y_1^0
        basis[3] = 0.488603f * x;   // Y_1^1
    }

    // ============================================
    // L1 SH Projection
    // ============================================

    void ProjectCubemapToSH_L1(
        const std::array<std::vector<XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<XMFLOAT3, 4>& outCoeffs)
    {
        for (int i = 0; i < 4; i++)
        {
            outCoeffs[i] = XMFLOAT3(0, 0, 0);
        }

        for (int face = 0; face < 6; face++)
        {
            const std::vector<XMFLOAT4>& faceData = cubemapData[face];

            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    float solidAngle = ComputeSolidAngle(u, v, size);

                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);

                    int pixelIndex = y * size + x;
                    XMFLOAT3 color(
                        faceData[pixelIndex].x,
                        faceData[pixelIndex].y,
                        faceData[pixelIndex].z
                    );

                    std::array<float, 4> basis;
                    EvaluateBasisL1(dir, basis);

                    for (int i = 0; i < 4; i++)
                    {
                        outCoeffs[i].x += color.x * basis[i] * solidAngle;
                        outCoeffs[i].y += color.y * basis[i] * solidAngle;
                        outCoeffs[i].z += color.z * basis[i] * solidAngle;
                    }
                }
            }
        }
    }

    // ============================================
    // L1 SH Evaluation
    // ============================================

    XMFLOAT3 EvaluateSH_L1(
        const std::array<XMFLOAT3, 4>& coeffs,
        const XMFLOAT3& dir)
    {
        std::array<float, 4> basis;
        EvaluateBasisL1(dir, basis);

        XMVECTOR result = XMVectorZero();

        for (int i = 0; i < 4; i++)
        {
            XMVECTOR coeff = XMLoadFloat3(&coeffs[i]);
            result = XMVectorAdd(result, XMVectorScale(coeff, basis[i]));
        }

        result = XMVectorMax(result, XMVectorZero());

        XMFLOAT3 output;
        XMStoreFloat3(&output, result);
        return output;
    }

    // ============================================
    // L1 SH Reconstruction to Cubemap
    // ============================================

    void ProjectSHToCubemap_L1(
        const std::array<XMFLOAT3, 4>& coeffs,
        int size,
        std::array<std::vector<XMFLOAT4>, 6>& outCubemapData)
    {
        for (int face = 0; face < 6; face++)
        {
            outCubemapData[face].resize(size * size);
        }

        for (int face = 0; face < 6; face++)
        {
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);
                    XMFLOAT3 color = EvaluateSH_L1(coeffs, dir);

                    int pixelIndex = y * size + x;
                    outCubemapData[face][pixelIndex] = XMFLOAT4(color.x, color.y, color.z, 1.0f);
                }
            }
        }
    }

    // ============================================
    // L1 Debug Export
    // ============================================

    void DebugExportSHAsCubemap_L1(
        const std::array<XMFLOAT3, 4>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix)
    {
        std::array<std::vector<XMFLOAT4>, 6> cubemapData;
        ProjectSHToCubemap_L1(coeffs, size, cubemapData);

        std::filesystem::create_directories(outputDir);

        std::string ktxPath = outputDir + "/" + prefix + ".ktx2";
        CKTXExporter::ExportCubemapFromCPUData(cubemapData, size, ktxPath, true);
    }

    // ============================================
    // L3 SH Basis Functions (16 coefficients)
    // ============================================
    // L0: 1, L1: 3, L2: 5, L3: 7 = 16 total

    void EvaluateBasisL3(const XMFLOAT3& dir, std::array<float, 16>& basis)
    {
        float x = dir.x;
        float y = dir.y;
        float z = dir.z;

        float x2 = x * x;
        float y2 = y * y;
        float z2 = z * z;

        // L0 (1 coeff)
        basis[0] = 0.282095f;  // Y_0^0

        // L1 (3 coeffs)
        basis[1] = 0.488603f * y;   // Y_1^-1
        basis[2] = 0.488603f * z;   // Y_1^0
        basis[3] = 0.488603f * x;   // Y_1^1

        // L2 (5 coeffs)
        basis[4] = 1.092548f * x * y;                      // Y_2^-2
        basis[5] = 1.092548f * y * z;                      // Y_2^-1
        basis[6] = 0.315392f * (3.0f * z2 - 1.0f);         // Y_2^0
        basis[7] = 1.092548f * x * z;                      // Y_2^1
        basis[8] = 0.546274f * (x2 - y2);                  // Y_2^2

        // L3 (7 coeffs)
        basis[9]  = 0.590044f * y * (3.0f * x2 - y2);      // Y_3^-3
        basis[10] = 2.890611f * x * y * z;                 // Y_3^-2
        basis[11] = 0.457046f * y * (5.0f * z2 - 1.0f);    // Y_3^-1
        basis[12] = 0.373176f * z * (5.0f * z2 - 3.0f);    // Y_3^0
        basis[13] = 0.457046f * x * (5.0f * z2 - 1.0f);    // Y_3^1
        basis[14] = 1.445306f * z * (x2 - y2);             // Y_3^2
        basis[15] = 0.590044f * x * (x2 - 3.0f * y2);      // Y_3^3
    }

    // ============================================
    // L3 SH Projection
    // ============================================

    void ProjectCubemapToSH_L3(
        const std::array<std::vector<XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<XMFLOAT3, 16>& outCoeffs)
    {
        for (int i = 0; i < 16; i++)
        {
            outCoeffs[i] = XMFLOAT3(0, 0, 0);
        }

        for (int face = 0; face < 6; face++)
        {
            const std::vector<XMFLOAT4>& faceData = cubemapData[face];

            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    float solidAngle = ComputeSolidAngle(u, v, size);

                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);

                    int pixelIndex = y * size + x;
                    XMFLOAT3 color(
                        faceData[pixelIndex].x,
                        faceData[pixelIndex].y,
                        faceData[pixelIndex].z
                    );

                    std::array<float, 16> basis;
                    EvaluateBasisL3(dir, basis);

                    for (int i = 0; i < 16; i++)
                    {
                        outCoeffs[i].x += color.x * basis[i] * solidAngle;
                        outCoeffs[i].y += color.y * basis[i] * solidAngle;
                        outCoeffs[i].z += color.z * basis[i] * solidAngle;
                    }
                }
            }
        }
    }

    // ============================================
    // L3 SH Evaluation
    // ============================================

    XMFLOAT3 EvaluateSH_L3(
        const std::array<XMFLOAT3, 16>& coeffs,
        const XMFLOAT3& dir)
    {
        std::array<float, 16> basis;
        EvaluateBasisL3(dir, basis);

        XMVECTOR result = XMVectorZero();

        for (int i = 0; i < 16; i++)
        {
            XMVECTOR coeff = XMLoadFloat3(&coeffs[i]);
            result = XMVectorAdd(result, XMVectorScale(coeff, basis[i]));
        }

        result = XMVectorMax(result, XMVectorZero());

        XMFLOAT3 output;
        XMStoreFloat3(&output, result);
        return output;
    }

    // ============================================
    // L3 SH Reconstruction to Cubemap
    // ============================================

    void ProjectSHToCubemap_L3(
        const std::array<XMFLOAT3, 16>& coeffs,
        int size,
        std::array<std::vector<XMFLOAT4>, 6>& outCubemapData)
    {
        for (int face = 0; face < 6; face++)
        {
            outCubemapData[face].resize(size * size);
        }

        for (int face = 0; face < 6; face++)
        {
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);
                    XMFLOAT3 color = EvaluateSH_L3(coeffs, dir);

                    int pixelIndex = y * size + x;
                    outCubemapData[face][pixelIndex] = XMFLOAT4(color.x, color.y, color.z, 1.0f);
                }
            }
        }
    }

    // ============================================
    // L3 Debug Export
    // ============================================

    void DebugExportSHAsCubemap_L3(
        const std::array<XMFLOAT3, 16>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix)
    {
        std::array<std::vector<XMFLOAT4>, 6> cubemapData;
        ProjectSHToCubemap_L3(coeffs, size, cubemapData);

        std::filesystem::create_directories(outputDir);

        std::string ktxPath = outputDir + "/" + prefix + ".ktx2";
        CKTXExporter::ExportCubemapFromCPUData(cubemapData, size, ktxPath, true);
    }

    // ============================================
    // L4 SH Basis Functions (25 coefficients)
    // ============================================
    // L0: 1, L1: 3, L2: 5, L3: 7, L4: 9 = 25 total
    // 参考: https://www.ppsloan.org/publications/StupidSH36.pdf

    void EvaluateBasisL4(const XMFLOAT3& dir, std::array<float, 25>& basis)
    {
        float x = dir.x;
        float y = dir.y;
        float z = dir.z;

        float x2 = x * x;
        float y2 = y * y;
        float z2 = z * z;

        // L0 (1 coeff)
        basis[0] = 0.282095f;  // Y_0^0

        // L1 (3 coeffs)
        basis[1] = 0.488603f * y;   // Y_1^-1
        basis[2] = 0.488603f * z;   // Y_1^0
        basis[3] = 0.488603f * x;   // Y_1^1

        // L2 (5 coeffs)
        basis[4] = 1.092548f * x * y;                      // Y_2^-2
        basis[5] = 1.092548f * y * z;                      // Y_2^-1
        basis[6] = 0.315392f * (3.0f * z2 - 1.0f);         // Y_2^0
        basis[7] = 1.092548f * x * z;                      // Y_2^1
        basis[8] = 0.546274f * (x2 - y2);                  // Y_2^2

        // L3 (7 coeffs)
        basis[9]  = 0.590044f * y * (3.0f * x2 - y2);      // Y_3^-3
        basis[10] = 2.890611f * x * y * z;                 // Y_3^-2
        basis[11] = 0.457046f * y * (5.0f * z2 - 1.0f);    // Y_3^-1
        basis[12] = 0.373176f * z * (5.0f * z2 - 3.0f);    // Y_3^0
        basis[13] = 0.457046f * x * (5.0f * z2 - 1.0f);    // Y_3^1
        basis[14] = 1.445306f * z * (x2 - y2);             // Y_3^2
        basis[15] = 0.590044f * x * (x2 - 3.0f * y2);      // Y_3^3

        // L4 (9 coeffs)
        basis[16] = 2.503343f * x * y * (x2 - y2);                         // Y_4^-4
        basis[17] = 1.770131f * y * z * (3.0f * x2 - y2);                  // Y_4^-3
        basis[18] = 0.946175f * x * y * (7.0f * z2 - 1.0f);                // Y_4^-2
        basis[19] = 0.669047f * y * z * (7.0f * z2 - 3.0f);                // Y_4^-1
        basis[20] = 0.105786f * (35.0f * z2 * z2 - 30.0f * z2 + 3.0f);     // Y_4^0
        basis[21] = 0.669047f * x * z * (7.0f * z2 - 3.0f);                // Y_4^1
        basis[22] = 0.473087f * (x2 - y2) * (7.0f * z2 - 1.0f);            // Y_4^2
        basis[23] = 1.770131f * x * z * (x2 - 3.0f * y2);                  // Y_4^3
        basis[24] = 0.625836f * (x2 * (x2 - 3.0f * y2) - y2 * (3.0f * x2 - y2));  // Y_4^4
    }

    // ============================================
    // L4 SH Projection
    // ============================================

    void ProjectCubemapToSH_L4(
        const std::array<std::vector<XMFLOAT4>, 6>& cubemapData,
        int size,
        std::array<XMFLOAT3, 25>& outCoeffs)
    {
        // 初始化系数为 0
        for (int i = 0; i < 25; i++)
        {
            outCoeffs[i] = XMFLOAT3(0, 0, 0);
        }

        // 遍历 6 个面
        for (int face = 0; face < 6; face++)
        {
            const std::vector<XMFLOAT4>& faceData = cubemapData[face];

            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    float u = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
                    float v = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
                    float solidAngle = ComputeSolidAngle(u, v, size);

                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);

                    int pixelIndex = y * size + x;
                    XMFLOAT3 color(
                        faceData[pixelIndex].x,
                        faceData[pixelIndex].y,
                        faceData[pixelIndex].z
                    );

                    std::array<float, 25> basis;
                    EvaluateBasisL4(dir, basis);

                    for (int i = 0; i < 25; i++)
                    {
                        outCoeffs[i].x += color.x * basis[i] * solidAngle;
                        outCoeffs[i].y += color.y * basis[i] * solidAngle;
                        outCoeffs[i].z += color.z * basis[i] * solidAngle;
                    }
                }
            }
        }
    }

    // ============================================
    // L4 SH Evaluation
    // ============================================

    XMFLOAT3 EvaluateSH_L4(
        const std::array<XMFLOAT3, 25>& coeffs,
        const XMFLOAT3& dir)
    {
        std::array<float, 25> basis;
        EvaluateBasisL4(dir, basis);

        XMVECTOR result = XMVectorZero();

        for (int i = 0; i < 25; i++)
        {
            XMVECTOR coeff = XMLoadFloat3(&coeffs[i]);
            result = XMVectorAdd(result, XMVectorScale(coeff, basis[i]));
        }

        result = XMVectorMax(result, XMVectorZero());

        XMFLOAT3 output;
        XMStoreFloat3(&output, result);
        return output;
    }

    // ============================================
    // L4 SH Reconstruction to Cubemap
    // ============================================

    void ProjectSHToCubemap_L4(
        const std::array<XMFLOAT3, 25>& coeffs,
        int size,
        std::array<std::vector<XMFLOAT4>, 6>& outCubemapData)
    {
        for (int face = 0; face < 6; face++)
        {
            outCubemapData[face].resize(size * size);
        }

        for (int face = 0; face < 6; face++)
        {
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    XMFLOAT3 dir = CubemapTexelToDirection(face, x, y, size);
                    XMFLOAT3 color = EvaluateSH_L4(coeffs, dir);

                    int pixelIndex = y * size + x;
                    outCubemapData[face][pixelIndex] = XMFLOAT4(color.x, color.y, color.z, 1.0f);
                }
            }
        }
    }

    // ============================================
    // L4 Debug Export
    // ============================================

    void DebugExportSHAsCubemap_L4(
        const std::array<XMFLOAT3, 25>& coeffs,
        int size,
        const std::string& outputDir,
        const std::string& prefix)
    {
        std::array<std::vector<XMFLOAT4>, 6> cubemapData;
        ProjectSHToCubemap_L4(coeffs, size, cubemapData);

        std::filesystem::create_directories(outputDir);

        std::string ktxPath = outputDir + "/" + prefix + ".ktx2";
        CKTXExporter::ExportCubemapFromCPUData(cubemapData, size, ktxPath, true);
    }

} // namespace SphericalHarmonics
