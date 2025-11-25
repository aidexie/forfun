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
            CFFLog::Info("Frame 20: Performing raycast test");

            // Take screenshot before raycast
            CScreenshot::CaptureTest(ctx.mainPass, "TestRayCast", 20);

            // Get actual camera matrices from MainPass (don't hardcode!)
            XMMATRIX viewMatrix = ctx.mainPass->GetCameraViewMatrix();
            XMMATRIX projMatrix = ctx.mainPass->GetCameraProjMatrix();

            // Get viewport size from MainPass
            UINT vpWidth = ctx.mainPass->GetOffscreenWidth();
            UINT vpHeight = ctx.mainPass->GetOffscreenHeight();

            // Cast ray from center of screen (should hit the cube)
            PickingUtils::Ray ray = PickingUtils::GenerateRayFromScreen(
                vpWidth / 2.0f, vpHeight / 2.0f,  // Screen center
                (float)vpWidth, (float)vpHeight,  // Viewport size
                viewMatrix,
                projMatrix
            );

            CFFLog::Info("Ray origin: (%.2f, %.2f, %.2f)",
                ray.origin.x, ray.origin.y, ray.origin.z);
            CFFLog::Info("Ray direction: (%.2f, %.2f, %.2f)",
                ray.direction.x, ray.direction.y, ray.direction.z);

            // Test intersection with the cube
            auto& scene = CScene::Instance();
            bool hitAnything = false;
            float closestDist = FLT_MAX;
            int hitIndex = -1;

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

                // Transform AABB to world space
                XMFLOAT3 worldMin, worldMax;
                PickingUtils::TransformAABB(localMin, localMax, worldMatrix, worldMin, worldMax);

                // Test intersection
                auto distance = PickingUtils::RayAABBIntersect(ray, worldMin, worldMax);

                if (distance.has_value() && distance.value() < closestDist) {
                    closestDist = distance.value();
                    hitIndex = i;
                    hitAnything = true;
                }
            }

            // Verify results
            if (hitAnything) {
                CFFLog::Info("✓ Raycast hit object at index %d (distance: %.2f)",
                    hitIndex, closestDist);
                ctx.testPassed = (hitIndex == 0);  // Should hit the first (and only) object
            } else {
                CFFLog::Error("✗ Raycast missed all objects");
                ctx.testPassed = false;
            }

            if (ctx.testPassed) {
                CFFLog::Info("✓ Test PASSED: Raycast hit the expected object");
            } else {
                CFFLog::Error("✗ Test FAILED: Raycast did not hit the expected object");
            }
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
