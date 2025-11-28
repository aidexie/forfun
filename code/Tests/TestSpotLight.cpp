#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/SpotLight.h"
#include "Engine/Rendering/MainPass.h"
#include <DirectXMath.h>

using namespace DirectX;

// Test Spot Light functionality with stage lighting setup
// Scene: 3 spot lights illuminating cubes from different angles
class CTestSpotLight : public ITestCase {
public:
    const char* GetName() const override {
        return "TestSpotLight";
    }

    void Setup(CTestContext& ctx) override {
        CFFLog::Info("[TestSpotLight] Setting up spot light test scene...");

        // Frame 1-10: Setup scene
        ctx.OnFrame(1, [&]() {
            CFFLog::Info("[TestSpotLight:Frame1] Creating test scene");

            auto& scene = CScene::Instance();

            // Camera default: (-6, 0.8, 0) looking +X direction
            // Create 3 cubes spread along X axis
            float cubeY = 0.5f;
            XMFLOAT3 cubePositions[] = {
                {2.0f, cubeY, -2.0f},   // Left (closer to camera in Z)
                {5.0f, cubeY, 0.0f},    // Center
                {8.0f, cubeY, 2.0f}     // Right (farther in Z)
            };

            for (int i = 0; i < 3; i++) {
                auto* cube = scene.GetWorld().Create("Cube" + std::to_string(i));
                auto* transform = cube->AddComponent<STransform>();
                transform->position = cubePositions[i];
                transform->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);

                auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
                meshRenderer->path = "mesh/cube.obj";
                meshRenderer->materialPath = "materials/default_white.ffasset";
            }

            // Create 3 spot lights (red, green, blue) above cubes, pointing down
            // Red spot light (left cube)
            {
                auto* spotLight = scene.GetWorld().Create("SpotLight_Red");
                auto* transform = spotLight->AddComponent<STransform>();
                transform->position = XMFLOAT3(2.0f, 4.0f, -2.0f);
                transform->SetRotation(0.0f, 0.0f, 0.0f);  // No rotation, direction is local down

                auto* light = spotLight->AddComponent<SSpotLight>();
                light->color = XMFLOAT3(1.0f, 0.0f, 0.0f);  // Red
                light->intensity = 500.0f;  // Increased for visibility
                light->range = 8.0f;
                light->direction = XMFLOAT3(0.0f, -1.0f, 0.0f);  // Local down
                light->innerConeAngle = 20.0f;  // degrees
                light->outerConeAngle = 35.0f;  // degrees
            }

            // Green spot light (center cube)
            {
                auto* spotLight = scene.GetWorld().Create("SpotLight_Green");
                auto* transform = spotLight->AddComponent<STransform>();
                transform->position = XMFLOAT3(5.0f, 5.0f, 0.0f);
                transform->SetRotation(0.0f, 0.0f, 0.0f);

                auto* light = spotLight->AddComponent<SSpotLight>();
                light->color = XMFLOAT3(0.0f, 1.0f, 0.0f);  // Green
                light->intensity = 600.0f;  // Increased for visibility
                light->range = 9.0f;
                light->direction = XMFLOAT3(0.0f, -1.0f, 0.0f);
                light->innerConeAngle = 15.0f;
                light->outerConeAngle = 30.0f;
            }

            // Blue spot light (right cube)
            {
                auto* spotLight = scene.GetWorld().Create("SpotLight_Blue");
                auto* transform = spotLight->AddComponent<STransform>();
                transform->position = XMFLOAT3(8.0f, 4.5f, 2.0f);
                transform->SetRotation(0.0f, 0.0f, 0.0f);

                auto* light = spotLight->AddComponent<SSpotLight>();
                light->color = XMFLOAT3(0.0f, 0.0f, 1.0f);  // Blue
                light->intensity = 600.0f;  // Increased for visibility
                light->range = 8.5f;
                light->direction = XMFLOAT3(0.0f, -1.0f, 0.0f);
                light->innerConeAngle = 18.0f;
                light->outerConeAngle = 32.0f;
            }

            CFFLog::Info("[TestSpotLight:Frame1] Scene setup complete");
        });

        // Frame 10: Verify setup
        ctx.OnFrame(10, [&]() {
            CFFLog::Info("[TestSpotLight:Frame10] Verifying scene setup");

            auto& world = CScene::Instance().GetWorld();
            int cubeCount = 0;
            int spotLightCount = 0;

            for (auto& go : world.Objects()) {
                if (go->GetComponent<SMeshRenderer>()) cubeCount++;
                if (go->GetComponent<SSpotLight>()) spotLightCount++;
            }

            ASSERT_EQUAL(ctx, cubeCount, 3, "Should have 3 cubes");
            ASSERT_EQUAL(ctx, spotLightCount, 3, "Should have 3 spot lights");

            CFFLog::Info("VISUAL_EXPECTATION: 3 cubes in a row lit by colored spot lights from above");
            CFFLog::Info("VISUAL_EXPECTATION: Red spot light on left cube, green in middle, blue on right");
            CFFLog::Info("VISUAL_EXPECTATION: Cone-shaped lighting with smooth falloff at edges");
            CFFLog::Info("VISUAL_EXPECTATION: No hard edges in lighting (smooth gradient from inner to outer cone)");

            CFFLog::Info("[TestSpotLight:Frame10] Verification complete, %d failures", (int)ctx.failures.size());
        });

        // Frame 20: Capture screenshot
        ctx.OnFrame(20, [&]() {
            CFFLog::Info("[TestSpotLight:Frame20] Capturing screenshot");
            CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);

            // Additional assertion: Verify light data
            auto& world = CScene::Instance().GetWorld();
            for (auto& go : world.Objects()) {
                if (auto* light = go->GetComponent<SSpotLight>()) {
                    ASSERT_IN_RANGE(ctx, light->innerConeAngle, 10.0f, 25.0f, "Inner cone angle should be reasonable");
                    ASSERT_IN_RANGE(ctx, light->outerConeAngle, 20.0f, 40.0f, "Outer cone angle should be reasonable");
                    ASSERT(ctx, light->outerConeAngle > light->innerConeAngle, "Outer cone must be larger than inner cone");
                }
            }

            CFFLog::Info("[TestSpotLight:Frame20] Screenshot captured, %d failures", (int)ctx.failures.size());
        });

        // Frame 30: Finalize
        ctx.OnFrame(30, [&]() {
            CFFLog::Info("[TestSpotLight:Frame30] Test finalization");

            if (ctx.failures.empty()) {
                CFFLog::Info("[TestSpotLight] ALL ASSERTIONS PASSED");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("[TestSpotLight] TEST FAILED - %d assertions failed:", (int)ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
                ctx.testPassed = false;
            }

            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestSpotLight)
