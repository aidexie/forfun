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
#include "Engine/Rendering/FSR2Pass.h"
#include "Engine/Camera.h"
#include "RHI/RHIManager.h"
#include <DirectXMath.h>

using namespace DirectX;

// Frame timing constants
namespace {
    constexpr int kFrameSetup = 1;
    constexpr int kFrameCheckSupport = 5;
    constexpr int kFrameEnableFSR2 = 10;
    constexpr int kFrameCaptureNativeAA = 30;
    constexpr int kFrameSwitchQuality = 35;
    constexpr int kFrameCaptureQuality = 55;
    constexpr int kFrameSwitchPerformance = 60;
    constexpr int kFrameCapturePerformance = 80;
    constexpr int kFrameVerify = 85;
    constexpr int kFrameFinish = 90;
}

/**
 * Test: FSR 2.0 (AMD FidelityFX Super Resolution 2)
 *
 * Verifies FSR 2.0 temporal upscaling:
 * - DX12-only support check
 * - Quality modes (NativeAA, Quality, Balanced, Performance, UltraPerformance)
 * - Temporal anti-aliasing and upscaling quality
 * - Jitter application and history accumulation
 */
class CTestFSR2 : public ITestCase {
public:
    const char* GetName() const override { return "TestFSR2"; }

    void Setup(CTestContext& ctx) override {
        ctx.OnFrame(kFrameSetup, [&ctx]() {
            CFFLog::Info("[TestFSR2] Setting up test scene");

            auto& scene = CScene::Instance();
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({0.0f, 5.0f, -12.0f}, {0.0f, 0.0f, 0.0f});

            // Enable TAA jitter (FSR2 uses same camera jitter system for now)
            cam.SetTAAEnabled(true);
            cam.SetJitterSampleCount(16);

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
            floor_t->scale = XMFLOAT3(25.0f, 0.1f, 25.0f);
            auto* floor_mesh = floor->AddComponent<SMeshRenderer>();
            floor_mesh->path = "mesh/cube.obj";
            floor_mesh->materialPath = "materials/default_gray.ffasset";

            // Thin vertical bars (high-frequency detail for upscaling quality test)
            for (int i = -6; i <= 6; ++i) {
                auto* bar = scene.GetWorld().Create("Bar" + std::to_string(i));
                auto* bar_t = bar->AddComponent<STransform>();
                bar_t->position = XMFLOAT3(i * 1.5f, 1.5f, 0.0f);
                bar_t->scale = XMFLOAT3(0.08f, 3.0f, 0.08f);
                auto* bar_mesh = bar->AddComponent<SMeshRenderer>();
                bar_mesh->path = "mesh/cube.obj";
                bar_mesh->materialPath = (i % 2 == 0) ?
                    "materials/default_white.ffasset" : "materials/default_red.ffasset";
            }

            // Rotated cube (diagonal edges test temporal stability)
            auto* cube = scene.GetWorld().Create("RotatedCube");
            auto* cube_t = cube->AddComponent<STransform>();
            cube_t->position = XMFLOAT3(0.0f, 2.5f, 6.0f);
            cube_t->scale = XMFLOAT3(2.5f, 2.5f, 2.5f);
            cube_t->SetRotation(15.0f, 45.0f, 0.0f);
            auto* cube_mesh = cube->AddComponent<SMeshRenderer>();
            cube_mesh->path = "mesh/cube.obj";
            cube_mesh->materialPath = "materials/default_blue.ffasset";

            // Sphere for smooth gradients
            auto* sphere = scene.GetWorld().Create("Sphere");
            auto* sphere_t = sphere->AddComponent<STransform>();
            sphere_t->position = XMFLOAT3(-5.0f, 1.5f, 4.0f);
            sphere_t->scale = XMFLOAT3(1.5f, 1.5f, 1.5f);
            auto* sphere_mesh = sphere->AddComponent<SMeshRenderer>();
            sphere_mesh->path = "mesh/sphere.obj";
            sphere_mesh->materialPath = "materials/default_white.ffasset";
        });

        ctx.OnFrame(kFrameCheckSupport, [&ctx]() {
            CFFLog::Info("[TestFSR2] Checking FSR 2.0 support");

            auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            ctx.Assert(pipeline != nullptr, "Expected DeferredRenderPipeline");
            if (!pipeline) return;

            bool isSupported = pipeline->GetFSR2Pass().IsSupported();
            bool isDX12 = RHI::CRHIManager::Instance().GetBackend() == RHI::EBackend::DX12;

            CFFLog::Info("[TestFSR2] Backend: %s, FSR2 Supported: %s",
                         isDX12 ? "DX12" : "DX11",
                         isSupported ? "Yes" : "No");

            // FSR2 should only be supported on DX12
            ctx.Assert(isSupported == isDX12, "FSR2 support should match DX12 backend");

            if (!isSupported) {
                CFFLog::Warning("[TestFSR2] FSR 2.0 not supported - skipping rendering tests");
                ctx.testPassed = true;
                ctx.Finish();
            }
        });

        ctx.OnFrame(kFrameEnableFSR2, [&ctx]() {
            CFFLog::Info("[TestFSR2] Enabling FSR 2.0 with NativeAA mode");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.fsr2.enabled = true;
            settings.fsr2.qualityMode = EFSR2QualityMode::NativeAA;
            settings.fsr2.sharpness = 0.5f;

            // Disable TAA (FSR2 replaces it)
            CEditorContext::Instance().GetShowFlags().TAA = false;
        });

        ctx.OnFrame(kFrameCaptureNativeAA, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, kFrameCaptureNativeAA);
            CFFLog::Info("VISUAL_EXPECTATION: FSR2 NativeAA - temporal AA quality similar to TAA, no upscaling");
        });

        ctx.OnFrame(kFrameSwitchQuality, [&ctx]() {
            CFFLog::Info("[TestFSR2] Switching to Quality mode (1.5x upscale)");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.fsr2.qualityMode = EFSR2QualityMode::Quality;

            if (auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline)) {
                pipeline->GetFSR2Pass().InvalidateHistory();
            }
        });

        ctx.OnFrame(kFrameCaptureQuality, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, kFrameCaptureQuality);
            CFFLog::Info("VISUAL_EXPECTATION: FSR2 Quality - upscaled output, sharp edges, good detail preservation");
        });

        ctx.OnFrame(kFrameSwitchPerformance, [&ctx]() {
            CFFLog::Info("[TestFSR2] Switching to Performance mode (2.0x upscale)");

            auto& settings = CScene::Instance().GetLightSettings();
            settings.fsr2.qualityMode = EFSR2QualityMode::Performance;

            if (auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline)) {
                pipeline->GetFSR2Pass().InvalidateHistory();
            }
        });

        ctx.OnFrame(kFrameCapturePerformance, [&ctx]() {
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, kFrameCapturePerformance);
            CFFLog::Info("VISUAL_EXPECTATION: FSR2 Performance - upscaled output, acceptable quality at 2x scale");
        });

        ctx.OnFrame(kFrameVerify, [&ctx]() {
            CFFLog::Info("[TestFSR2] Verifying FSR2 pass state");

            auto* pipeline = dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            ctx.Assert(pipeline != nullptr, "Expected DeferredRenderPipeline");
            if (!pipeline) return;

            auto& fsr2_pass = pipeline->GetFSR2Pass();
            auto& settings = CScene::Instance().GetLightSettings().fsr2;

            ctx.Assert(settings.enabled, "FSR2 should be enabled");
            ctx.Assert(fsr2_pass.IsSupported(), "FSR2 should be supported on DX12");
            ctx.Assert(fsr2_pass.IsReady(), "FSR2 context should be ready");
            ctx.Assert(settings.qualityMode == EFSR2QualityMode::Performance, "Quality mode should be Performance");

            CFFLog::Info("[TestFSR2] FSR2 Pass Status:");
            CFFLog::Info("  - Supported: %s", fsr2_pass.IsSupported() ? "Yes" : "No");
            CFFLog::Info("  - Ready: %s", fsr2_pass.IsReady() ? "Yes" : "No");
            CFFLog::Info("  - Quality Mode: %s", GetFSR2QualityModeName(settings.qualityMode));
            CFFLog::Info("  - Sharpness: %.2f", settings.sharpness);
        });

        ctx.OnFrame(kFrameFinish, [&ctx]() {
            CFFLog::Info("[TestFSR2] Test complete");
            ctx.testPassed = ctx.failures.empty();
            if (ctx.testPassed) {
                CFFLog::Info("TEST PASSED: FSR 2.0 rendering correctly");
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestFSR2)
