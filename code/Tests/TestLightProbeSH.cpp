#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/SphericalHarmonics.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/LightProbe.h"
#include "Engine/Rendering/LightProbeManager.h"
#include <DirectXMath.h>

using namespace DirectX;

// ============================================
// TestLightProbeSH - 测试 SH 编码/解码
// ============================================
// 验证球谐函数的编码和解码是否正确
//
// 测试策略：
// 1. 创建一个简单的 Cubemap（纯色或渐变）
// 2. 投影到 SH 系数
// 3. 从 SH 系数重建，验证误差在可接受范围内
// ============================================
class CTestLightProbeSH : public ITestCase
{
public:
    const char* GetName() const override { return "TestLightProbeSH"; }

    void Setup(CTestContext& ctx) override
    {
        ctx.OnFrame(1, [&]() {
            CFFLog::Info("[TestLightProbeSH] Frame 1: Testing SH encoding/decoding");

            // ============================================
            // 测试 1：纯色 Cubemap
            // ============================================
            TestSolidColorCubemap(ctx);

            // ============================================
            // 测试 2：渐变 Cubemap
            // ============================================
            TestGradientCubemap(ctx);

            // ============================================
            // 测试 3：SH 基函数正交性
            // ============================================
            TestSHOrthogonality(ctx);
        });

        ctx.OnFrame(10, [&]() {
            CFFLog::Info("[TestLightProbeSH] Frame 10: Test complete");
            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }

private:
    // 测试纯色 Cubemap
    void TestSolidColorCubemap(CTestContext& ctx)
    {
        CFFLog::Info("[TestLightProbeSH] Test 1: Solid color cubemap");

        // 创建纯红色 Cubemap (32x32)
        const int size = 32;
        std::array<std::vector<XMFLOAT4>, 6> cubemapData;
        for (int face = 0; face < 6; face++)
        {
            cubemapData[face].resize(size * size);
            for (int i = 0; i < size * size; i++)
            {
                cubemapData[face][i] = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);  // 纯红色
            }
        }

        // 投影到 SH
        std::array<XMFLOAT3, 9> shCoeffs;
        SphericalHarmonics::ProjectCubemapToSH(cubemapData, size, shCoeffs);

        // 验证：L0 系数应该接近 (1, 0, 0) * 0.282095
        // 因为纯色 Cubemap 只有 DC 分量
        float expectedL0 = 1.0f * 0.282095f;
        ASSERT_IN_RANGE(ctx, shCoeffs[0].x, expectedL0 * 0.9f, expectedL0 * 1.1f,
                       "L0 coefficient R should be close to expected");
        ASSERT_IN_RANGE(ctx, shCoeffs[0].y, -0.1f, 0.1f,
                       "L0 coefficient G should be near zero");
        ASSERT_IN_RANGE(ctx, shCoeffs[0].z, -0.1f, 0.1f,
                       "L0 coefficient B should be near zero");

        // L1-L2 系数应该接近 0（纯色没有方向性）
        for (int i = 1; i < 9; i++)
        {
            ASSERT_IN_RANGE(ctx, shCoeffs[i].x, -0.1f, 0.1f,
                           "Higher order coefficients should be near zero");
        }

        CFFLog::Info("[TestLightProbeSH] Test 1 passed: L0=%.3f,%.3f,%.3f",
                    shCoeffs[0].x, shCoeffs[0].y, shCoeffs[0].z);
    }

    // 测试渐变 Cubemap
    void TestGradientCubemap(CTestContext& ctx)
    {
        CFFLog::Info("[TestLightProbeSH] Test 2: Gradient cubemap");

        // 创建从上到下的渐变 Cubemap
        const int size = 32;
        std::array<std::vector<XMFLOAT4>, 6> cubemapData;
        for (int face = 0; face < 6; face++)
        {
            cubemapData[face].resize(size * size);
            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    // 根据方向计算颜色
                    XMFLOAT3 dir = SphericalHarmonics::CubemapTexelToDirection(face, x, y, size);

                    // 上半球白色，下半球黑色
                    float brightness = (dir.y > 0) ? 1.0f : 0.0f;

                    int idx = y * size + x;
                    cubemapData[face][idx] = XMFLOAT4(brightness, brightness, brightness, 1.0f);
                }
            }
        }

        // 投影到 SH
        std::array<XMFLOAT3, 9> shCoeffs;
        SphericalHarmonics::ProjectCubemapToSH(cubemapData, size, shCoeffs);

        // 验证：L1 Y 分量应该为正（上半球亮）
        ASSERT(ctx, shCoeffs[1].x > 0.1f, "L1 Y coefficient should be positive (top hemisphere is bright)");

        // 重建并验证
        XMFLOAT3 topDir(0, 1, 0);  // 向上
        XMFLOAT3 bottomDir(0, -1, 0);  // 向下

        XMFLOAT3 topColor = SphericalHarmonics::EvaluateSH(shCoeffs, topDir);
        XMFLOAT3 bottomColor = SphericalHarmonics::EvaluateSH(shCoeffs, bottomDir);

        // 上方应该比下方亮
        ASSERT(ctx, topColor.x > bottomColor.x, "Top should be brighter than bottom");

        CFFLog::Info("[TestLightProbeSH] Test 2 passed: Top=%.3f, Bottom=%.3f",
                    topColor.x, bottomColor.x);
    }

    // 测试 SH 基函数正交性
    void TestSHOrthogonality(CTestContext& ctx)
    {
        CFFLog::Info("[TestLightProbeSH] Test 3: SH basis orthogonality");

        // 采样球面上的多个方向
        const int sampleCount = 100;
        float dotProducts[9][9] = {0};

        for (int i = 0; i < sampleCount; i++)
        {
            // 均匀采样球面
            float theta = XM_PI * (float)i / (float)sampleCount;
            float phi = XM_2PI * (float)i / (float)sampleCount;

            XMFLOAT3 dir(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta)
            );

            // 计算基函数
            std::array<float, 9> basis;
            SphericalHarmonics::EvaluateBasis(dir, basis);

            // 累加点积
            for (int j = 0; j < 9; j++)
            {
                for (int k = 0; k < 9; k++)
                {
                    dotProducts[j][k] += basis[j] * basis[k];
                }
            }
        }

        // 归一化
        for (int j = 0; j < 9; j++)
        {
            for (int k = 0; k < 9; k++)
            {
                dotProducts[j][k] /= (float)sampleCount;
            }
        }

        // 验证对角线接近 1，非对角线接近 0
        for (int j = 0; j < 9; j++)
        {
            for (int k = 0; k < 9; k++)
            {
                if (j == k)
                {
                    // 对角线应该接近 1（但不完全是，因为采样有限）
                    ASSERT_IN_RANGE(ctx, dotProducts[j][k], 0.5f, 1.5f,
                                   "Diagonal should be close to 1");
                }
                else
                {
                    // 非对角线应该接近 0
                    ASSERT_IN_RANGE(ctx, dotProducts[j][k], -0.3f, 0.3f,
                                   "Off-diagonal should be close to 0");
                }
            }
        }

        CFFLog::Info("[TestLightProbeSH] Test 3 passed: SH basis functions are approximately orthogonal");
    }
};

REGISTER_TEST(CTestLightProbeSH)
