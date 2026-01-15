#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Editor/EditorContext.h"
#include "Engine/Scene.h"
#include "Engine/SceneLightSettings.h"
#include "Engine/Camera.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Motion Blur Post-Processing Effect
 *
 * Purpose:
 *   Verify that the camera motion blur post-processing effect works correctly.
 *   Tests velocity-based blur along camera movement direction.
 *
 * Expected Results:
 *   - Camera rotation produces visible motion blur
 *   - Intensity and sample count controls work as expected
 *   - No visual artifacts or crashes
 *   - Static camera produces no blur
 */
class CTestMotionBlur : public ITestCase {
public:
    const char* GetName() const override {
        return "TestMotionBlur";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create scene for motion blur testing
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Create directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 3.0f;

            // Create test spheres at various positions
            for (int i = 0; i < 5; ++i) {
                auto* sphere = scene.GetWorld().Create(("Sphere" + std::to_string(i)).c_str());
                auto* transform = sphere->AddComponent<STransform>();
                transform->position = XMFLOAT3(-2.0f + i * 1.0f, 0.5f, 4.0f + i * 0.5f);
                transform->scale = XMFLOAT3(0.4f, 0.4f, 0.4f);
                auto* mesh = sphere->AddComponent<SMeshRenderer>();
                mesh->path = "mesh/sphere.obj";
            }

            // Create cubes for visual reference
            for (int i = 0; i < 3; ++i) {
                auto* cube = scene.GetWorld().Create(("Cube" + std::to_string(i)).c_str());
                auto* transform = cube->AddComponent<STransform>();
                transform->position = XMFLOAT3(-1.5f + i * 1.5f, 1.5f, 5.0f);
                transform->scale = XMFLOAT3(0.3f, 0.3f, 0.3f);
                auto* mesh = cube->AddComponent<SMeshRenderer>();
                mesh->path = "mesh/cube.obj";
            }

            // Create ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundTransform = ground->AddComponent<STransform>();
            groundTransform->position = XMFLOAT3(0.0f, -0.5f, 5.0f);
            groundTransform->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* groundMesh = ground->AddComponent<SMeshRenderer>();
            groundMesh->path = "mesh/cube.obj";

            CFFLog::Info("[TestMotionBlur:Frame1] Scene created with multiple objects");
        });

        // Frame 5: Capture static scene (no motion blur)
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame5] Capturing static scene without motion blur");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 5);

            CFFLog::Info("VISUAL_EXPECTATION: Static scene with no blur effect");
        });

        // Frame 10: Enable motion blur and start camera rotation
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame10] Enabling motion blur");

            auto& settings = CScene::Instance().GetLightSettings();
            CEditorContext::Instance().GetShowFlags().MotionBlur = true;
            settings.motionBlur.intensity = 0.8f;
            settings.motionBlur.sampleCount = 12;
            settings.motionBlur.maxBlurPixels = 32.0f;

            CFFLog::Info("[TestMotionBlur:Frame10] Motion blur enabled: intensity=%.2f, samples=%d, maxBlur=%.0f",
                        settings.motionBlur.intensity, settings.motionBlur.sampleCount, settings.motionBlur.maxBlurPixels);
        });

        // Frame 11-20: Rotate camera to generate velocity
        for (int frame = 11; frame <= 20; ++frame) {
            ctx.OnFrame(frame, [frame, &ctx]() {
                // Rotate camera to generate motion vectors
                float rotationSpeed = 0.035f;  // radians per frame (~2 degrees)

                CCamera& camera = CScene::Instance().GetEditorCamera();
                camera.Rotate(rotationSpeed, 0.0f);  // Rotate yaw only

                if (frame == 15) {
                    CFFLog::Info("[TestMotionBlur:Frame15] Camera rotating - capturing motion blur");
                    CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 15);
                    CFFLog::Info("VISUAL_EXPECTATION: Visible horizontal motion blur from camera rotation");
                }

                if (frame == 20) {
                    CFFLog::Info("[TestMotionBlur:Frame20] Capturing end of rotation");
                    CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);
                }
            });
        }

        // Frame 25: Test high intensity motion blur
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame25] Testing high intensity motion blur");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.motionBlur.intensity = 1.0f;
            settings.motionBlur.sampleCount = 16;
            settings.motionBlur.maxBlurPixels = 64.0f;

            // Quick camera rotation for strong blur
            CCamera& camera = CScene::Instance().GetEditorCamera();
            camera.Rotate(0.087f, 0.0f);  // ~5 degrees in radians
        });

        // Frame 26: Capture high intensity blur
        ctx.OnFrame(26, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame26] Capturing high intensity motion blur");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 26);

            CFFLog::Info("VISUAL_EXPECTATION: Strong motion blur with longer trails");
        });

        // Frame 30: Test low intensity motion blur
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame30] Testing low intensity motion blur");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.motionBlur.intensity = 0.3f;
            settings.motionBlur.sampleCount = 8;
            settings.motionBlur.maxBlurPixels = 16.0f;

            // Quick camera rotation
            CCamera& camera = CScene::Instance().GetEditorCamera();
            camera.Rotate(0.052f, 0.0f);  // ~3 degrees in radians
        });

        // Frame 31: Capture low intensity blur
        ctx.OnFrame(31, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame31] Capturing low intensity motion blur");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 31);

            CFFLog::Info("VISUAL_EXPECTATION: Subtle motion blur effect");
        });

        // Frame 35: Disable motion blur for comparison
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame35] Disabling motion blur");
            CEditorContext::Instance().GetShowFlags().MotionBlur = false;

            // Quick camera rotation (should show no blur)
            CCamera& camera = CScene::Instance().GetEditorCamera();
            camera.Rotate(0.052f, 0.0f);  // ~3 degrees in radians
        });

        // Frame 36: Capture without motion blur
        ctx.OnFrame(36, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame36] Capturing scene without motion blur");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 36);

            CFFLog::Info("VISUAL_EXPECTATION: Sharp image with no motion blur despite camera movement");
        });

        // Frame 40: Finish test
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestMotionBlur:Frame40] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Motion blur rendering completed without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestMotionBlur)
