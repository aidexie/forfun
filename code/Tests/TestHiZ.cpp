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
#include "Engine/Rendering/HiZPass.h"
#include "Engine/Camera.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Hi-Z (Hierarchical-Z Depth Pyramid)
 *
 * Purpose:
 *   Verify that the Hi-Z pyramid is correctly generated:
 *   - Mip 0 matches the depth buffer exactly
 *   - Each subsequent mip is half resolution
 *   - MAX reduction preserves closest surface (reversed-Z)
 *
 * Scene Setup:
 *   - Simple scene with objects at various depths
 *   - Demonstrates depth pyramid mip levels
 *
 * Expected Results:
 *   - Mip 0: Full resolution depth
 *   - Mip 1-4: Progressively lower resolution, blocky appearance
 *   - No black pixels (would indicate barrier issues)
 */
class CTestHiZ : public ITestCase {
public:
    const char* GetName() const override {
        return "TestHiZ";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Set up camera
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({5.0f, 5.0f, -5.0f}, {0.0f, 0.0f, 5.0f});

            // Directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 1.0f, 1.0f);
            dirLight->intensity = 2.0f;

            // Floor
            auto* floor = scene.GetWorld().Create("Floor");
            auto* floorT = floor->AddComponent<STransform>();
            floorT->position = XMFLOAT3(0.0f, 0.0f, 5.0f);
            floorT->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* floorMesh = floor->AddComponent<SMeshRenderer>();
            floorMesh->path = "mesh/cube.obj";

            // Objects at various depths
            for (int i = 0; i < 5; ++i) {
                auto* box = scene.GetWorld().Create("Box" + std::to_string(i));
                auto* boxT = box->AddComponent<STransform>();
                boxT->position = XMFLOAT3(-3.0f + i * 1.5f, 0.5f, 3.0f + i * 2.0f);
                boxT->scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
                auto* boxMesh = box->AddComponent<SMeshRenderer>();
                boxMesh->path = "mesh/cube.obj";
            }

            CFFLog::Info("[TestHiZ:Frame1] Scene created");
        });

        // Frame 5: Show Hi-Z Mip 0 (should match depth)
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame5] Setting debug mode to Hi-Z Mip 0");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::HiZ_Mip0;
        });

        // Frame 10: Capture Hi-Z Mip 0
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame10] Capturing Hi-Z Mip 0");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 10);
            CFFLog::Info("VISUAL_EXPECTATION: Hi-Z Mip 0 - should match depth buffer exactly");
        });

        // Frame 15: Show Hi-Z Mip 1
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame15] Setting debug mode to Hi-Z Mip 1");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::HiZ_Mip1;
        });

        // Frame 20: Capture Hi-Z Mip 1
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame20] Capturing Hi-Z Mip 1");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);
            CFFLog::Info("VISUAL_EXPECTATION: Hi-Z Mip 1 - half resolution, slight blocky");
        });

        // Frame 25: Show Hi-Z Mip 2
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame25] Setting debug mode to Hi-Z Mip 2");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::HiZ_Mip2;
        });

        // Frame 30: Capture Hi-Z Mip 2
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame30] Capturing Hi-Z Mip 2");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 30);
            CFFLog::Info("VISUAL_EXPECTATION: Hi-Z Mip 2 - more blocky, lower resolution");
        });

        // Frame 35: Show Hi-Z Mip 3
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame35] Setting debug mode to Hi-Z Mip 3");
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::HiZ_Mip3;
        });

        // Frame 40: Capture Hi-Z Mip 3
        ctx.OnFrame(40, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame40] Capturing Hi-Z Mip 3");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 40);
            CFFLog::Info("VISUAL_EXPECTATION: Hi-Z Mip 3 - very blocky, low resolution");
        });

        // Frame 45: Verify Hi-Z pass initialization
        ctx.OnFrame(45, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame45] Verifying Hi-Z pass state");

            CDeferredRenderPipeline* deferredPipeline =
                dynamic_cast<CDeferredRenderPipeline*>(ctx.pipeline);
            if (deferredPipeline) {
                auto& hiZPass = deferredPipeline->GetHiZPass();
                uint32_t mipCount = hiZPass.GetMipCount();
                RHI::ITexture* hiZTexture = hiZPass.GetHiZTexture();

                CFFLog::Info("[TestHiZ:Frame45] Hi-Z pyramid: %ux%u, %u mips",
                    hiZPass.GetWidth(), hiZPass.GetHeight(), mipCount);

                ctx.Assert(hiZTexture != nullptr, "Hi-Z texture should be created");
                ctx.Assert(mipCount > 0, "Hi-Z should have at least 1 mip level");
                ctx.Assert(mipCount >= 8, "Hi-Z should have ~10+ mips at 1080p");
            } else {
                ctx.Assert(false, "Expected DeferredRenderPipeline");
            }
        });

        // Frame 50: Finish test
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestHiZ:Frame50] Test complete");

            // Reset debug mode
            CScene::Instance().GetLightSettings().gBufferDebugMode = EGBufferDebugMode::None;

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Hi-Z pyramid generated correctly");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestHiZ)
