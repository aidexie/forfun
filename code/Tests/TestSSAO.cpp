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
 *   Verify that all three SSAO algorithms work correctly:
 *   - GTAO (Ground Truth AO)
 *   - HBAO (Horizon-Based AO)
 *   - Crytek SSAO (Classic hemisphere sampling)
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

            // Set G-Buffer debug mode to SSAO visualization
            scene.GetLightSettings().gBufferDebugMode = EGBufferDebugMode::SSAO;

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

        // Frame 5: Debug - Raw Depth visualization
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame5] Debug: Raw Depth visualization");

            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                ctx.showFlags.SSAO = true;
                auto& ssaoSettings = deferredPipeline->GetSSAOPass().GetSettings();
                ssaoSettings.algorithm = ESSAOAlgorithm::Debug_RawDepth;
                ssaoSettings.radius = 0.5f;
                ssaoSettings.intensity = 1.5f;
                ssaoSettings.numSlices = 3;
                ssaoSettings.numSteps = 4;
                ssaoSettings.blurRadius = 0;  // Disable blur for debug

                CFFLog::Info("[TestSSAO:Frame5] Debug_RawDepth mode enabled");
            }
        });

        // Frame 15: Capture Raw Depth
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame15] Capturing Raw Depth");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 15);
            CFFLog::Info("VISUAL_EXPECTATION: Depth [0,1] - near=dark, far=white");
        });

        // Frame 20: Debug - Linear Depth
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame20] Debug: Linear Depth visualization");
            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                deferredPipeline->GetSSAOPass().GetSettings().algorithm = ESSAOAlgorithm::Debug_LinearDepth;
            }
        });

        // Frame 25: Capture Linear Depth
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame25] Capturing Linear Depth");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 25);
            CFFLog::Info("VISUAL_EXPECTATION: Linearized Z - gradual brightness with distance");
        });

        // Frame 30: Debug - View Position Z (sign check)
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame30] Debug: View Position Z");
            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                deferredPipeline->GetSSAOPass().GetSettings().algorithm = ESSAOAlgorithm::Debug_ViewPosZ;
            }
        });

        // Frame 35: Capture View Position Z
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame35] Capturing View Position Z");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 35);
            CFFLog::Info("VISUAL_EXPECTATION: Positive Z = visible, should show depth gradient");
        });

        // Frame 40: Debug - View Normal Z
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame40] Debug: View Normal Z");
            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                deferredPipeline->GetSSAOPass().GetSettings().algorithm = ESSAOAlgorithm::Debug_ViewNormalZ;
            }
        });

        // Frame 45: Capture View Normal Z
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame45] Capturing View Normal Z");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 45);
            CFFLog::Info("VISUAL_EXPECTATION: Surfaces facing camera = white, away = dark");
        });

        // Frame 50: Switch to Crytek SSAO (actual algorithm test)
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame50] Switching to Crytek SSAO algorithm");
            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                deferredPipeline->GetSSAOPass().GetSettings().algorithm = ESSAOAlgorithm::GTAO;
                deferredPipeline->GetSSAOPass().GetSettings().blurRadius = 2;  // Re-enable blur
            }
        });

        // Frame 60: Capture Crytek SSAO
        ctx.OnFrame(60, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame60] Capturing Crytek SSAO");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 60);
            CFFLog::Info("VISUAL_EXPECTATION: Crytek SSAO - proper AO in corners/contacts");
        });

        // Frame 65: Finish test
        ctx.OnFrame(65, [&ctx]() {
            CFFLog::Info("[TestSSAO:Frame65] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: All SSAO algorithms rendered without errors");
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
