#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Components/ReflectionProbe.h"
#include <DirectXMath.h>

using namespace DirectX;

// ============================================
// TestReflectionProbe - 测试 Reflection Probe 渲染
// ============================================
// 验证：
// 1. Reflection Probe 组件正确加载
// 2. 物体在 Probe 影响范围内时使用局部 IBL
// 3. 物体在 Probe 影响范围外时使用全局 IBL
//
// 场景设置：
// - 一个 Reflection Probe 位于原点，半径 10
// - 一个金属球在 Probe 范围内 (0, 1, 0)
// - 一个金属球在 Probe 范围外 (20, 1, 0)
// - 视觉上两个球应该有不同的反射效果
// ============================================
class CTestReflectionProbe : public ITestCase
{
public:
    const char* GetName() const override { return "TestReflectionProbe"; }

    void Setup(CTestContext& ctx) override
    {
        CFFLog::Info("[TestReflectionProbe] Setting up reflection probe test scene...");

        // Frame 1: Setup scene
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestReflectionProbe:Frame1] Creating test scene");

            auto& scene = CScene::Instance();

            // Clear existing scene
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            auto& world = scene.GetWorld();

            // 1. Directional Light
            {
                auto* lightObj = world.Create("DirectionalLight");
                auto* transform = lightObj->AddComponent<STransform>();
                transform->rotationEuler = { XMConvertToRadians(45.0f), XMConvertToRadians(-30.0f), 0.0f };

                auto* light = lightObj->AddComponent<SDirectionalLight>();
                light->color = { 1.0f, 1.0f, 1.0f };
                light->intensity = 1.0f;
                light->ibl_intensity = 1.0f;
            }

            // 2. Reflection Probe at origin
            {
                auto* probeObj = world.Create("ReflectionProbe");
                auto* transform = probeObj->AddComponent<STransform>();
                transform->position = { 0.0f, 0.0f, 0.0f };

                auto* probe = probeObj->AddComponent<SReflectionProbe>();
                probe->radius = 10.0f;
                probe->resolution = 256;
                // 使用已存在的烘焙资产
                probe->assetPath = "reflection_probe/reflection_probe.ffasset";
            }

            // 3. Metal sphere INSIDE probe range (should use local IBL)
            {
                auto* sphereObj = world.Create("Sphere_InProbe");
                auto* transform = sphereObj->AddComponent<STransform>();
                transform->position = { 0.0f, 1.0f, 0.0f };  // 在 Probe 半径内

                auto* meshRenderer = sphereObj->AddComponent<SMeshRenderer>();
                meshRenderer->path = "mesh/sphere.obj";
                meshRenderer->materialPath = "materials/default_metal.ffasset";
            }

            // 4. Metal sphere OUTSIDE probe range (should use global IBL)
            {
                auto* sphereObj = world.Create("Sphere_OutProbe");
                auto* transform = sphereObj->AddComponent<STransform>();
                transform->position = { 20.0f, 1.0f, 0.0f };  // 在 Probe 半径外

                auto* meshRenderer = sphereObj->AddComponent<SMeshRenderer>();
                meshRenderer->path = "mesh/sphere.obj";
                meshRenderer->materialPath = "materials/default_metal.ffasset";
            }

            // 设置相机位置，能同时看到两个球
            // 左球在 (0,1,0)，右球在 (20,1,0)，中点在 (10,1,0)
            scene.GetEditorCamera().position = { 10.0f, 3.0f, -10.0f };
            scene.GetEditorCamera().SetLookAt(
                { 10.0f, 3.0f, -10.0f },  // eye
                { 10.0f, 1.0f, 0.0f },    // target (中间位置)
                { 0.0f, 1.0f, 0.0f }      // up
            );

            CFFLog::Info("[TestReflectionProbe:Frame1] Scene setup complete");
        });

        // Frame 10: Verify setup
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("[TestReflectionProbe:Frame10] Verifying scene setup");

            auto& world = CScene::Instance().GetWorld();
            int sphereCount = 0;
            int probeCount = 0;

            for (auto& go : world.Objects()) {
                if (go->GetComponent<SMeshRenderer>()) sphereCount++;
                if (go->GetComponent<SReflectionProbe>()) probeCount++;
            }

            ASSERT_EQUAL(ctx, sphereCount, 2, "Should have 2 spheres");
            ASSERT_EQUAL(ctx, probeCount, 1, "Should have 1 reflection probe");

            CFFLog::Info("VISUAL_EXPECTATION: Two metal spheres visible");
            CFFLog::Info("VISUAL_EXPECTATION: Left sphere (in probe range) uses local IBL");
            CFFLog::Info("VISUAL_EXPECTATION: Right sphere (outside probe range) uses global IBL");

            CFFLog::Info("[TestReflectionProbe:Frame10] Verification complete");
        });

        // Frame 20: Capture screenshot
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestReflectionProbe:Frame20] Capturing screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            // Verify probe component data
            auto& world = CScene::Instance().GetWorld();
            for (auto& go : world.Objects()) {
                if (auto* probe = go->GetComponent<SReflectionProbe>()) {
                    ASSERT_EQUAL(ctx, probe->radius, 10.0f, "Probe radius should be 10");
                    ASSERT(ctx, !probe->assetPath.empty(), "Probe should have asset path");
                    CFFLog::Info("[TestReflectionProbe] Probe asset: %s", probe->assetPath.c_str());
                }
            }

            CFFLog::Info("[TestReflectionProbe:Frame20] Screenshot captured");
        });

        // Frame 30: Finalize
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestReflectionProbe:Frame30] Test finalization");

            if (ctx.failures.empty()) {
                CFFLog::Info("[TestReflectionProbe] ALL ASSERTIONS PASSED");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("[TestReflectionProbe] TEST FAILED - %d assertions failed:", (int)ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
                ctx.testPassed = false;
            }

            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestReflectionProbe)
