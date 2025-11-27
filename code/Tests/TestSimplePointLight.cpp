#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/PointLight.h"
#include "Engine/Components/DirectionalLight.h"
#include <DirectXMath.h>

using namespace DirectX;

class CTestSimplePointLight : public ITestCase {
public:
    const char* GetName() const override {
        return "TestSimplePointLight";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create minimal scene: 1 cube + 1 huge white point light
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("Frame 1: Creating simple point light test scene");

            // Clear existing scene
            auto& scene = CScene::Instance();
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            // Disable directional light (set to 0)
            auto* dirLight = scene.GetWorld().Create("DirectionalLight");
            auto* dirLightComp = dirLight->AddComponent<SDirectionalLight>();
            dirLightComp->intensity = 0.0f;
            dirLightComp->ibl_intensity = 0.0f;  // No ambient

            // Create ONE cube in front of camera
            auto* cube = scene.GetWorld().Create("Cube");
            auto* transform = cube->AddComponent<STransform>();
            transform->position = {5.0f, 0.8f, 0.0f};  // Camera looks at +X
            transform->scale = {1.0f, 1.0f, 1.0f};
            auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
            meshRenderer->path = "mesh/cube.obj";

            // Create ONE HUGE white point light right next to the cube
            auto* lightObj = scene.GetWorld().Create("PointLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->position = {5.0f, 0.8f, 3.0f};  // 3 units away from cube

            auto* light = lightObj->AddComponent<SPointLight>();
            light->color = {1.0f, 1.0f, 1.0f};  // Pure white
            light->intensity = 50.0f;  // VERY high intensity
            light->range = 20.0f;  // Large range

            CFFLog::Info("Created 1 cube + 1 huge white point light (intensity=50, range=20)");
        });

        // Frame 20: Capture screenshot
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("Frame 20: Capturing screenshot");
            CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Cube should be BRIGHT WHITE from point light");
            CFFLog::Info("VISUAL_EXPECTATION: Black background (no IBL)");
            CFFLog::Info("VISUAL_EXPECTATION: If cube is black, point light system is NOT working");

            CFFLog::Info("✓ Frame 20: Screenshot captured");
        });

        // Frame 30: Finalize
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("Frame 30: Finalizing test");
            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestSimplePointLight)
