#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Rendering/MainPass.h"
#include "Editor/PickingUtils.h"
#include <DirectXMath.h>
#include <sstream>

using namespace DirectX;

class CTestRayCast : public ITestCase {
public:
    const char* GetName() const override {
        return "TestRayCast";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene with a cube
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("Frame 1: Creating test scene");

            // Clear existing scene
            auto& scene = CScene::Instance();
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            // Create a cube in front of the camera
            // Left-handed coordinate system: camera looks at +X direction
            // Camera is at approximately (-6, 0.8, 0), so align cube's Y
            auto* cube = scene.GetWorld().Create("TestCube");

            auto* transform = cube->AddComponent<STransform>();
            transform->position = {5.0f, 0.8f, 0.0f};  // 5 units along +X, align Y with camera
            transform->scale = {1.0f, 1.0f, 1.0f};

            auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
            meshRenderer->path = "mesh/cube.obj";

            CFFLog::Info("Created cube at position (5, 0.8, 0) - aligned with camera height");
        });

        // Frame 10: Wait for resources to load and verify scene setup
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("Frame 10: Waiting for resources to load...");

            auto& scene = CScene::Instance();

            // Assertions: verify scene setup
            ASSERT_EQUAL(ctx, (int)scene.GetWorld().Count(), 1, "Scene should have 1 object");

            auto* cube = scene.GetWorld().Get(0);
            ASSERT_NOT_NULL(ctx, cube, "Test cube object");
            ASSERT_EQUAL(ctx, cube->GetName(), std::string("TestCube"), "Object name");

            auto* transform = cube->GetComponent<STransform>();
            ASSERT_NOT_NULL(ctx, transform, "Transform component");
            ASSERT_VEC3_EQUAL(ctx,
                            transform->position,
                            (DirectX::XMFLOAT3{5.0f, 0.8f, 0.0f}),
                            0.01f,
                            "Cube position");

            auto* meshRenderer = cube->GetComponent<SMeshRenderer>();
            ASSERT_NOT_NULL(ctx, meshRenderer, "MeshRenderer component");

            // Generate and log scene report
            std::string report = scene.GenerateReport();
            CFFLog::Info("Scene State:\n%s", report.c_str());

            CFFLog::Info("✓ Frame 10: All setup assertions passed");
        });

        // Frame 20: Perform raycast test
        ctx.OnFrame(20, [&ctx]() {
            auto& log = CFFLog::Instance();
            log.BeginSession("TEST_SESSION", "Raycast Test");

            CFFLog::Info("Frame 20: Performing raycast test");

            // Take screenshot before raycast
            CScreenshot::CaptureTest(ctx.mainPass, "TestRayCast", 20);

            // Get actual camera matrices from MainPass (don't hardcode!)
            XMMATRIX viewMatrix = ctx.mainPass->GetCameraViewMatrix();
            XMMATRIX projMatrix = ctx.mainPass->GetCameraProjMatrix();

            // Get viewport size from MainPass
            UINT vpWidth = ctx.mainPass->GetOffscreenWidth();
            UINT vpHeight = ctx.mainPass->GetOffscreenHeight();
            float screenX = vpWidth / 2.0f;
            float screenY = vpHeight / 2.0f;

            log.LogEvent("Ray Generation");
            log.LogInfo("Input:");
            log.LogInfo("  screenX=%.1f, screenY=%.1f (center)", screenX, screenY);
            log.LogInfo("  viewportW=%u, viewportH=%u", vpWidth, vpHeight);

            // Cast ray from center of screen (should hit the cube)
            PickingUtils::Ray ray = PickingUtils::GenerateRayFromScreen(
                screenX, screenY,  // Screen center
                (float)vpWidth, (float)vpHeight,  // Viewport size
                viewMatrix,
                projMatrix
            );

            log.LogInfo("Ray (World Space):");
            log.LogVector("  Origin", ray.origin);
            log.LogVector("  Direction", ray.direction);

            // Test intersection with the cube
            auto& scene = CScene::Instance();
            bool hitAnything = false;
            float closestDist = FLT_MAX;
            int hitIndex = -1;

            log.LogEvent("Intersection Tests");
            log.LogInfo("Testing %d objects...", scene.GetWorld().Count());
            log.LogInfo("");

            for (int i = 0; i < scene.GetWorld().Count(); ++i) {
                auto* obj = scene.GetWorld().Get(i);
                auto* meshRenderer = obj->GetComponent<SMeshRenderer>();

                if (!meshRenderer) continue;

                // Get world transform
                auto* transform = obj->GetComponent<STransform>();
                XMMATRIX worldMatrix = transform->WorldMatrix();

                // Get local AABB from mesh (for now, use a simple cube AABB)
                XMFLOAT3 localMin = {-0.5f, -0.5f, -0.5f};
                XMFLOAT3 localMax = {0.5f, 0.5f, 0.5f};

                log.LogSubsectionStart((std::string("[") + std::to_string(i+1) + "/" +
                    std::to_string(scene.GetWorld().Count()) + "] Object: \"" +
                    obj->GetName() + "\"").c_str());
                log.LogInfo("Transform:");
                log.LogVector("  Position", transform->position);
                log.LogVector("  Scale", transform->scale);
                log.LogAABB("Local AABB", localMin, localMax);

                // Transform AABB to world space
                XMFLOAT3 worldMin, worldMax;
                PickingUtils::TransformAABB(localMin, localMax, worldMatrix, worldMin, worldMax);

                log.LogAABB("World AABB (after transform)", worldMin, worldMax);

                // Test intersection
                auto distance = PickingUtils::RayAABBIntersect(ray, worldMin, worldMax);

                if (distance.has_value()) {
                    log.LogSuccess(("HIT at distance " + std::to_string(distance.value())).c_str());

                    if (distance.value() < closestDist) {
                        closestDist = distance.value();
                        hitIndex = i;
                        hitAnything = true;
                    }
                } else {
                    log.LogFailure("NO HIT");
                }

                log.LogSubsectionEnd();
            }

            // Verify results with assertions
            log.LogEvent("Test Verification");
            log.LogInfo("Hits found: %d", (hitAnything ? 1 : 0));

            if (hitAnything) {
                const char* hitName = scene.GetWorld().Get(hitIndex)->GetName().c_str();
                log.LogInfo("Closest hit: \"%s\" at distance %.3f (index %d)",
                    hitName, closestDist, hitIndex);
            }

            // Assertions: verify raycast results
            ASSERT(ctx, hitAnything, "Raycast should hit the cube");
            ASSERT_EQUAL(ctx, hitIndex, 0, "Should hit the first object");
            ASSERT_IN_RANGE(ctx, closestDist, 10.0f, 11.0f, "Hit distance should be ~10.4");

            log.LogSuccess("TEST PASSED: All raycast assertions passed");
            CFFLog::Info("✓ Test PASSED: Raycast hit the expected object at distance %.2f", closestDist);

            // Add scene state report to test log
            log.LogEvent("Scene State Report");
            std::string sceneReport = scene.GenerateReport();
            // Split report into lines for proper logging
            std::istringstream iss(sceneReport);
            std::string line;
            while (std::getline(iss, line)) {
                log.LogInfo("%s", line.c_str());
            }

            log.EndSession();
            log.FlushToFile("E:/forfun/debug/logs/test_raycast.log");
        });

        // Frame 30: Finish test
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("Frame 30: Test finished");

            // Mark test as passed if no failures
            if (ctx.failures.empty()) {
                ctx.testPassed = true;
                CFFLog::Info("✓ ALL ASSERTIONS PASSED");
            } else {
                ctx.testPassed = false;
                CFFLog::Error("✗ TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
            }

            ctx.Finish();
        });
    }
};

// Register the test
REGISTER_TEST(CTestRayCast)
