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
 * Test: Bloom Post-Processing Effect
 *
 * Purpose:
 *   Verify that the HDR bloom post-processing effect works correctly.
 *   Tests threshold extraction, blur chain, and compositing.
 *
 * Expected Results:
 *   - Bright areas produce a soft glow effect
 *   - Bloom intensity and threshold controls work as expected
 *   - No visual artifacts or crashes
 */
class CTestBloom : public ITestCase {
public:
    const char* GetName() const override {
        return "TestBloom";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create scene with bright light for bloom testing
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Create a very bright directional light (will trigger bloom)
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 5.0f;  // Very bright to trigger bloom

            // Create test spheres with default material (bright specular will bloom)
            auto* sphere1 = scene.GetWorld().Create("Sphere1");
            auto* sphere1Transform = sphere1->AddComponent<STransform>();
            sphere1Transform->position = XMFLOAT3(0.0f, 1.0f, 3.0f);
            sphere1Transform->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* sphere1Mesh = sphere1->AddComponent<SMeshRenderer>();
            sphere1Mesh->path = "mesh/sphere.obj";

            // Second sphere
            auto* sphere2 = scene.GetWorld().Create("Sphere2");
            auto* sphere2Transform = sphere2->AddComponent<STransform>();
            sphere2Transform->position = XMFLOAT3(-1.5f, 0.5f, 4.0f);
            sphere2Transform->scale = XMFLOAT3(0.4f, 0.4f, 0.4f);
            auto* sphere2Mesh = sphere2->AddComponent<SMeshRenderer>();
            sphere2Mesh->path = "mesh/sphere.obj";

            // Create a cube
            auto* cube = scene.GetWorld().Create("TestCube");
            auto* cubeTransform = cube->AddComponent<STransform>();
            cubeTransform->position = XMFLOAT3(1.5f, 0.5f, 4.0f);
            cubeTransform->scale = XMFLOAT3(0.4f, 0.4f, 0.4f);
            auto* cubeMesh = cube->AddComponent<SMeshRenderer>();
            cubeMesh->path = "mesh/cube.obj";

            // Create ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundTransform = ground->AddComponent<STransform>();
            groundTransform->position = XMFLOAT3(0.0f, -0.5f, 5.0f);
            groundTransform->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* groundMesh = ground->AddComponent<SMeshRenderer>();
            groundMesh->path = "mesh/cube.obj";

            CFFLog::Info("[TestBloom:Frame1] Scene created");
        });

        // Frame 5: Configure bloom and enable
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame5] Configuring bloom settings");

            auto& settings = CScene::Instance().GetLightSettings();
            CEditorContext::Instance().GetShowFlags().Bloom = true;
            settings.bloom.threshold = 1.0f;
            settings.bloom.intensity = 1.5f;
            settings.bloom.scatter = 0.7f;

            CFFLog::Info("[TestBloom:Frame5] Bloom enabled: threshold=%.2f, intensity=%.2f, scatter=%.2f",
                        settings.bloom.threshold, settings.bloom.intensity, settings.bloom.scatter);
        });

        // Frame 20: Capture screenshot with bloom enabled
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame20] Capturing screenshot with bloom enabled");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Bright specular highlights should have visible glow/bloom effect");
        });

        // Frame 25: Disable bloom for comparison
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame25] Disabling bloom for comparison");

            CEditorContext::Instance().GetShowFlags().Bloom = false;
        });

        // Frame 30: Capture screenshot without bloom
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame30] Capturing screenshot without bloom");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 30);

            CFFLog::Info("VISUAL_EXPECTATION: Same scene without bloom glow effect");
        });

        // Frame 35: Test high intensity bloom
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame35] Testing high intensity bloom");

            auto& settings = CScene::Instance().GetLightSettings();
            CEditorContext::Instance().GetShowFlags().Bloom = true;
            settings.bloom.threshold = 0.5f;  // Lower threshold = more bloom
            settings.bloom.intensity = 2.5f;  // Higher intensity
            settings.bloom.scatter = 0.9f;    // More diffuse
        });

        // Frame 40: Capture high intensity bloom
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame40] Capturing high intensity bloom");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 40);

            CFFLog::Info("VISUAL_EXPECTATION: Much stronger bloom effect with larger halos");
        });

        // Frame 45: Finish test
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestBloom:Frame45] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Bloom rendering completed without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestBloom)
