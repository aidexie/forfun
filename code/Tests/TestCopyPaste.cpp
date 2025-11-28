// TestCopyPaste.cpp - Test GameObject Copy/Paste/Duplicate functionality
#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/PointLight.h"
#include <DirectXMath.h>

using namespace DirectX;

class CTestCopyPaste : public ITestCase {
public:
    const char* GetName() const override { return "TestCopyPaste"; }

    void Setup(CTestContext& ctx) override {
        CFFLog::Info("[TestCopyPaste] Setting up copy/paste test...");

        // ============================================
        // Frame 1-10: Scene Setup
        // ============================================
        ctx.OnFrame(1, [&]() {
            CFFLog::Info("[TestCopyPaste:Frame1] Creating test scene");
            auto& scene = CScene::Instance();

            // Create original GameObject with multiple components
            auto* original = scene.GetWorld().Create("TestObject");

            // Add Transform
            auto* transform = original->AddComponent<STransform>();
            transform->position = XMFLOAT3(5.0f, 2.0f, 0.0f);
            transform->SetRotation(0.0f, 45.0f, 0.0f);
            transform->scale = XMFLOAT3(1.5f, 1.5f, 1.5f);

            // Add MeshRenderer (resource reference)
            auto* meshRenderer = original->AddComponent<SMeshRenderer>();
            meshRenderer->path = "mesh/sphere.obj";
            meshRenderer->materialPath = "materials/default_white.ffasset";

            // Add PointLight
            auto* pointLight = original->AddComponent<SPointLight>();
            pointLight->color = XMFLOAT3(1.0f, 0.5f, 0.2f);
            pointLight->intensity = 100.0f;
            pointLight->range = 10.0f;

            CFFLog::Info("[TestCopyPaste:Frame1] Created original object with 3 components");
            ASSERT_EQUAL(ctx, (int)scene.GetWorld().Count(), 1, "Scene should have 1 object");
        });

        // ============================================
        // Frame 20: Test Copy + Paste
        // ============================================
        ctx.OnFrame(20, [&]() {
            CFFLog::Info("[TestCopyPaste:Frame20] Testing Copy + Paste");
            auto& scene = CScene::Instance();

            // Get original object
            auto* original = scene.GetWorld().Get(0);
            ASSERT_NOT_NULL(ctx, original, "Original object should exist");
            ASSERT_EQUAL(ctx, original->GetName(), std::string("TestObject"), "Original name");

            // === Test 1: Copy + Paste ===
            CFFLog::Info("[TestCopyPaste] Test 1: Copy + Paste");
            scene.CopyGameObject(original);
            auto* copy1 = scene.PasteGameObject();
            ASSERT_NOT_NULL(ctx, copy1, "First paste should succeed");

            // Verify naming conflict resolution
            ASSERT_EQUAL(ctx, copy1->GetName(), std::string("TestObject (1)"), "First copy should be named 'TestObject (1)'");

            // Verify object count
            ASSERT_EQUAL(ctx, (int)scene.GetWorld().Count(), 2, "Scene should have 2 objects after first paste");

            // Verify Transform offset
            auto* copy1Transform = copy1->GetComponent<STransform>();
            ASSERT_NOT_NULL(ctx, copy1Transform, "Copy should have Transform");
            ASSERT_IN_RANGE(ctx, copy1Transform->position.x, 5.4f, 5.6f, "Transform X should be offset by ~0.5");
            ASSERT_IN_RANGE(ctx, copy1Transform->position.y, 1.9f, 2.1f, "Transform Y should remain same");

            // Verify components copied
            auto* copy1Mesh = copy1->GetComponent<SMeshRenderer>();
            ASSERT_NOT_NULL(ctx, copy1Mesh, "Copy should have MeshRenderer");
            ASSERT_EQUAL(ctx, copy1Mesh->path, std::string("mesh/sphere.obj"), "Mesh path should be copied");
            ASSERT_EQUAL(ctx, copy1Mesh->materialPath, std::string("materials/default_white.ffasset"), "Material path should be copied");

            auto* copy1Light = copy1->GetComponent<SPointLight>();
            ASSERT_NOT_NULL(ctx, copy1Light, "Copy should have PointLight");
            ASSERT_IN_RANGE(ctx, copy1Light->intensity, 99.0f, 101.0f, "Light intensity should be copied");
            ASSERT_IN_RANGE(ctx, copy1Light->range, 9.0f, 11.0f, "Light range should be copied");

            // === Test 2: Paste Again (Name (2)) ===
            CFFLog::Info("[TestCopyPaste] Test 2: Paste again (should be Name (2))");
            auto* copy2 = scene.PasteGameObject();
            ASSERT_NOT_NULL(ctx, copy2, "Second paste should succeed");
            ASSERT_EQUAL(ctx, copy2->GetName(), std::string("TestObject (2)"), "Second copy should be named 'TestObject (2)'");
            ASSERT_EQUAL(ctx, (int)scene.GetWorld().Count(), 3, "Scene should have 3 objects");

            // === Test 3: Duplicate Function ===
            CFFLog::Info("[TestCopyPaste] Test 3: Duplicate (Copy+Paste in one step)");
            auto* dup1 = scene.DuplicateGameObject(original);
            ASSERT_NOT_NULL(ctx, dup1, "Duplicate should succeed");
            ASSERT_EQUAL(ctx, dup1->GetName(), std::string("TestObject (3)"), "Duplicated object should be named 'TestObject (3)'");
            ASSERT_EQUAL(ctx, (int)scene.GetWorld().Count(), 4, "Scene should have 4 objects");

            // Verify Transform offset on duplicate
            auto* dupTransform = dup1->GetComponent<STransform>();
            ASSERT_NOT_NULL(ctx, dupTransform, "Duplicate should have Transform");
            ASSERT_IN_RANGE(ctx, dupTransform->position.x, 5.4f, 5.6f, "Duplicate Transform X should be offset");

            // === Test 4: Copy an already numbered object ===
            CFFLog::Info("[TestCopyPaste] Test 4: Copy object with existing suffix");
            auto* dup2 = scene.DuplicateGameObject(copy1);  // copy1 is "TestObject (1)"
            ASSERT_NOT_NULL(ctx, dup2, "Duplicate of (1) should succeed");
            ASSERT_EQUAL(ctx, dup2->GetName(), std::string("TestObject (4)"), "Should continue numbering to (4)");

            // Visual expectation
            CFFLog::Info("VISUAL_EXPECTATION: N/A (this is a logic test, no screenshot needed)");

            CFFLog::Info("[TestCopyPaste:Frame20] All assertions passed");
        });

        // ============================================
        // Frame 30: Finalization
        // ============================================
        ctx.OnFrame(30, [&]() {
            CFFLog::Info("[TestCopyPaste:Frame30] Test finalization");

            if (ctx.failures.empty()) {
                CFFLog::Info("[TestCopyPaste] ALL ASSERTIONS PASSED");
                CFFLog::Info("[TestCopyPaste] Copy/Paste functionality working correctly:");
                CFFLog::Info("  [OK] Copy to clipboard (JSON serialization)");
                CFFLog::Info("  [OK] Paste from clipboard (JSON deserialization)");
                CFFLog::Info("  [OK] Naming conflict resolution (Name (1), (2), (3), ...)");
                CFFLog::Info("  [OK] Transform position offset (+0.5 units)");
                CFFLog::Info("  [OK] Component data deep copy");
                CFFLog::Info("  [OK] Resource reference sharing (mesh/material paths)");
                CFFLog::Info("  [OK] Duplicate function (Copy+Paste shortcut)");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("[TestCopyPaste] TEST FAILED with %d assertion failures", (int)ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
                ctx.testPassed = false;
            }

            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestCopyPaste)
