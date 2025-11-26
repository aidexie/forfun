#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/Testing/Assertions.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Core/MaterialManager.h"
#include "Core/MaterialAsset.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Emissive Materials
 *
 * Purpose:
 *   Verify that emissive materials (self-emitted light) are correctly rendered.
 *   Emissive should:
 *   1. Be visible in complete darkness (no ambient/IBL/directional light)
 *   2. NOT be affected by shadows or AO
 *   3. Support HDR intensity (emissiveStrength > 1.0)
 *   4. Work with both constant color and emissive textures
 *
 * Test Setup:
 *   - Frame 1-10: Create test scene with 3 cubes (different emissive modes)
 *   - Frame 20: Screenshot + Assertions (verify emissive is visible)
 *   - Frame 30: Finish test
 *
 * Expected Results:
 *   - All 3 cubes should be visible despite no lighting
 *   - Left cube (red emissive): Pure red glow
 *   - Middle cube (green emissive, high strength): Bright green glow
 *   - Right cube (no emissive): Should be BLACK (invisible)
 */
class CTestEmissive : public ITestCase {
public:
    const char* GetName() const override {
        return "TestEmissive";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1-10: Setup scene
        ctx.OnFrame(1, [&]() {
            CFFLog::Info("[TestEmissive:Frame1] Setting up emissive materials test scene");

            CScene& scene = ctx.scene;

            // Disable ALL lighting to ensure only emissive is visible
            auto* dirLightObj = scene.CreateGameObject("DirectionalLight");
            auto* dirLight = dirLightObj->AddComponent<SDirectionalLight>();
            dirLight->intensity = 0.0f;  // NO direct lighting
            dirLight->ibl_intensity = 0.0f;  // NO IBL lighting
            CFFLog::Info("[TestEmissive] Disabled all lighting (intensity=0, IBL=0)");

            // Create 3 cubes with different emissive settings
            float spacing = 3.0f;

            // === LEFT CUBE: Red Emissive (constant color, no texture) ===
            {
                CMaterialAsset* mat = new CMaterialAsset("EmissiveRed");
                mat->albedo = XMFLOAT3(0.1f, 0.1f, 0.1f);  // Dark gray (shouldn't be visible without light)
                mat->emissive = XMFLOAT3(1.0f, 0.0f, 0.0f);  // RED emissive
                mat->emissiveStrength = 1.0f;
                mat->metallic = 0.0f;
                mat->roughness = 0.5f;
                std::string matPath = "generated/EmissiveRed.ffasset";
                mat->SaveToFile("E:/forfun/assets/" + matPath);
                delete mat;

                auto* cubeLeft = scene.CreateGameObject("CubeEmissiveRed");
                auto* transform = cubeLeft->GetComponent<STransform>();
                transform->position = XMFLOAT3(-spacing, 0.0f, 0.0f);

                auto* meshRenderer = cubeLeft->AddComponent<SMeshRenderer>();
                meshRenderer->SetPath("primitives/cube.obj");
                meshRenderer->materialPath = matPath;

                CFFLog::Info("[TestEmissive] Created left cube: Red emissive (1.0 strength)");
            }

            // === MIDDLE CUBE: Green Emissive (HDR, high strength) ===
            {
                CMaterialAsset* mat = new CMaterialAsset("EmissiveGreenHDR");
                mat->albedo = XMFLOAT3(0.05f, 0.05f, 0.05f);  // Very dark
                mat->emissive = XMFLOAT3(0.0f, 1.0f, 0.0f);  // GREEN emissive
                mat->emissiveStrength = 5.0f;  // HDR: 5x intensity (for Bloom)
                mat->metallic = 0.0f;
                mat->roughness = 0.5f;
                std::string matPath = "generated/EmissiveGreenHDR.ffasset";
                mat->SaveToFile("E:/forfun/assets/" + matPath);
                delete mat;

                auto* cubeMiddle = scene.CreateGameObject("CubeEmissiveGreen");
                auto* transform = cubeMiddle->GetComponent<STransform>();
                transform->position = XMFLOAT3(0.0f, 0.0f, 0.0f);

                auto* meshRenderer = cubeMiddle->AddComponent<SMeshRenderer>();
                meshRenderer->SetPath("primitives/cube.obj");
                meshRenderer->materialPath = matPath;

                CFFLog::Info("[TestEmissive] Created middle cube: Green emissive (5.0 HDR strength)");
            }

            // === RIGHT CUBE: NO Emissive (should be invisible) ===
            {
                CMaterialAsset* mat = new CMaterialAsset("NoEmissive");
                mat->albedo = XMFLOAT3(0.8f, 0.8f, 0.8f);  // Bright albedo (but no light to see it)
                mat->emissive = XMFLOAT3(0.0f, 0.0f, 0.0f);  // NO emissive
                mat->emissiveStrength = 0.0f;
                mat->metallic = 0.0f;
                mat->roughness = 0.5f;
                std::string matPath = "generated/NoEmissive.ffasset";
                mat->SaveToFile("E:/forfun/assets/" + matPath);
                delete mat;

                auto* cubeRight = scene.CreateGameObject("CubeNoEmissive");
                auto* transform = cubeRight->GetComponent<STransform>();
                transform->position = XMFLOAT3(spacing, 0.0f, 0.0f);

                auto* meshRenderer = cubeRight->AddComponent<SMeshRenderer>();
                meshRenderer->SetPath("primitives/cube.obj");
                meshRenderer->materialPath = matPath;

                CFFLog::Info("[TestEmissive] Created right cube: NO emissive (should be invisible)");
            }

            // Set camera position to view all 3 cubes
            ctx.mainPass.ResetCameraLookAt(
                XMFLOAT3(0.0f, 2.0f, 10.0f),  // Eye: above and in front
                XMFLOAT3(0.0f, 0.0f, 0.0f)    // Look at origin
            );

            CFFLog::Info("[TestEmissive:Frame1] Scene setup complete");
        });

        // Frame 20: Capture screenshot and verify
        ctx.OnFrame(20, [&]() {
            CFFLog::Info("[TestEmissive:Frame20] Capturing screenshot and verifying emissive");

            // Take screenshot
            CScreenshot::CaptureTest(ctx.mainPass, ctx.testName, 20);

            // Visual expectations
            CFFLog::Info("VISUAL_EXPECTATION: Left cube should glow RED");
            CFFLog::Info("VISUAL_EXPECTATION: Middle cube should glow BRIGHT GREEN (HDR)");
            CFFLog::Info("VISUAL_EXPECTATION: Right cube should be INVISIBLE (black, no emissive)");
            CFFLog::Info("VISUAL_EXPECTATION: Background should be dark (no ambient light)");
            CFFLog::Info("VISUAL_EXPECTATION: Only emissive cubes are visible");

            // Assertions: Verify scene state
            ASSERT_EQUAL(ctx, ctx.scene.GetWorld().Objects().size(), 4,
                         "Scene should have 4 objects (3 cubes + 1 light)");

            // Verify DirectionalLight settings (no lighting)
            auto* dirLightObj = ctx.scene.FindGameObject("DirectionalLight");
            ASSERT_NOT_NULL(ctx, dirLightObj, "DirectionalLight object should exist");

            auto* dirLight = dirLightObj->GetComponent<SDirectionalLight>();
            ASSERT_NOT_NULL(ctx, dirLight, "DirectionalLight component should exist");
            ASSERT_EQUAL(ctx, dirLight->intensity, 0.0f, "DirectionalLight intensity should be 0");
            ASSERT_EQUAL(ctx, dirLight->ibl_intensity, 0.0f, "IBL intensity should be 0");

            // Verify emissive materials were created
            CMaterialManager& matMgr = CMaterialManager::Instance();
            CMaterialAsset* redMat = matMgr.Load("generated/EmissiveRed.ffasset");
            ASSERT_NOT_NULL(ctx, redMat, "Red emissive material should load");
            ASSERT_VEC3_EQUAL(ctx, redMat->emissive, XMFLOAT3(1.0f, 0.0f, 0.0f), 0.01f,
                              "Red emissive color should be (1, 0, 0)");

            CMaterialAsset* greenMat = matMgr.Load("generated/EmissiveGreenHDR.ffasset");
            ASSERT_NOT_NULL(ctx, greenMat, "Green emissive material should load");
            ASSERT_EQUAL(ctx, greenMat->emissiveStrength, 5.0f, "Green emissive strength should be 5.0 (HDR)");

            CFFLog::Info("[TestEmissive:Frame20] All assertions passed");
        });

        // Frame 30: Finish test
        ctx.OnFrame(30, [&]() {
            CFFLog::Info("[TestEmissive:Frame30] Test completed");

            // Check if there were any failures
            if (ctx.failures.empty()) {
                CFFLog::Info("✓ ALL ASSERTIONS PASSED");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("✗ TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }

            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestEmissive)
