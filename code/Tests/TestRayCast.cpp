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

        // Frame 10: Wait for resources to load
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("Frame 10: Waiting for resources to load...");

            // Verify scene has objects
            auto& scene = CScene::Instance();
            int objectCount = scene.GetWorld().Count();
            CFFLog::Info("Scene has %d object(s)", objectCount);
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

            // Verify results
            log.LogEvent("Test Verification");
            log.LogInfo("Hits found: %d", (hitAnything ? 1 : 0));

            if (hitAnything) {
                const char* hitName = scene.GetWorld().Get(hitIndex)->GetName().c_str();
                log.LogInfo("Closest hit: \"%s\" at distance %.3f (index %d)",
                    hitName, closestDist, hitIndex);

                CFFLog::Info("✓ Raycast hit object at index %d (distance: %.2f)",
                    hitIndex, closestDist);
                ctx.testPassed = (hitIndex == 0);  // Should hit the first (and only) object

                if (ctx.testPassed) {
                    log.LogSuccess("TEST PASSED: Hit expected object");
                    CFFLog::Info("✓ Test PASSED: Raycast hit the expected object");
                } else {
                    log.LogFailure("TEST FAILED: Hit wrong object");
                    CFFLog::Error("✗ Test FAILED: Raycast hit wrong object");
                }
            } else {
                log.LogFailure("TEST FAILED: No hits");
                CFFLog::Error("✗ Raycast missed all objects");
                CFFLog::Error("✗ Test FAILED: Raycast did not hit the expected object");
                ctx.testPassed = false;
            }

            log.EndSession();
            log.FlushToFile("E:/forfun/debug/logs/test_raycast.log");
        });

        // Frame 30: Finish test
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("Frame 30: Test finished");
            ctx.Finish();
        });
    }
};

// Register the test
REGISTER_TEST(CTestRayCast)
