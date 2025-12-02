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

class CTestClusteredLighting : public ITestCase {
public:
    const char* GetName() const override {
        return "TestClusteredLighting";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create night scene with multiple point lights
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("Frame 1: Creating night scene with point lights");

            // Clear existing scene
            auto& scene = CScene::Instance();
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            // Set IBL intensity to very low (simulate night sky)
            auto* dirLight = scene.GetWorld().Create("DirectionalLight");
            auto* dirLightComp = dirLight->AddComponent<SDirectionalLight>();
            dirLightComp->intensity = 0.05f;  // Very dim directional light
            dirLightComp->ibl_intensity = 0.1f;  // Very low ambient light (night)

            // Create a ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundTransform = ground->AddComponent<STransform>();
            groundTransform->position = {5.0f, -1.0f, 0.0f};
            groundTransform->scale = {10.0f, 0.2f, 10.0f};
            auto* groundMesh = ground->AddComponent<SMeshRenderer>();
            groundMesh->path = "mesh/cube.obj";

            // Create several cubes
            for (int i = 0; i < 5; i++) {
                for (int j = 0; j < 3; j++) {
                    auto* cube = scene.GetWorld().Create("Cube");
                    auto* transform = cube->AddComponent<STransform>();
                    transform->position = {
                        2.0f + i * 2.0f,
                        0.0f + j * 2.0f,
                        (i % 2) * 2.0f - 1.0f
                    };
                    transform->scale = {0.8f, 0.8f, 0.8f};

                    auto* meshRenderer = cube->AddComponent<SMeshRenderer>();
                    meshRenderer->path = "mesh/cube.obj";
                }
            }

            // Create N point lights with different colors
            struct LightConfig {
                XMFLOAT3 position;
                XMFLOAT3 color;
                float intensity;
                float range;
            };

            LightConfig lights[] = {
                // Red light (left front)
                {{3.0f, 2.0f, -3.0f}, {1.0f, 0.2f, 0.2f}, 5.0f, 8.0f},
                // Green light (center front)
                {{7.0f, 2.0f, -3.0f}, {0.2f, 1.0f, 0.2f}, 5.0f, 8.0f},
                // Blue light (right front)
                {{11.0f, 2.0f, -3.0f}, {0.2f, 0.2f, 1.0f}, 5.0f, 8.0f},
                // Yellow light (left back)
                {{3.0f, 2.0f, 3.0f}, {1.0f, 1.0f, 0.2f}, 5.0f, 8.0f},
                // Cyan light (center back)
                {{7.0f, 2.0f, 3.0f}, {0.2f, 1.0f, 1.0f}, 5.0f, 8.0f},
                // Magenta light (right back)
                {{11.0f, 2.0f, 3.0f}, {1.0f, 0.2f, 1.0f}, 5.0f, 8.0f},
                // White light (center top)
                {{7.0f, 5.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 8.0f, 12.0f},
                // Orange light (moving around)
                {{5.0f, 1.0f, 2.0f}, {1.0f, 0.5f, 0.0f}, 4.0f, 6.0f},
            };

            for (size_t i = 0; i < sizeof(lights) / sizeof(lights[0]); i++) {
                auto* lightObj = scene.GetWorld().Create("PointLight");
                auto* transform = lightObj->AddComponent<STransform>();
                transform->position = lights[i].position;

                auto* light = lightObj->AddComponent<SPointLight>();
                light->color = lights[i].color;
                light->intensity = lights[i].intensity;
                light->range = lights[i].range;
            }

            CFFLog::Info("Created %zu point lights in night scene", sizeof(lights) / sizeof(lights[0]));
            CFFLog::Info("Total objects in scene: %d", scene.GetWorld().Count());

            // Setup camera to view the scene
            // Cubes are at X=(2-11), Y=(0-4), Z=(-1 to 1)
            // Position camera to view from front-left, looking at center of scene
            scene.GetEditorCamera().position = { -2.0f, 5.0f, -8.0f };
            scene.GetEditorCamera().SetLookAt(
                { -2.0f, 5.0f, -8.0f },  // eye
                { 7.0f, 1.0f, 0.0f },    // target (center of cubes)
                { 0.0f, 1.0f, 0.0f }     // up
            );
        });

        // Frame 10: Verify scene setup
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("Frame 10: Verifying scene setup");

            auto& scene = CScene::Instance();

            // Count lights
            int pointLightCount = 0;
            for (auto& obj : scene.GetWorld().Objects()) {
                if (obj->GetComponent<SPointLight>()) {
                    pointLightCount++;
                }
            }

            ASSERT_EQUAL(ctx, pointLightCount, 8, "Should have 8 point lights");
            CFFLog::Info("✓ Frame 10: Found %d point lights", pointLightCount);

            // Generate scene report
            std::string report = scene.GenerateReport();
            CFFLog::Info("Scene State:\n%s", report.c_str());
        });

        // Frame 20: Capture screenshot and verify visual output
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("Frame 20: Capturing screenshot");

            // Capture screenshot
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            // Visual expectations (for AI analysis)
            CFFLog::Info("VISUAL_EXPECTATION: Multiple colored lights illuminating cubes in dark scene");
            CFFLog::Info("VISUAL_EXPECTATION: Red light (left front), Green (center front), Blue (right front)");
            CFFLog::Info("VISUAL_EXPECTATION: Yellow light (left back), Cyan (center back), Magenta (right back)");
            CFFLog::Info("VISUAL_EXPECTATION: White light (center top) providing overall illumination");
            CFFLog::Info("VISUAL_EXPECTATION: Orange light visible near ground");
            CFFLog::Info("VISUAL_EXPECTATION: Dark background (low IBL intensity simulating night)");
            CFFLog::Info("VISUAL_EXPECTATION: Cubes show color bleeding from nearby point lights");
            CFFLog::Info("VISUAL_EXPECTATION: Smooth falloff of light intensity with distance");

            CFFLog::Info("✓ Frame 20: Screenshot captured");
        });

        // Frame 30: Finalize test
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("Frame 30: Finalizing test");

            if (ctx.failures.empty()) {
                CFFLog::Info("✓ ALL ASSERTIONS PASSED");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("✗ TEST FAILED with %zu assertion failures", ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
                ctx.testPassed = false;
            }

            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestClusteredLighting)
