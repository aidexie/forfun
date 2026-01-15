#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Editor/EditorContext.h"
#include "Engine/Scene.h"
#include "Engine/SceneLightSettings.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Auto Exposure (Eye Adaptation)
 *
 * Purpose:
 *   Verify that the histogram-based auto exposure feature works correctly.
 *   Tests exposure adaptation from bright to dark scenes and vice versa.
 *
 * Expected Results:
 *   - Exposure adjusts automatically based on scene luminance
 *   - Bright scenes result in lower exposure (darker output)
 *   - Dark scenes result in higher exposure (brighter output)
 *   - Adaptation is smooth over time
 */
class CTestAutoExposure : public ITestCase {
public:
    const char* GetName() const override {
        return "TestAutoExposure";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create scene with moderate lighting
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Create directional light with moderate intensity
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 3.0f;

            // Create test spheres
            auto* sphere1 = scene.GetWorld().Create("Sphere1");
            auto* sphere1Transform = sphere1->AddComponent<STransform>();
            sphere1Transform->position = XMFLOAT3(0.0f, 1.0f, 3.0f);
            sphere1Transform->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* sphere1Mesh = sphere1->AddComponent<SMeshRenderer>();
            sphere1Mesh->path = "mesh/sphere.obj";

            // Create ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundTransform = ground->AddComponent<STransform>();
            groundTransform->position = XMFLOAT3(0.0f, -0.5f, 5.0f);
            groundTransform->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* groundMesh = ground->AddComponent<SMeshRenderer>();
            groundMesh->path = "mesh/cube.obj";

            CFFLog::Info("[TestAutoExposure:Frame1] Scene created");
        });

        // Frame 5: Enable auto exposure with default settings
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame5] Enabling auto exposure");

            auto& settings = CScene::Instance().GetLightSettings();
            CEditorContext::Instance().GetShowFlags().AutoExposure = true;

            // Configure auto exposure settings
            settings.autoExposure.minEV = -4.0f;
            settings.autoExposure.maxEV = 4.0f;
            settings.autoExposure.adaptSpeedUp = 1.0f;
            settings.autoExposure.adaptSpeedDown = 1.5f;
            settings.autoExposure.exposureCompensation = 0.0f;
            settings.autoExposure.centerWeight = 0.5f;

            CFFLog::Info("[TestAutoExposure:Frame5] Auto exposure enabled: minEV=%.1f, maxEV=%.1f",
                        settings.autoExposure.minEV, settings.autoExposure.maxEV);
        });

        // Frame 20: Capture screenshot with auto exposure (moderate scene)
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame20] Capturing screenshot with moderate lighting");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should be properly exposed with balanced brightness");
        });

        // Frame 25: Make scene very bright
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame25] Increasing light intensity (bright scene)");

            auto& scene = CScene::Instance();
            for (auto& objPtr : scene.GetWorld().Objects()) {
                auto* dirLight = objPtr->GetComponent<SDirectionalLight>();
                if (dirLight) {
                    dirLight->intensity = 15.0f;  // Very bright
                    CFFLog::Info("[TestAutoExposure:Frame25] Light intensity set to %.1f", dirLight->intensity);
                    break;
                }
            }
        });

        // Frame 45: Capture screenshot after adaptation to bright scene
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame45] Capturing screenshot after bright adaptation");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 45);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should still be properly exposed (auto exposure compensated for brightness)");
        });

        // Frame 50: Make scene very dark
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame50] Decreasing light intensity (dark scene)");

            auto& scene = CScene::Instance();
            for (auto& objPtr : scene.GetWorld().Objects()) {
                auto* dirLight = objPtr->GetComponent<SDirectionalLight>();
                if (dirLight) {
                    dirLight->intensity = 0.5f;  // Very dark
                    CFFLog::Info("[TestAutoExposure:Frame50] Light intensity set to %.1f", dirLight->intensity);
                    break;
                }
            }
        });

        // Frame 70: Capture screenshot after adaptation to dark scene
        ctx.OnFrame(70, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame70] Capturing screenshot after dark adaptation");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 70);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should still be visible (auto exposure boosted brightness)");
        });

        // Frame 75: Disable auto exposure for comparison
        ctx.OnFrame(75, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame75] Disabling auto exposure for comparison");
            CEditorContext::Instance().GetShowFlags().AutoExposure = false;
        });

        // Frame 80: Capture screenshot without auto exposure (dark scene)
        ctx.OnFrame(80, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame80] Capturing screenshot without auto exposure");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 80);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should be very dark without auto exposure compensation");
        });

        // Frame 85: Finish test
        ctx.OnFrame(85, [&ctx]() {
            CFFLog::Info("[TestAutoExposure:Frame85] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Auto exposure rendering completed without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestAutoExposure)
