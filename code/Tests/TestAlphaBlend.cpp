#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Core/MaterialManager.h"
#include "Core/MaterialAsset.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Alpha Test (Blend Mode)
 *
 * Purpose:
 *   Verify that Alpha Test (binary transparency) works correctly with face model.
 *   This test validates that alphaMode=Blend and alphaCutoff parameters are properly
 *   integrated into the rendering pipeline.
 */
class CTestAlphaBlend : public ITestCase {
public:
    const char* GetName() const override {
        return "TestAlphaBlend";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create face with alpha test material
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestAlphaBlend:Frame1] Setting up alpha test scene");

            auto& scene = CScene::Instance();

            // Create face object
            auto* face = scene.GetWorld().Create("Face");
            auto* transform = face->AddComponent<STransform>();
            transform->position = XMFLOAT3(0.0f, 0.0f, 2.0f);

            auto* meshRenderer = face->AddComponent<SMeshRenderer>();
            meshRenderer->path = "pbr_models/TestAlpha/AlphaTest.gltf";
            meshRenderer->materialPath = "materials/alpha_test.ffasset";

            CFFLog::Info("[TestAlphaBlend] Created face with alphaMode=Blend");
        });

        // Frame 20: Screenshot and verify
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestAlphaBlend:Frame20] Capturing screenshot");
            CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Face with smooth alpha blending");
            CFFLog::Info("VISUAL_EXPECTATION: Skybox visible through transparent parts");

            // Verify material has alpha test enabled
            CMaterialManager& matMgr = CMaterialManager::Instance();
            CMaterialAsset* faceMat = matMgr.Load("materials/alpha_test.ffasset");
            ASSERT_NOT_NULL(ctx, faceMat, "Face material should load");
            ASSERT_EQUAL(ctx, static_cast<int>(faceMat->alphaMode), static_cast <int>(EAlphaMode::Blend),       
                         "Face material alphaMode should be Blend");
            ASSERT_EQUAL(ctx, faceMat->alphaCutoff, 0.5f, "alphaCutoff should be 0.5");

            CFFLog::Info("[TestAlphaBlend:Frame20] All assertions passed");
        });

        // Frame 30: Finish
        ctx.OnFrame(30, [&ctx]() {
            if (ctx.failures.empty()) {
                CFFLog::Info("✓ TEST PASSED");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("✗ TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestAlphaBlend)
