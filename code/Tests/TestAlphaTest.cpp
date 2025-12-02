#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"  // FFPath namespace
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Core/MaterialManager.h"
#include "Core/MaterialAsset.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Alpha Test (Mask Mode)
 *
 * Purpose:
 *   Verify that Alpha Test (binary transparency) works correctly with grass model.
 *   This test validates that alphaMode=Mask and alphaCutoff parameters are properly
 *   integrated into the rendering pipeline.
 */
class CTestAlphaTest : public ITestCase {
public:
    const char* GetName() const override {
        return "TestAlphaTest";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create grass with alpha test material
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestAlphaTest:Frame1] Setting up alpha test scene");

            auto& scene = CScene::Instance();

            // Create grass material with Alpha Test enabled
            CMaterialAsset* mat = new CMaterialAsset("GrassAlphaTest");
            mat->albedo = XMFLOAT3(1.0f, 1.0f, 1.0f);
            mat->metallic = 0.0f;
            mat->roughness = 0.8f;
            mat->alphaMode = EAlphaMode::Mask;  // Enable alpha test
            mat->alphaCutoff = 0.5f;  // Cutoff threshold
            std::string matPath = "generated/GrassAlphaTest.ffasset";
            mat->SaveToFile(FFPath::GetAbsolutePath(matPath));
            delete mat;

            // Create grass object
            auto* grass = scene.GetWorld().Create("Grass");
            auto* transform = grass->AddComponent<STransform>();
            transform->position = XMFLOAT3(0.0f, 0.0f, 0.0f);

            auto* meshRenderer = grass->AddComponent<SMeshRenderer>();
            meshRenderer->path = "pbr_models/grass_medium/grass_medium_01_1k.gltf";
            meshRenderer->materialPath = matPath;

            CFFLog::Info("[TestAlphaTest] Created grass with alphaMode=Mask");
        });

        // Frame 20: Screenshot and verify
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestAlphaTest:Frame20] Capturing screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Grass with hard-edged transparency");
            CFFLog::Info("VISUAL_EXPECTATION: No black squares around grass blades");

            // Verify material has alpha test enabled
            CMaterialManager& matMgr = CMaterialManager::Instance();
            CMaterialAsset* grassMat = matMgr.Load("generated/GrassAlphaTest.ffasset");
            ASSERT_NOT_NULL(ctx, grassMat, "Grass material should load");
            ASSERT_EQUAL(ctx, static_cast<int>(grassMat->alphaMode), static_cast<int>(EAlphaMode::Mask),
                         "Grass material alphaMode should be Mask");
            ASSERT_EQUAL(ctx, grassMat->alphaCutoff, 0.5f, "alphaCutoff should be 0.5");

            CFFLog::Info("[TestAlphaTest:Frame20] All assertions passed");
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

REGISTER_TEST(CTestAlphaTest)
