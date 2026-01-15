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
#include "Engine/Rendering/SSRPass.h"
#include "Engine/Rendering/HiZPass.h"
#include "Engine/Camera.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: SSR (Screen-Space Reflections)
 *
 * Purpose:
 *   Verify that SSR correctly traces reflections:
 *   - Reflective floor shows reflected objects
 *   - Hit confidence is computed correctly
 *   - Roughness fadeout works
 *
 * Scene Setup:
 *   - Reflective floor (roughness=0, metallic=1) using mirror.ffasset
 *   - Colored cubes to be reflected
 *   - Camera looking at reflections
 *
 * Expected Results:
 *   - SSR Result: Visible reflections of cubes on floor
 *   - SSR Confidence: White where hits, black where misses
 *   - No obvious artifacts or black holes
 */
class CTestSSR : public ITestCase {
public:
    const char* GetName() const override {
        return "TestSSR";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Set up camera looking down at the floor
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({0.0f, 5.0f, -8.0f}, {0.0f, 0.0f, 2.0f});

            // Directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 1.0f, 0.95f);
            dirLight->intensity = 3.0f;

            // Reflective floor (mirror-like) - uses mirror.ffasset (metallic=1, roughness=0)
            auto* floor = scene.GetWorld().Create("ReflectiveFloor");
            auto* floorT = floor->AddComponent<STransform>();
            floorT->position = XMFLOAT3(0.0f, 0.0f, 5.0f);
            floorT->scale = XMFLOAT3(15.0f, 0.1f, 15.0f);
            auto* floorMesh = floor->AddComponent<SMeshRenderer>();
            floorMesh->path = "mesh/cube.obj";
            floorMesh->materialPath = "materials/mirror.ffasset";

            // Colored cubes using different material assets
            const char* matPaths[] = {
                "materials/default_red.ffasset",
                "materials/default_green.ffasset",
                "materials/default_blue.ffasset",
                "materials/default_white.ffasset",
                "materials/default_gray.ffasset",
            };

            for (int i = 0; i < 5; ++i) {
                auto* box = scene.GetWorld().Create("ColorBox" + std::to_string(i));
                auto* boxT = box->AddComponent<STransform>();
                boxT->position = XMFLOAT3(-4.0f + i * 2.0f, 1.0f, 4.0f);
                boxT->scale = XMFLOAT3(0.8f, 0.8f, 0.8f);
                auto* boxMesh = box->AddComponent<SMeshRenderer>();
                boxMesh->path = "mesh/cube.obj";
                boxMesh->materialPath = matPaths[i];
            }
            {
                auto* box = scene.GetWorld().Create("ColorBox_Wall");
                auto* boxT = box->AddComponent<STransform>();
                boxT->position = XMFLOAT3(0.0f, 0.0f, 12.0f);
                boxT->scale = XMFLOAT3(15.8f, 15.8f, 0.8f);
                auto* boxMesh = box->AddComponent<SMeshRenderer>();
                boxMesh->path = "mesh/cube.obj";
                boxMesh->materialPath = matPaths[3];
            }

            // Tall box in background (to test reflection at distance)
            auto* tallBox = scene.GetWorld().Create("TallBox");
            auto* tallT = tallBox->AddComponent<STransform>();
            tallT->position = XMFLOAT3(0.0f, 2.0f, 8.0f);
            tallT->scale = XMFLOAT3(1.0f, 4.0f, 1.0f);
            auto* tallMesh = tallBox->AddComponent<SMeshRenderer>();
            tallMesh->path = "mesh/cube.obj";
            tallMesh->materialPath = "materials/default_white.ffasset";

            CFFLog::Info("[TestSSR:Frame1] Scene created with reflective floor and colored cubes");
        });

        // Frame 3: Enable SSR and Hi-Z
        ctx.OnFrame(3, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame3] Enabling SSR and Hi-Z");

            // Enable SSR and Hi-Z via showFlags
            ctx.showFlags.HiZ = true;
            ctx.showFlags.SSR = true;

            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                // Configure SSR quality settings
                deferredPipeline->GetSSRPass().GetSettings().maxDistance = 500.0f;
                deferredPipeline->GetSSRPass().GetSettings().maxSteps = 64;
            }
        });

        // Frame 10: Show normal rendering (with SSR applied)
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame10] Capturing normal rendering with SSR");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::None;
        });

        // Frame 15: Capture normal rendering
        ctx.OnFrame(15, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 15);
            CFFLog::Info("VISUAL_EXPECTATION: Normal rendering - floor should show reflections of cubes");
        });

        // Frame 20: Show SSR Result debug
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame20] Setting debug mode to SSR Result");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::SSR_Result;
        });

        // Frame 25: Capture SSR Result
        ctx.OnFrame(25, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 25);
            CFFLog::Info("VISUAL_EXPECTATION: SSR Result - reflected colors visible on floor area");
        });

        // Frame 30: Show SSR Confidence debug
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame30] Setting debug mode to SSR Confidence");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::SSR_Confidence;
        });

        // Frame 35: Capture SSR Confidence
        ctx.OnFrame(35, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 35);
            CFFLog::Info("VISUAL_EXPECTATION: SSR Confidence - white where hits found, black elsewhere");
        });

        // Frame 40: Verify SSR pass state
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame40] Verifying SSR pass state");

            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                auto& ssrPass = deferredPipeline->GetSSRPass();
                auto& hiZPass = deferredPipeline->GetHiZPass();

                RHI::ITexture* ssrTexture = ssrPass.GetSSRTexture();
                RHI::ITexture* hiZTexture = hiZPass.GetHiZTexture();

                CFFLog::Info("[TestSSR:Frame40] SSR enabled: %s, Hi-Z enabled: %s",
                    ctx.showFlags.SSR ? "yes" : "no",
                    ctx.showFlags.HiZ ? "yes" : "no");

                ctx.Assert(ssrTexture != nullptr, "SSR texture should be created");
                ctx.Assert(hiZTexture != nullptr, "Hi-Z texture should be created (SSR dependency)");
                ctx.Assert(ctx.showFlags.SSR, "SSR should be enabled");
                ctx.Assert(ctx.showFlags.HiZ, "Hi-Z should be enabled");
            } else {
                ctx.Assert(false, "Expected DeferredRenderPipeline");
            }
        });

        // Frame 45: Finish test
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestSSR:Frame45] Test complete");

            // Reset debug mode
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::SSR_Result;

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: SSR rendering correctly");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestSSR)
