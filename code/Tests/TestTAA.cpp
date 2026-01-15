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
#include "Engine/Rendering/TAAPass.h"
#include "Engine/Camera.h"
#include <DirectXMath.h>

using namespace DirectX;

// Frame timing constants
namespace {
    constexpr int kFrameSetup = 1;
    constexpr int kFrameDisableTAA = 5;
    constexpr int kFrameCaptureNoTAA = 10;
    constexpr int kFrameEnableBasic = 15;
    constexpr int kFrameCaptureBasic = 30;
    constexpr int kFrameSwitchProduction = 35;
    constexpr int kFrameCaptureProduction = 55;
    constexpr int kFrameVerify = 60;
    constexpr int kFrameTestAlgorithms = 65;
    constexpr int kFrameFinish = 70;
}

/**
 * Test: TAA (Temporal Anti-Aliasing)
 *
 * Verifies TAA reduces aliasing through temporal accumulation:
 * - Sub-pixel jitter applied to projection matrix
 * - History buffer accumulates samples over time
 * - Different algorithm levels produce expected quality
 */
class CTestTAA : public ITestCase {
public:
    const char* GetName() const override { return "TestTAA"; }

    void Setup(CTestContext& ctx) override {
        ctx.OnFrame(kFrameSetup, [&ctx]() {
            CFFLog::Info("[TestTAA] Setting up test scene");

            auto& scene = CScene::Instance();
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({0.0f, 5.0f, -10.0f}, {0.0f, 0.0f, 0.0f});

            // Directional light
            auto* light_obj = scene.GetWorld().Create("DirectionalLight");
            auto* light_t = light_obj->AddComponent<STransform>();
            light_t->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dir_light = light_obj->AddComponent<SDirectionalLight>();
            dir_light->color = XMFLOAT3(1.0f, 1.0f, 0.95f);
            dir_light->intensity = 3.0f;

            // Ground plane
            auto* floor = scene.GetWorld().Create("Floor");
            auto* floor_t = floor->AddComponent<STransform>();
            floor_t->position = XMFLOAT3(0.0f, -0.5f, 0.0f);
            floor_t->scale = XMFLOAT3(20.0f, 0.1f, 20.0f);
            auto* floor_mesh = floor->AddComponent<SMeshRenderer>();
            floor_mesh->path = "mesh/cube.obj";
            floor_mesh->materialPath = "materials/default_gray.ffasset";

            // Thin vertical bars (high-frequency detail for aliasing test)
            for (int i = -5; i <= 5; ++i) {
                auto* bar = scene.GetWorld().Create("Bar" + std::to_string(i));
                auto* bar_t = bar->AddComponent<STransform>();
                bar_t->position = XMFLOAT3(i * 1.5f, 1.0f, 0.0f);
                bar_t->scale = XMFLOAT3(0.1f, 2.0f, 0.1f);
                auto* bar_mesh = bar->AddComponent<SMeshRenderer>();
                bar_mesh->path = "mesh/cube.obj";
                bar_mesh->materialPath = (i % 2 == 0) ?
                    "materials/default_white.ffasset" : "materials/default_red.ffasset";
            }

            // Rotated cube (diagonal edges show aliasing clearly)
            auto* cube = scene.GetWorld().Create("RotatedCube");
            auto* cube_t = cube->AddComponent<STransform>();
            cube_t->position = XMFLOAT3(0.0f, 2.0f, 5.0f);
            cube_t->scale = XMFLOAT3(2.0f, 2.0f, 2.0f);
            cube_t->SetRotation(0.0f, 45.0f, 0.0f);
            auto* cube_mesh = cube->AddComponent<SMeshRenderer>();
            cube_mesh->path = "mesh/cube.obj";
            cube_mesh->materialPath = "materials/default_blue.ffasset";
        });

        ctx.OnFrame(kFrameDisableTAA, [&ctx]() {
            CFFLog::Info("[TestTAA] Capturing baseline without TAA");
            CEditorContext::Instance().GetShowFlags().TAA = false;
        });

        ctx.OnFrame(kFrameCaptureNoTAA, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, kFrameCaptureNoTAA);
            CFFLog::Info("VISUAL_EXPECTATION: Without TAA - visible aliasing on thin bars and diagonal edges");
        });

        ctx.OnFrame(kFrameEnableBasic, [&ctx]() {
            CFFLog::Info("[TestTAA] Enabling TAA with Basic algorithm");
            CEditorContext::Instance().GetShowFlags().TAA = true;

            if (auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline)) {
                auto& settings = pipeline->GetTAAPass().GetSettings();
                settings.algorithm = ETAAAlgorithm::Basic;
                settings.history_blend = 0.9f;
            }
        });

        ctx.OnFrame(kFrameCaptureBasic, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, kFrameCaptureBasic);
            CFFLog::Info("VISUAL_EXPECTATION: Basic TAA - some smoothing but may have ghosting");
        });

        ctx.OnFrame(kFrameSwitchProduction, [&ctx]() {
            CFFLog::Info("[TestTAA] Switching to Production algorithm");
            if (auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline)) {
                auto& settings = pipeline->GetTAAPass().GetSettings();
                settings.algorithm = ETAAAlgorithm::Production;
                settings.history_blend = 0.95f;
                settings.sharpening_enabled = true;
                settings.sharpening_strength = 0.2f;
                pipeline->GetTAAPass().InvalidateHistory();
            }
        });

        ctx.OnFrame(kFrameCaptureProduction, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, kFrameCaptureProduction);
            CFFLog::Info("VISUAL_EXPECTATION: Production TAA - smooth edges, minimal ghosting, sharp details");
        });

        ctx.OnFrame(kFrameVerify, [&ctx]() {
            CFFLog::Info("[TestTAA] Verifying TAA pass state");

            auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            ctx.Assert(pipeline != nullptr, "Expected DeferredRenderPipeline");
            if (!pipeline) return;

            auto& taa_pass = pipeline->GetTAAPass();
            auto& show_flags = CEditorContext::Instance().GetShowFlags();
            auto& settings = taa_pass.GetSettings();

            ctx.Assert(show_flags.TAA, "TAA should be enabled");
            ctx.Assert(taa_pass.GetOutput() != nullptr, "TAA output texture should exist");
            ctx.Assert(settings.algorithm == ETAAAlgorithm::Production, "Algorithm should be Production");
            ctx.Assert(CScene::Instance().GetEditorCamera().IsTAAEnabled(), "Camera TAA jitter should be enabled");
        });

        ctx.OnFrame(kFrameTestAlgorithms, [&ctx]() {
            CFFLog::Info("[TestTAA] Testing algorithm switching");
            if (auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline)) {
                auto& settings = pipeline->GetTAAPass().GetSettings();
                for (int alg = 0; alg <= 6; ++alg) {
                    settings.algorithm = static_cast<ETAAAlgorithm>(alg);
                }
                settings.algorithm = ETAAAlgorithm::Production;
            }
        });

        ctx.OnFrame(kFrameFinish, [&ctx]() {
            CFFLog::Info("[TestTAA] Test complete");
            ctx.testPassed = ctx.failures.empty();
            if (ctx.testPassed) {
                CFFLog::Info("TEST PASSED: TAA rendering correctly");
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestTAA)
