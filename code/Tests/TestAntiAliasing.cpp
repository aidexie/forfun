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
#include "Engine/Rendering/RenderPipeline.h"
#include "Engine/Rendering/Deferred/DeferredRenderPipeline.h"
#include "Engine/Camera.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Anti-Aliasing (FXAA and SMAA)
 *
 * Purpose:
 *   Verify that both anti-aliasing algorithms work correctly:
 *   - FXAA (Fast Approximate Anti-Aliasing)
 *   - SMAA (Subpixel Morphological Anti-Aliasing)
 *
 * Scene Setup:
 *   - High-contrast edges (black/white checkerboard pattern)
 *   - Diagonal lines and thin geometry
 *   - Objects at various angles to show aliasing artifacts
 *
 * Expected Results:
 *   - No AA: Visible jagged edges on diagonal lines
 *   - FXAA: Smoothed edges with slight blur
 *   - SMAA: Cleaner edges with better preservation of detail
 */
class CTestAntiAliasing : public ITestCase {
public:
    const char* GetName() const override {
        return "TestAntiAliasing";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene with high-contrast edges
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Set up camera
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({5.0f, 3.0f, -2.0f}, {0.0f, 0.5f, 0.0f});
            CFFLog::Info("[TestAntiAliasing:Frame1] Camera positioned");

            // Directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 1.0f, 1.0f);
            dirLight->intensity = 3.0f;

            // Floor
            auto* floor = scene.GetWorld().Create("Floor");
            auto* floorT = floor->AddComponent<STransform>();
            floorT->position = XMFLOAT3(0.0f, 0.0f, 0.0f);
            floorT->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* floorMesh = floor->AddComponent<SMeshRenderer>();
            floorMesh->path = "mesh/cube.obj";

            // Diagonal thin bars (good for testing AA on diagonal edges)
            for (int i = 0; i < 5; i++) {
                auto* bar = scene.GetWorld().Create(("DiagonalBar" + std::to_string(i)).c_str());
                auto* barT = bar->AddComponent<STransform>();
                barT->position = XMFLOAT3(-2.0f + i * 1.0f, 0.5f, 0.0f);
                barT->scale = XMFLOAT3(0.05f, 1.0f, 0.05f);
                barT->SetRotation(0.0f, 0.0f, 30.0f + i * 10.0f);  // Various angles
                auto* barMesh = bar->AddComponent<SMeshRenderer>();
                barMesh->path = "mesh/cube.obj";
            }

            // Sphere (curved edges)
            auto* sphere = scene.GetWorld().Create("Sphere");
            auto* sphereT = sphere->AddComponent<STransform>();
            sphereT->position = XMFLOAT3(2.0f, 0.5f, 2.0f);
            sphereT->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            auto* sphereMesh = sphere->AddComponent<SMeshRenderer>();
            sphereMesh->path = "mesh/sphere.obj";

            // Cube at angle (sharp edges)
            auto* cube = scene.GetWorld().Create("AngledCube");
            auto* cubeT = cube->AddComponent<STransform>();
            cubeT->position = XMFLOAT3(-2.0f, 0.5f, 2.0f);
            cubeT->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
            cubeT->SetRotation(0.0f, 45.0f, 0.0f);
            auto* cubeMesh = cube->AddComponent<SMeshRenderer>();
            cubeMesh->path = "mesh/cube.obj";

            // Thin horizontal lines at different heights
            for (int i = 0; i < 3; i++) {
                auto* line = scene.GetWorld().Create(("HorizontalLine" + std::to_string(i)).c_str());
                auto* lineT = line->AddComponent<STransform>();
                lineT->position = XMFLOAT3(0.0f, 0.2f + i * 0.3f, -2.0f);
                lineT->scale = XMFLOAT3(3.0f, 0.02f, 0.02f);
                auto* lineMesh = line->AddComponent<SMeshRenderer>();
                lineMesh->path = "mesh/cube.obj";
            }

            CFFLog::Info("[TestAntiAliasing:Frame1] Scene created with diagonal bars, sphere, and thin lines");
        });

        // Frame 5: Disable AA (baseline)
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame5] Disabling AA (baseline)");

            CEditorContext::Instance().GetShowFlags().AntiAliasing = false;

            auto& scene = CScene::Instance();
            scene.GetLightSettings().antiAliasing.mode = EAntiAliasingMode::Off;

            CFFLog::Info("[TestAntiAliasing:Frame5] AA disabled");
        });

        // Frame 15: Capture No AA
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame15] Capturing No AA baseline");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 15);
            CFFLog::Info("VISUAL_EXPECTATION: Visible jagged edges on diagonal lines and sphere silhouette");
        });

        // Frame 20: Enable FXAA
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame20] Enabling FXAA");

            CEditorContext::Instance().GetShowFlags().AntiAliasing = true;

            auto& scene = CScene::Instance();
            auto& aaSettings = scene.GetLightSettings().antiAliasing;
            aaSettings.mode = EAntiAliasingMode::FXAA;
            aaSettings.fxaaSubpixelQuality = 0.75f;
            aaSettings.fxaaEdgeThreshold = 0.166f;
            aaSettings.fxaaEdgeThresholdMin = 0.0833f;

            CFFLog::Info("[TestAntiAliasing:Frame20] FXAA enabled with default settings");
        });

        // Frame 30: Capture FXAA
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame30] Capturing FXAA");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 30);
            CFFLog::Info("VISUAL_EXPECTATION: Smoothed edges, slight blur on high-contrast areas");
        });

        // Frame 35: Enable SMAA
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame35] Enabling SMAA");

            auto& scene = CScene::Instance();
            scene.GetLightSettings().antiAliasing.mode = EAntiAliasingMode::SMAA;

            CFFLog::Info("[TestAntiAliasing:Frame35] SMAA enabled");
        });

        // Frame 45: Capture SMAA
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame45] Capturing SMAA");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 45);
            CFFLog::Info("VISUAL_EXPECTATION: Clean edges with better detail preservation than FXAA");
        });

        // Frame 50: Test FXAA with high subpixel quality
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame50] Testing FXAA with high subpixel quality");

            auto& scene = CScene::Instance();
            auto& aaSettings = scene.GetLightSettings().antiAliasing;
            aaSettings.mode = EAntiAliasingMode::FXAA;
            aaSettings.fxaaSubpixelQuality = 1.0f;  // Maximum subpixel AA

            CFFLog::Info("[TestAntiAliasing:Frame50] FXAA subpixel quality set to 1.0");
        });

        // Frame 60: Capture FXAA high quality
        ctx.OnFrame(60, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame60] Capturing FXAA high quality");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 60);
            CFFLog::Info("VISUAL_EXPECTATION: More aggressive smoothing, softer image");
        });

        // Frame 65: Finish test
        ctx.OnFrame(65, [&ctx]() {
            CFFLog::Info("[TestAntiAliasing:Frame65] Test complete");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: All AA modes rendered without errors");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestAntiAliasing)
