#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/MaterialAsset.h"
#include "Core/MaterialManager.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * Test: Material Types in Deferred Rendering
 *
 * Purpose:
 *   Verify that different material types (Standard, Unlit) are correctly
 *   handled by the Deferred Rendering Pipeline's MaterialID system.
 *
 * Expected Results:
 *   - Standard materials show full PBR lighting (shadows, IBL, reflections)
 *   - Unlit materials show only emissive + albedo color (no lighting)
 *   - MaterialID is correctly encoded in G-Buffer RT3.a
 */
class CTestMaterialTypes : public ITestCase {
public:
    const char* GetName() const override {
        return "TestMaterialTypes";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create scene with test objects using different material types
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestMaterialTypes:Frame1] Setting up test scene");

            auto& scene = CScene::Instance();

            // Create a directional light (important for testing Standard vs Unlit)
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 3.0f;

            // Create materials with different types
            auto& matMgr = CMaterialManager::Instance();

            // Standard PBR material (red)
            CMaterialAsset* standardMat = matMgr.Create("mat/test_standard.ffasset");
            standardMat->albedo = XMFLOAT3(1.0f, 0.2f, 0.2f);  // Red
            standardMat->metallic = 0.0f;
            standardMat->roughness = 0.5f;
            standardMat->materialType = EMaterialType::Standard;
            standardMat->SaveToFile(FFPath::GetAssetsDir() + "/mat/test_standard.ffasset");

            // Unlit material (bright green emissive)
            CMaterialAsset* unlitMat = matMgr.Create("mat/test_unlit.ffasset");
            unlitMat->albedo = XMFLOAT3(0.0f, 1.0f, 0.0f);  // Green
            unlitMat->emissive = XMFLOAT3(0.0f, 1.0f, 0.0f);
            unlitMat->emissiveStrength = 2.0f;
            unlitMat->materialType = EMaterialType::Unlit;
            unlitMat->SaveToFile(FFPath::GetAssetsDir() + "/mat/test_unlit.ffasset");

            // Standard PBR material (blue, metallic)
            CMaterialAsset* metallicMat = matMgr.Create("mat/test_metallic.ffasset");
            metallicMat->albedo = XMFLOAT3(0.2f, 0.2f, 1.0f);  // Blue
            metallicMat->metallic = 1.0f;
            metallicMat->roughness = 0.1f;
            metallicMat->materialType = EMaterialType::Standard;
            metallicMat->SaveToFile(FFPath::GetAssetsDir() + "/mat/test_metallic.ffasset");

            // Create spheres with different materials - arranged in a row
            // Left: Standard Red
            auto* sphereStandard = scene.GetWorld().Create("SphereStandard");
            auto* t1 = sphereStandard->AddComponent<STransform>();
            t1->position = XMFLOAT3(-3.0f, 0.0f, 5.0f);
            t1->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            auto* m1 = sphereStandard->AddComponent<SMeshRenderer>();
            m1->path = "sphere.obj";
            m1->materialPath = "mat/test_standard.ffasset";

            // Center: Unlit Green (should NOT have shadows or lighting variation)
            auto* sphereUnlit = scene.GetWorld().Create("SphereUnlit");
            auto* t2 = sphereUnlit->AddComponent<STransform>();
            t2->position = XMFLOAT3(0.0f, 0.0f, 5.0f);
            t2->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            auto* m2 = sphereUnlit->AddComponent<SMeshRenderer>();
            m2->path = "sphere.obj";
            m2->materialPath = "mat/test_unlit.ffasset";

            // Right: Metallic Blue
            auto* sphereMetallic = scene.GetWorld().Create("SphereMetallic");
            auto* t3 = sphereMetallic->AddComponent<STransform>();
            t3->position = XMFLOAT3(3.0f, 0.0f, 5.0f);
            t3->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            auto* m3 = sphereMetallic->AddComponent<SMeshRenderer>();
            m3->path = "sphere.obj";
            m3->materialPath = "mat/test_metallic.ffasset";

            // Ground plane for shadow visibility
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundT = ground->AddComponent<STransform>();
            groundT->position = XMFLOAT3(0.0f, -1.5f, 5.0f);
            groundT->scale = XMFLOAT3(10.0f, 0.1f, 10.0f);
            auto* groundM = ground->AddComponent<SMeshRenderer>();
            groundM->path = "cube.obj";

            CFFLog::Info("[TestMaterialTypes:Frame1] Scene created with 3 material types");
        });

        // Frame 5: Verify material types are correctly set
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestMaterialTypes:Frame5] Verifying material types");

            auto& matMgr = CMaterialManager::Instance();

            // Load and verify Standard material
            CMaterialAsset* standardMat = matMgr.Load("mat/test_standard.ffasset");
            ASSERT_NOT_NULL(ctx, standardMat, "Standard material should load");
            if (standardMat) {
                ASSERT_EQUAL(ctx, (int)standardMat->materialType, (int)EMaterialType::Standard,
                    "Standard material should have EMaterialType::Standard");
            }

            // Load and verify Unlit material
            CMaterialAsset* unlitMat = matMgr.Load("mat/test_unlit.ffasset");
            ASSERT_NOT_NULL(ctx, unlitMat, "Unlit material should load");
            if (unlitMat) {
                ASSERT_EQUAL(ctx, (int)unlitMat->materialType, (int)EMaterialType::Unlit,
                    "Unlit material should have EMaterialType::Unlit");
            }

            CFFLog::Info("[TestMaterialTypes:Frame5] Material type verification passed");
        });

        // Frame 20: Take screenshot for visual verification
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestMaterialTypes:Frame20] Capturing screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION:");
            CFFLog::Info("  - Left sphere (red): Standard PBR with lighting and shadows");
            CFFLog::Info("  - Center sphere (green): Unlit, uniform color, NO lighting/shadows");
            CFFLog::Info("  - Right sphere (blue): Metallic PBR with reflections");
            CFFLog::Info("  - Ground visible with shadows from Standard/Metallic spheres");
        });

        // Frame 25: Finish test
        ctx.OnFrame(25, [&ctx]() {
            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Material types work correctly");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestMaterialTypes)
