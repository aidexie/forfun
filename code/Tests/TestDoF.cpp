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
 * Test: Depth of Field Post-Processing Effect
 *
 * Purpose:
 *   Verify that the depth of field post-processing effect works correctly.
 *   Tests near/far blur separation, focus distance control, and aperture settings.
 *
 * Expected Results:
 *   - Objects at focus distance appear sharp
 *   - Objects closer (near field) are blurred
 *   - Objects farther (far field) are blurred
 *   - Aperture control affects blur intensity
 *   - No visual artifacts or foreground bleeding
 */
class CTestDoF : public ITestCase {
public:
    const char* GetName() const override {
        return "TestDoF";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create scene with objects at different depths
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame1] Setting up test scene with depth variation");

            auto& scene = CScene::Instance();

            // Create directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 3.0f;

            // Near object (will be blurred when focusing on mid-ground)
            auto* nearSphere = scene.GetWorld().Create("NearSphere");
            auto* nearTransform = nearSphere->AddComponent<STransform>();
            nearTransform->position = XMFLOAT3(-1.0f, 0.5f, 2.0f);  // Close to camera
            nearTransform->scale = XMFLOAT3(0.4f, 0.4f, 0.4f);
            auto* nearMesh = nearSphere->AddComponent<SMeshRenderer>();
            nearMesh->path = "mesh/sphere.obj";

            // Mid-ground object (focus target)
            auto* midSphere = scene.GetWorld().Create("MidSphere");
            auto* midTransform = midSphere->AddComponent<STransform>();
            midTransform->position = XMFLOAT3(0.0f, 0.5f, 5.0f);  // Mid-distance
            midTransform->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* midMesh = midSphere->AddComponent<SMeshRenderer>();
            midMesh->path = "mesh/sphere.obj";

            // Far object (will be blurred when focusing on mid-ground)
            auto* farSphere = scene.GetWorld().Create("FarSphere");
            auto* farTransform = farSphere->AddComponent<STransform>();
            farTransform->position = XMFLOAT3(1.0f, 0.5f, 12.0f);  // Far from camera
            farTransform->scale = XMFLOAT3(0.6f, 0.6f, 0.6f);
            auto* farMesh = farSphere->AddComponent<SMeshRenderer>();
            farMesh->path = "mesh/sphere.obj";

            // Create cubes at various depths for better depth perception
            for (int i = 0; i < 5; ++i) {
                auto* cube = scene.GetWorld().Create(("Cube" + std::to_string(i)).c_str());
                auto* cubeTransform = cube->AddComponent<STransform>();
                cubeTransform->position = XMFLOAT3(-2.0f + i * 1.0f, 0.0f, 3.0f + i * 2.0f);
                cubeTransform->scale = XMFLOAT3(0.3f, 0.3f, 0.3f);
                auto* cubeMesh = cube->AddComponent<SMeshRenderer>();
                cubeMesh->path = "mesh/cube.obj";
            }

            // Ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundTransform = ground->AddComponent<STransform>();
            groundTransform->position = XMFLOAT3(0.0f, -0.5f, 8.0f);
            groundTransform->scale = XMFLOAT3(15.0f, 0.1f, 20.0f);
            auto* groundMesh = ground->AddComponent<SMeshRenderer>();
            groundMesh->path = "mesh/cube.obj";

            CFFLog::Info("[TestDoF:Frame1] Scene created with objects at depths: 2m, 5m, 12m");
        });

        // Frame 5: Enable DoF with focus on mid-ground
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame5] Enabling DoF, focus on mid-ground (5m)");

            auto& settings = CScene::Instance().GetLightSettings();
            CEditorContext::Instance().GetShowFlags().DepthOfField = true;

            settings.depthOfField.focusDistance = 5.0f;   // Focus on mid-sphere
            settings.depthOfField.focalRange = 2.0f;      // Moderate in-focus range
            settings.depthOfField.aperture = 2.8f;        // f/2.8 for visible blur
            settings.depthOfField.maxBlurRadius = 8.0f;   // Moderate blur

            CFFLog::Info("[TestDoF:Frame5] DoF settings: focus=%.1fm, range=%.1fm, f/%.1f, blur=%.0fpx",
                        settings.depthOfField.focusDistance, settings.depthOfField.focalRange,
                        settings.depthOfField.aperture, settings.depthOfField.maxBlurRadius);
        });

        // Frame 20: Capture screenshot with mid-ground focus
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame20] Capturing screenshot - focus on mid-ground");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Mid sphere (5m) sharp, near sphere (2m) blurred, far sphere (12m) blurred");
        });

        // Frame 25: Change focus to near object
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame25] Changing focus to near object (2m)");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.depthOfField.focusDistance = 2.0f;   // Focus on near sphere
            settings.depthOfField.focalRange = 1.0f;      // Tight focus
        });

        // Frame 35: Capture screenshot with near focus
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame35] Capturing screenshot - focus on near object");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 35);

            CFFLog::Info("VISUAL_EXPECTATION: Near sphere (2m) sharp, mid and far spheres blurred");
        });

        // Frame 40: Test wide aperture (strong blur)
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame40] Testing wide aperture (f/1.4 - strong blur)");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.depthOfField.focusDistance = 5.0f;
            settings.depthOfField.focalRange = 1.0f;
            settings.depthOfField.aperture = 1.4f;        // Wide aperture = strong blur
            settings.depthOfField.maxBlurRadius = 12.0f;  // Larger blur radius
        });

        // Frame 50: Capture screenshot with wide aperture
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame50] Capturing screenshot - wide aperture");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 50);

            CFFLog::Info("VISUAL_EXPECTATION: Strong blur on out-of-focus areas, circular bokeh pattern");
        });

        // Frame 55: Test narrow aperture (minimal blur)
        ctx.OnFrame(55, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame55] Testing narrow aperture (f/16 - minimal blur)");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.depthOfField.aperture = 16.0f;  // Narrow aperture = almost no blur
        });

        // Frame 65: Capture screenshot with narrow aperture
        ctx.OnFrame(65, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame65] Capturing screenshot - narrow aperture");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 65);

            CFFLog::Info("VISUAL_EXPECTATION: Almost everything in focus (minimal DoF effect)");
        });

        // Frame 70: Disable DoF for comparison
        ctx.OnFrame(70, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame70] Disabling DoF for comparison");
            CEditorContext::Instance().GetShowFlags().DepthOfField = false;
        });

        // Frame 75: Capture screenshot without DoF
        ctx.OnFrame(75, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame75] Capturing screenshot - no DoF");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 75);

            CFFLog::Info("VISUAL_EXPECTATION: All objects equally sharp (no depth blur)");
        });

        // Frame 80: Finish test
        ctx.OnFrame(80, [&ctx]() {
            CFFLog::Info("[TestDoF:Frame80] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Depth of Field rendering completed without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestDoF)
