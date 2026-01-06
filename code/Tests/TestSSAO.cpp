#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/SceneLightSettings.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Rendering/RenderPipeline.h"
#include "Engine/Rendering/Deferred/DeferredRenderPipeline.h"
#include "Engine/Camera.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: SSAO (Screen-Space Ambient Occlusion)
 *
 * Purpose:
 *   Verify that the GTAO-based SSAO implementation works correctly.
 *   Tests occlusion detection, bilateral blur, and edge-preserving upsample.
 *
 * Scene Setup:
 *   - Cornell box style setup with walls and floor
 *   - Objects with various contact scenarios (corners, crevices)
 *   - Demonstrates SSAO in wall-floor intersections and object occlusion
 *
 * Expected Results:
 *   - Dark occlusion in corners where walls meet floor
 *   - Contact shadows at object-floor intersections
 *   - Smooth, noise-free AO with bilateral blur
 *   - No edge bleeding from depth-aware upsample
 */
class CTestSSAO : public ITestCase {
public:
    const char* GetName() const override {
        return "TestSSAO";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene optimized for SSAO visibility
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Set up camera to view the scene
            // Scene is centered around (0, 1, 6), camera positioned at front-right elevated
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({4.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 6.0f});
            CFFLog::Info("[TestSSAO:Frame1] Camera positioned at (4, 4, 0) looking at (0, 1, 6)");

            // Directional light (moderate intensity to see AO clearly)
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 1.0f, 1.0f);
            dirLight->intensity = 2.0f;

            // Floor
            auto* floor = scene.GetWorld().Create("Floor");
            auto* floorT = floor->AddComponent<STransform>();
            floorT->position = XMFLOAT3(0.0f, 0.0f, 5.0f);
            floorT->scale = XMFLOAT3(8.0f, 0.1f, 8.0f);
            auto* floorMesh = floor->AddComponent<SMeshRenderer>();
            floorMesh->path = "mesh/cube.obj";

            // Back wall (creates corner with floor - should show AO)
            auto* backWall = scene.GetWorld().Create("BackWall");
            auto* backWallT = backWall->AddComponent<STransform>();
            backWallT->position = XMFLOAT3(0.0f, 2.0f, 9.0f);
            backWallT->scale = XMFLOAT3(8.0f, 4.0f, 0.1f);
            auto* backWallMesh = backWall->AddComponent<SMeshRenderer>();
            backWallMesh->path = "mesh/cube.obj";

            // Left wall (another corner)
            auto* leftWall = scene.GetWorld().Create("LeftWall");
            auto* leftWallT = leftWall->AddComponent<STransform>();
            leftWallT->position = XMFLOAT3(-4.0f, 2.0f, 5.0f);
            leftWallT->scale = XMFLOAT3(0.1f, 4.0f, 8.0f);
            auto* leftWallMesh = leftWall->AddComponent<SMeshRenderer>();
            leftWallMesh->path = "mesh/cube.obj";

            // Box sitting on floor (contact shadow)
            auto* box1 = scene.GetWorld().Create("Box1");
            auto* box1T = box1->AddComponent<STransform>();
            box1T->position = XMFLOAT3(-1.5f, 0.4f, 5.0f);
            box1T->scale = XMFLOAT3(0.8f, 0.8f, 0.8f);
            auto* box1Mesh = box1->AddComponent<SMeshRenderer>();
            box1Mesh->path = "mesh/cube.obj";

            // Sphere sitting on floor (curved contact shadow)
            auto* sphere1 = scene.GetWorld().Create("Sphere1");
            auto* sphere1T = sphere1->AddComponent<STransform>();
            sphere1T->position = XMFLOAT3(1.5f, 0.5f, 5.0f);
            sphere1T->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* sphere1Mesh = sphere1->AddComponent<SMeshRenderer>();
            sphere1Mesh->path = "mesh/sphere.obj";

            // Box in corner (maximum AO)
            auto* cornerBox = scene.GetWorld().Create("CornerBox");
            auto* cornerBoxT = cornerBox->AddComponent<STransform>();
            cornerBoxT->position = XMFLOAT3(-3.5f, 0.3f, 8.5f);
            cornerBoxT->scale = XMFLOAT3(0.6f, 0.6f, 0.6f);
            auto* cornerBoxMesh = cornerBox->AddComponent<SMeshRenderer>();
            cornerBoxMesh->path = "mesh/cube.obj";

            // Tall pillar
            auto* pillar = scene.GetWorld().Create("Pillar");
            auto* pillarT = pillar->AddComponent<STransform>();
            pillarT->position = XMFLOAT3(0.0f, 1.0f, 6.0f);
            pillarT->scale = XMFLOAT3(0.3f, 2.0f, 0.3f);
            auto* pillarMesh = pillar->AddComponent<SMeshRenderer>();
            pillarMesh->path = "mesh/cube.obj";

            CFFLog::Info("[TestSSAO:Frame1] Scene created with walls, floor, and objects");
        });

        // Frame 5: Enable SSAO with default settings
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame5] Enabling SSAO");

            // Get SSAO pass from pipeline
            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                auto& ssaoSettings = deferredPipeline->GetSSAOPass().GetSettings();
                ssaoSettings.enabled = true;
                ssaoSettings.radius = 0.5f;
                ssaoSettings.intensity = 1.5f;
                ssaoSettings.numSlices = 3;
                ssaoSettings.numSteps = 4;
                ssaoSettings.blurRadius = 2;

                CFFLog::Info("[TestSSAO:Frame5] SSAO enabled: radius=%.2f, intensity=%.2f",
                            ssaoSettings.radius, ssaoSettings.intensity);
            } else {
                CFFLog::Error("[TestSSAO:Frame5] Not using deferred pipeline - SSAO not available");
            }
        });

        // Frame 20: Capture with SSAO enabled
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame20] Capturing screenshot with SSAO enabled");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Dark occlusion in corners, contact shadows at floor");
        });

        // Frame 25: Disable SSAO for comparison
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame25] Disabling SSAO for comparison");

            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                deferredPipeline->GetSSAOPass().GetSettings().enabled = false;
            }
        });

        // Frame 30: Capture without SSAO
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame30] Capturing screenshot without SSAO");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 30);

            CFFLog::Info("VISUAL_EXPECTATION: Same scene without ambient occlusion");
        });

        // Frame 35: Re-enable with high intensity
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame35] Testing high intensity SSAO");

            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                auto& ssaoSettings = deferredPipeline->GetSSAOPass().GetSettings();
                ssaoSettings.enabled = true;
                ssaoSettings.radius = 1.0f;      // Larger radius
                ssaoSettings.intensity = 2.5f;   // Higher intensity
                ssaoSettings.numSlices = 4;      // Max quality
                ssaoSettings.numSteps = 6;
            }
        });

        // Frame 45: Capture high intensity SSAO
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame45] Capturing high intensity SSAO");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 45);

            CFFLog::Info("VISUAL_EXPECTATION: Stronger AO effect with larger occlusion halos");
        });

        // Frame 50: Finish test
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame50] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: SSAO rendering completed without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestSSAO)
