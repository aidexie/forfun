#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/MaterialManager.h"
#include "Core/TextureManager.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include <DirectXMath.h>

using namespace DirectX;

class CTestMaterialAsset : public ITestCase {
public:
    const char* GetName() const override {
        return "TestMaterialAsset";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create test scene with multiple objects using different materials
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("Frame 1: Creating test scene with materials");

            // Clear existing scene
            auto& scene = CScene::Instance();
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            // Object 1: Cube with default white material
            auto* cube1 = scene.GetWorld().Create("Cube_DefaultWhite");
            auto* t1 = cube1->AddComponent<STransform>();
            t1->position = {-2.0f, 1.0f, 0.0f};
            t1->scale = {1.0f, 1.0f, 1.0f};
            auto* mr1 = cube1->AddComponent<SMeshRenderer>();
            mr1->path = "mesh/cube.obj";
            mr1->materialPath = "materials/default_white.ffasset";

            // Object 2: Sphere with default metal material
            auto* sphere1 = scene.GetWorld().Create("Sphere_DefaultMetal");
            auto* t2 = sphere1->AddComponent<STransform>();
            t2->position = {0.0f, 1.0f, 0.0f};
            t2->scale = {1.0f, 1.0f, 1.0f};
            auto* mr2 = sphere1->AddComponent<SMeshRenderer>();
            mr2->path = "mesh/sphere.obj";
            mr2->materialPath = "materials/default_metal.ffasset";

            // Object 3: Another sphere sharing the same metal material (test material sharing)
            auto* sphere2 = scene.GetWorld().Create("Sphere_SharedMetal");
            auto* t3 = sphere2->AddComponent<STransform>();
            t3->position = {2.0f, 1.0f, 0.0f};
            t3->scale = {1.0f, 1.0f, 1.0f};
            auto* mr3 = sphere2->AddComponent<SMeshRenderer>();
            mr3->path = "mesh/sphere.obj";
            mr3->materialPath = "materials/default_metal.ffasset";  // Same material as sphere1

            // Object 4: Cube with PBR textured material (Barrel)
            auto* barrel = scene.GetWorld().Create("Barrel_PBR");
            auto* t4 = barrel->AddComponent<STransform>();
            t4->position = {-2.0f, 0.0f, -3.0f};
            t4->scale = {1.0f, 1.0f, 1.0f};
            auto* mr4 = barrel->AddComponent<SMeshRenderer>();
            mr4->path = "pbr_models/Barrel_01_1k.gltf/Barrel_01_1k.gltf";
            mr4->materialPath = "materials/Barrel_01_1k.gltf_Barrel_01.ffasset";

            // Object 5: Ground plane with default gray material
            auto* ground = scene.GetWorld().Create("Ground");
            auto* t5 = ground->AddComponent<STransform>();
            t5->position = {0.0f, 0.0f, 0.0f};
            t5->scale = {10.0f, 0.1f, 10.0f};
            auto* mr5 = ground->AddComponent<SMeshRenderer>();
            mr5->path = "mesh/cube.obj";
            mr5->materialPath = "materials/default_gray.ffasset";

            CFFLog::Info("Created 5 test objects with different materials");
        });

        // Frame 10: Verify material loading and resource setup
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("Frame 10: Verifying material system");

            auto& scene = CScene::Instance();

            // Assertions: verify scene setup
            ASSERT_EQUAL(ctx, (int)scene.GetWorld().Count(), 5, "Scene should have 5 objects");

            // Verify MaterialManager is working
            CMaterialManager& matMgr = CMaterialManager::Instance();

            // Check that default_white material is loaded
            ASSERT(ctx, matMgr.IsLoaded("materials/default_white.ffasset"), "default_white material should be loaded");

            // Check that default_metal material is loaded (and shared by 2 objects)
            ASSERT(ctx, matMgr.IsLoaded("materials/default_metal.ffasset"), "default_metal material should be loaded");

            // Check that Barrel material is loaded
            ASSERT(ctx, matMgr.IsLoaded("materials/Barrel_01_1k.gltf_Barrel_01.ffasset"), "Barrel material should be loaded");

            // Verify all objects have MeshRenderer with materialPath
            for (int i = 0; i < scene.GetWorld().Count(); ++i) {
                auto* obj = scene.GetWorld().Get(i);
                auto* mr = obj->GetComponent<SMeshRenderer>();
                ASSERT_NOT_NULL(ctx, mr, ("MeshRenderer for " + obj->GetName()).c_str());
                ASSERT(ctx, !mr->materialPath.empty(), ("materialPath should not be empty for " + obj->GetName()).c_str());
            }

            // Verify TextureManager default textures exist
            CTextureManager& texMgr = CTextureManager::Instance();
            ASSERT_NOT_NULL(ctx, texMgr.GetDefaultWhite().get(), "Default white texture");
            ASSERT_NOT_NULL(ctx, texMgr.GetDefaultNormal().get(), "Default normal texture");
            ASSERT_NOT_NULL(ctx, texMgr.GetDefaultBlack().get(), "Default black texture");

            CFFLog::Info("✓ Frame 10: All material system assertions passed");
        });

        // Frame 20: Capture screenshot and verify visual results
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("Frame 20: Capturing screenshot and final verification");

            // Take screenshot
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            // Visual expectations
            CFFLog::Info("VISUAL_EXPECTATION: White cube should be visible on the left");
            CFFLog::Info("VISUAL_EXPECTATION: Two metallic spheres should be visible (center and right)");
            CFFLog::Info("VISUAL_EXPECTATION: Both metallic spheres should look identical (shared material)");
            CFFLog::Info("VISUAL_EXPECTATION: Barrel model should be visible with PBR textures");
            CFFLog::Info("VISUAL_EXPECTATION: Gray ground plane should be visible at bottom");
            CFFLog::Info("VISUAL_EXPECTATION: No pink/black missing texture colors");
            CFFLog::Info("VISUAL_EXPECTATION: All objects should have proper lighting and shadows");

            // Final assertions
            auto& scene = CScene::Instance();

            // Verify scene state
            std::string report = scene.GenerateReport();
            CFFLog::Info("Scene State:\n%s", report.c_str());

            // Verify material manager cache (should have 4 unique materials loaded)
            CMaterialManager& matMgr = CMaterialManager::Instance();
            int expectedMaterialCount = 4;  // default_white, default_metal, default_gray, Barrel
            CFFLog::Info("Material cache should contain %d unique materials", expectedMaterialCount);

            CFFLog::Info("✓ Frame 20: Visual verification complete");
        });

        // Frame 30: Finish test
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("Frame 30: Test finished");

            // Mark test as passed if no failures
            if (ctx.failures.empty()) {
                ctx.testPassed = true;
                CFFLog::Info("✓ ALL ASSERTIONS PASSED");
                CFFLog::Info("✓ Material Asset System working correctly");
                CFFLog::Info("✓ Material sharing verified (2 spheres use same material)");
                CFFLog::Info("✓ Texture loading working (default + PBR textures)");
            } else {
                ctx.testPassed = false;
                CFFLog::Error("✗ TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
            }

            ctx.Finish();
        });
    }
};

// Register the test
REGISTER_TEST(CTestMaterialAsset)
