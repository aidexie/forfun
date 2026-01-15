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
 * Test: Color Grading Post-Processing Effect
 *
 * Purpose:
 *   Verify that the color grading post-processing effect works correctly.
 *   Tests Lift/Gamma/Gain controls, saturation, contrast, temperature,
 *   and preset switching.
 *
 * Expected Results:
 *   - Neutral preset produces no visible change
 *   - Warm preset adds orange tint
 *   - Cool preset adds blue tint
 *   - Cinematic preset adds contrast and teal/orange look
 *   - Lift/Gamma/Gain controls affect shadows/midtones/highlights
 *   - No visual artifacts or crashes
 */
class CTestColorGrading : public ITestCase {
public:
    const char* GetName() const override {
        return "TestColorGrading";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Create directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 3.0f;

            // Create colorful test spheres
            // Red sphere
            auto* redSphere = scene.GetWorld().Create("RedSphere");
            auto* redTransform = redSphere->AddComponent<STransform>();
            redTransform->position = XMFLOAT3(-2.0f, 1.0f, 4.0f);
            redTransform->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* redMesh = redSphere->AddComponent<SMeshRenderer>();
            redMesh->path = "mesh/sphere.obj";

            // Green sphere
            auto* greenSphere = scene.GetWorld().Create("GreenSphere");
            auto* greenTransform = greenSphere->AddComponent<STransform>();
            greenTransform->position = XMFLOAT3(0.0f, 1.0f, 4.0f);
            greenTransform->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* greenMesh = greenSphere->AddComponent<SMeshRenderer>();
            greenMesh->path = "mesh/sphere.obj";

            // Blue sphere
            auto* blueSphere = scene.GetWorld().Create("BlueSphere");
            auto* blueTransform = blueSphere->AddComponent<STransform>();
            blueTransform->position = XMFLOAT3(2.0f, 1.0f, 4.0f);
            blueTransform->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* blueMesh = blueSphere->AddComponent<SMeshRenderer>();
            blueMesh->path = "mesh/sphere.obj";

            // Ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundTransform = ground->AddComponent<STransform>();
            groundTransform->position = XMFLOAT3(0.0f, -0.5f, 5.0f);
            groundTransform->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* groundMesh = ground->AddComponent<SMeshRenderer>();
            groundMesh->path = "mesh/cube.obj";

            CFFLog::Info("[TestColorGrading:Frame1] Scene created");
        });

        // Frame 5: Enable color grading with Neutral preset (baseline)
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame5] Enabling color grading with Neutral preset");

            auto& settings = CScene::Instance().GetLightSettings();
            CEditorContext::Instance().GetShowFlags().ColorGrading = true;
            settings.colorGrading.ApplyPreset(EColorGradingPreset::Neutral);

            CFFLog::Info("[TestColorGrading:Frame5] Color grading enabled (Neutral)");
        });

        // Frame 15: Capture baseline screenshot (Neutral)
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame15] Capturing Neutral preset screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 15);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should look normal with no color grading applied");
        });

        // Frame 20: Switch to Warm preset
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame20] Switching to Warm preset");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.colorGrading.ApplyPreset(EColorGradingPreset::Warm);

            CFFLog::Info("[TestColorGrading:Frame20] Warm preset applied: temp=%.2f, sat=%.2f",
                        settings.colorGrading.temperature, settings.colorGrading.saturation);
        });

        // Frame 25: Capture Warm preset screenshot
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame25] Capturing Warm preset screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 25);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should have warm orange tint");
        });

        // Frame 30: Switch to Cool preset
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame30] Switching to Cool preset");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.colorGrading.ApplyPreset(EColorGradingPreset::Cool);

            CFFLog::Info("[TestColorGrading:Frame30] Cool preset applied: temp=%.2f, contrast=%.2f",
                        settings.colorGrading.temperature, settings.colorGrading.contrast);
        });

        // Frame 35: Capture Cool preset screenshot
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame35] Capturing Cool preset screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 35);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should have cool blue tint");
        });

        // Frame 40: Switch to Cinematic preset
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame40] Switching to Cinematic preset");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.colorGrading.ApplyPreset(EColorGradingPreset::Cinematic);

            CFFLog::Info("[TestColorGrading:Frame40] Cinematic preset applied: contrast=%.2f, sat=%.2f",
                        settings.colorGrading.contrast, settings.colorGrading.saturation);
        });

        // Frame 45: Capture Cinematic preset screenshot
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame45] Capturing Cinematic preset screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 45);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should have high contrast, teal/orange look");
        });

        // Frame 50: Test extreme Lift/Gamma/Gain values
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame50] Testing extreme Lift/Gamma/Gain values");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.colorGrading.preset = EColorGradingPreset::Custom;
            settings.colorGrading.lift = {0.1f, 0.0f, -0.1f};   // Red shadows, blue reduction
            settings.colorGrading.gamma = {-0.2f, 0.0f, 0.2f};  // Darker red midtones, brighter blue
            settings.colorGrading.gain = {0.2f, 0.1f, 0.0f};    // Brighter red/green highlights
            settings.colorGrading.saturation = 0.0f;
            settings.colorGrading.contrast = 0.0f;
            settings.colorGrading.temperature = 0.0f;

            CFFLog::Info("[TestColorGrading:Frame50] Custom LGG applied");
        });

        // Frame 55: Capture custom LGG screenshot
        ctx.OnFrame(55, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame55] Capturing custom LGG screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 55);

            CFFLog::Info("VISUAL_EXPECTATION: Visible color shift from Lift/Gamma/Gain adjustments");
        });

        // Frame 60: Test saturation and contrast extremes
        ctx.OnFrame(60, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame60] Testing saturation and contrast extremes");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.colorGrading.preset = EColorGradingPreset::Custom;
            settings.colorGrading.lift = settings.colorGrading.gamma = settings.colorGrading.gain = {0, 0, 0};
            settings.colorGrading.saturation = 0.8f;   // High saturation
            settings.colorGrading.contrast = 0.5f;     // High contrast
            settings.colorGrading.temperature = 0.0f;

            CFFLog::Info("[TestColorGrading:Frame60] High saturation (%.2f) and contrast (%.2f)",
                        settings.colorGrading.saturation, settings.colorGrading.contrast);
        });

        // Frame 65: Capture high saturation/contrast screenshot
        ctx.OnFrame(65, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame65] Capturing high saturation/contrast screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 65);

            CFFLog::Info("VISUAL_EXPECTATION: Very saturated colors with high contrast");
        });

        // Frame 70: Disable color grading for comparison
        ctx.OnFrame(70, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame70] Disabling color grading");

            CEditorContext::Instance().GetShowFlags().ColorGrading = false;
        });

        // Frame 75: Capture disabled screenshot
        ctx.OnFrame(75, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame75] Capturing screenshot with color grading disabled");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 75);

            CFFLog::Info("VISUAL_EXPECTATION: Scene should look normal (no color grading)");
        });

        // Frame 80: Finish test
        ctx.OnFrame(80, [&ctx]() {
            CFFLog::Info("[TestColorGrading:Frame80] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Color grading rendering completed without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestColorGrading)
