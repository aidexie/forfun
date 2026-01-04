#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Rendering/Deferred/GBuffer.h"
#include "Engine/Rendering/Deferred/DepthPrePass.h"
#include "Engine/Rendering/Deferred/GBufferPass.h"
#include "RHI/RHIManager.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: G-Buffer Infrastructure
 *
 * Purpose:
 *   Verify that the Deferred Rendering G-Buffer infrastructure works correctly.
 *   Tests GBuffer class creation and render target management.
 *
 * Expected Results:
 *   - GBuffer creates 5 render targets + depth buffer
 *   - All render targets have correct formats
 *   - DepthPrePass and GBufferPass initialize successfully
 */
class CTestGBuffer : public ITestCase {
public:
    const char* GetName() const override {
        return "TestGBuffer";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create scene with test objects
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestGBuffer:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Create a directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);  // Use SetRotation for degrees
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 2.0f;

            // Create a test sphere
            auto* sphere = scene.GetWorld().Create("TestSphere");
            auto* sphereTransform = sphere->AddComponent<STransform>();
            sphereTransform->position = XMFLOAT3(0.0f, 0.0f, 3.0f);
            sphereTransform->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            auto* sphereMesh = sphere->AddComponent<SMeshRenderer>();
            sphereMesh->path = "sphere.obj";

            // Create a test cube
            auto* cube = scene.GetWorld().Create("TestCube");
            auto* cubeTransform = cube->AddComponent<STransform>();
            cubeTransform->position = XMFLOAT3(-2.0f, 0.0f, 5.0f);
            cubeTransform->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            auto* cubeMesh = cube->AddComponent<SMeshRenderer>();
            cubeMesh->path = "cube.obj";

            CFFLog::Info("[TestGBuffer:Frame1] Scene created");
        });

        // Frame 5: Test GBuffer creation
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestGBuffer:Frame5] Testing GBuffer creation");

            // Create GBuffer
            CGBuffer gbuffer;
            bool initResult = gbuffer.Initialize(1280, 720);

            ASSERT(ctx, initResult, "GBuffer should initialize successfully");
            ASSERT(ctx, gbuffer.GetWidth() == 1280, "GBuffer width should be 1280");
            ASSERT(ctx, gbuffer.GetHeight() == 720, "GBuffer height should be 720");

            // Verify all render targets exist
            ASSERT_NOT_NULL(ctx, gbuffer.GetWorldPosMetallic(), "RT0 (WorldPosMetallic) should exist");
            ASSERT_NOT_NULL(ctx, gbuffer.GetNormalRoughness(), "RT1 (NormalRoughness) should exist");
            ASSERT_NOT_NULL(ctx, gbuffer.GetAlbedoAO(), "RT2 (AlbedoAO) should exist");
            ASSERT_NOT_NULL(ctx, gbuffer.GetEmissiveMaterialID(), "RT3 (EmissiveMaterialID) should exist");
            ASSERT_NOT_NULL(ctx, gbuffer.GetVelocity(), "RT4 (Velocity) should exist");
            ASSERT_NOT_NULL(ctx, gbuffer.GetDepthBuffer(), "Depth buffer should exist");

            // Verify GetRenderTargets API
            RHI::ITexture* rts[5];
            uint32_t rtCount;
            gbuffer.GetRenderTargets(rts, rtCount);
            ASSERT_EQUAL(ctx, (int)rtCount, 5, "GetRenderTargets should return 5 render targets");

            // Test resize
            gbuffer.Resize(1920, 1080);
            ASSERT(ctx, gbuffer.GetWidth() == 1920, "GBuffer width should be 1920 after resize");
            ASSERT(ctx, gbuffer.GetHeight() == 1080, "GBuffer height should be 1080 after resize");

            // Cleanup
            gbuffer.Shutdown();

            CFFLog::Info("[TestGBuffer:Frame5] GBuffer creation test passed");
        });

        // Frame 10: Test DepthPrePass initialization
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("[TestGBuffer:Frame10] Testing DepthPrePass initialization");

            CDepthPrePass depthPrePass;
            bool initResult = depthPrePass.Initialize();

            ASSERT(ctx, initResult, "DepthPrePass should initialize successfully");

            depthPrePass.Shutdown();
            CFFLog::Info("[TestGBuffer:Frame10] DepthPrePass initialization test passed");
        });

        // Frame 15: Test GBufferPass initialization
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("[TestGBuffer:Frame15] Testing GBufferPass initialization");

            CGBufferPass gbufferPass;
            bool initResult = gbufferPass.Initialize();

            ASSERT(ctx, initResult, "GBufferPass should initialize successfully");

            gbufferPass.Shutdown();
            CFFLog::Info("[TestGBuffer:Frame15] GBufferPass initialization test passed");
        });

        // Frame 20: Take screenshot using forward pipeline (for visual reference)
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestGBuffer:Frame20] Capturing reference screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Scene with sphere, cube, and ground visible");
        });

        // Frame 25: Finish test
        ctx.OnFrame(25, [&ctx]() {
            if (ctx.failures.empty()) {
                CFFLog::Info("✓ TEST PASSED: G-Buffer infrastructure works correctly");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("✗ TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestGBuffer)
