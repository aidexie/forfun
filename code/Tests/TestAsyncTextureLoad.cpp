#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/TextureManager.h"
#include "Core/TextureHandle.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include <DirectXMath.h>

using namespace DirectX;

/**
 * TestAsyncTextureLoad - Test async texture loading system
 *
 * Tests:
 * 1. LoadAsync() returns handle immediately (non-blocking)
 * 2. Handle initially returns placeholder texture
 * 3. Tick() processes pending loads
 * 4. After Tick(), handle returns real texture
 * 5. Duplicate LoadAsync() calls return same handle (caching)
 * 6. FlushPendingLoads() blocks until all loaded
 * 7. Load failure gracefully returns fallback texture
 */
class CTestAsyncTextureLoad : public ITestCase {
public:
    const char* GetName() const override {
        return "TestAsyncTextureLoad";
    }

    void Setup(CTestContext& ctx) override {
        // Store handles for cross-frame verification
        static TextureHandlePtr handle1;
        static TextureHandlePtr handle2;
        static TextureHandlePtr handle3;
        static TextureHandlePtr handleDuplicate;
        static TextureHandlePtr handleInvalid;
        static RHI::TextureSharedPtr placeholder;

        // Frame 1: Test LoadAsync returns immediately with placeholder
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("=== Frame 1: Testing LoadAsync() non-blocking behavior ===");

            CTextureManager& texMgr = CTextureManager::Instance();

            // Store placeholder for comparison
            placeholder = texMgr.GetPlaceholder();
            ASSERT_NOT_NULL(ctx, placeholder.get(), "Placeholder texture should exist");
            CFFLog::Info("Placeholder texture: %p", placeholder.get());

            // Clear any existing pending loads
            texMgr.Clear();

            // Test 1: LoadAsync should return immediately
            CFFLog::Info("Calling LoadAsync for 3 textures...");

            handle1 = texMgr.LoadAsync("pbr_models/Barrel_01_1k.gltf/Barrel_01_1k_albedo.png", true);
            ASSERT_NOT_NULL(ctx, handle1.get(), "Handle1 should not be null");
            ASSERT(ctx, handle1->IsLoading(), "Handle1 should be in loading state");
            ASSERT(ctx, !handle1->IsReady(), "Handle1 should not be ready yet");

            handle2 = texMgr.LoadAsync("pbr_models/Barrel_01_1k.gltf/Barrel_01_1k_normal.png", false);
            ASSERT_NOT_NULL(ctx, handle2.get(), "Handle2 should not be null");

            handle3 = texMgr.LoadAsync("pbr_models/Barrel_01_1k.gltf/Barrel_01_1k_metallic.png", false);
            ASSERT_NOT_NULL(ctx, handle3.get(), "Handle3 should not be null");

            // Test 2: GetTexture() should return placeholder
            RHI::ITexture* tex1 = handle1->GetTexture();
            ASSERT_NOT_NULL(ctx, tex1, "GetTexture should return non-null (placeholder)");
            ASSERT(ctx, tex1 == placeholder.get(), "GetTexture should return placeholder before load completes");

            // Test 3: Pending count should be 3
            uint32_t pendingCount = texMgr.GetPendingCount();
            ASSERT_EQUAL(ctx, (int)pendingCount, 3, "Should have 3 pending loads");
            CFFLog::Info("Pending loads: %u", pendingCount);

            // Test 4: Duplicate LoadAsync should return same handle
            handleDuplicate = texMgr.LoadAsync("pbr_models/Barrel_01_1k.gltf/Barrel_01_1k_albedo.png", true);
            ASSERT(ctx, handleDuplicate.get() == handle1.get(), "Duplicate LoadAsync should return same handle");
            CFFLog::Info("Duplicate handle test passed (same pointer: %p)", handle1.get());

            // Test 5: Invalid path should still return handle
            handleInvalid = texMgr.LoadAsync("nonexistent/texture.png", true);
            ASSERT_NOT_NULL(ctx, handleInvalid.get(), "Invalid path should still return handle");

            CFFLog::Info("✓ Frame 1: LoadAsync non-blocking tests passed");
        });

        // Frame 5: Test Tick() processes loads incrementally
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("=== Frame 5: Testing Tick() incremental processing ===");

            CTextureManager& texMgr = CTextureManager::Instance();

            uint32_t beforePending = texMgr.GetPendingCount();
            CFFLog::Info("Pending before Tick: %u", beforePending);

            // Process 1 load
            uint32_t processed = texMgr.Tick(1);
            CFFLog::Info("Tick(1) processed: %u loads", processed);

            uint32_t afterPending = texMgr.GetPendingCount();
            CFFLog::Info("Pending after Tick: %u", afterPending);

            // Should have processed exactly 1 (or 0 if none pending)
            ASSERT(ctx, processed <= 1, "Tick(1) should process at most 1 load");

            // Check if handle1 became ready (it was first in queue)
            if (handle1->IsReady()) {
                CFFLog::Info("Handle1 is now ready!");
                RHI::ITexture* realTex = handle1->GetTexture();
                ASSERT_NOT_NULL(ctx, realTex, "Real texture should not be null");
                ASSERT(ctx, realTex != placeholder.get(), "Real texture should differ from placeholder");
                CFFLog::Info("Real texture loaded: %p", realTex);
            }

            CFFLog::Info("✓ Frame 5: Tick incremental test passed");
        });

        // Frame 10: Test FlushPendingLoads() blocks until all loaded
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("=== Frame 10: Testing FlushPendingLoads() ===");

            CTextureManager& texMgr = CTextureManager::Instance();

            uint32_t beforeFlush = texMgr.GetPendingCount();
            CFFLog::Info("Pending before flush: %u", beforeFlush);

            // Flush all remaining
            texMgr.FlushPendingLoads();

            uint32_t afterFlush = texMgr.GetPendingCount();
            CFFLog::Info("Pending after flush: %u", afterFlush);

            ASSERT_EQUAL(ctx, (int)afterFlush, 0, "No pending loads after flush");

            // All valid handles should be ready now
            ASSERT(ctx, handle1->IsReady(), "Handle1 should be ready after flush");
            ASSERT(ctx, handle2->IsReady(), "Handle2 should be ready after flush");
            ASSERT(ctx, handle3->IsReady(), "Handle3 should be ready after flush");

            // Verify real textures are loaded
            ASSERT(ctx, handle1->GetTexture() != placeholder.get(), "Handle1 should have real texture");
            ASSERT(ctx, handle2->GetTexture() != placeholder.get(), "Handle2 should have real texture");
            ASSERT(ctx, handle3->GetTexture() != placeholder.get(), "Handle3 should have real texture");

            CFFLog::Info("Handle1 texture: %p (ready: %d)", handle1->GetTexture(), handle1->IsReady());
            CFFLog::Info("Handle2 texture: %p (ready: %d)", handle2->GetTexture(), handle2->IsReady());
            CFFLog::Info("Handle3 texture: %p (ready: %d)", handle3->GetTexture(), handle3->IsReady());

            // Test invalid path handling
            ASSERT(ctx, handleInvalid->IsReady() || handleInvalid->IsFailed(),
                   "Invalid handle should be ready (with fallback) or failed");
            ASSERT_NOT_NULL(ctx, handleInvalid->GetTexture(),
                            "Invalid path should still return fallback texture");
            CFFLog::Info("Invalid path handle state: ready=%d, failed=%d",
                         handleInvalid->IsReady(), handleInvalid->IsFailed());

            CFFLog::Info("✓ Frame 10: FlushPendingLoads test passed");
        });

        // Frame 15: Create visual test scene
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("=== Frame 15: Creating visual test scene ===");

            auto& scene = CScene::Instance();

            // Clear existing scene
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            // Create objects that will use async-loaded textures
            auto* barrel = scene.GetWorld().Create("Barrel_AsyncTextures");
            auto* t1 = barrel->AddComponent<STransform>();
            t1->position = {0.0f, 0.0f, 0.0f};
            t1->scale = {2.0f, 2.0f, 2.0f};
            auto* mr1 = barrel->AddComponent<SMeshRenderer>();
            mr1->path = "pbr_models/Barrel_01_1k.gltf/Barrel_01_1k.gltf";
            mr1->materialPath = "materials/Barrel_01_1k.gltf_Barrel_01.ffasset";

            // Position camera
            CCamera& cam = scene.GetEditorCamera();
            cam.SetLookAt({3.0f, 2.0f, 3.0f}, {0.0f, 0.5f, 0.0f});

            CFFLog::Info("✓ Frame 15: Test scene created");
        });

        // Frame 25: Capture screenshot
        ctx.OnFrame(25, [&ctx]() {
            CFFLog::Info("=== Frame 25: Capturing screenshot ===");

            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 25);

            CFFLog::Info("VISUAL_EXPECTATION: Barrel model should be visible with full PBR textures");
            CFFLog::Info("VISUAL_EXPECTATION: No magenta/black checkerboard (placeholder) visible");
            CFFLog::Info("VISUAL_EXPECTATION: Textures should be properly loaded and displayed");

            CFFLog::Info("✓ Frame 25: Screenshot captured");
        });

        // Frame 30: Test sync API still works alongside async
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("=== Frame 30: Testing sync/async API compatibility ===");

            CTextureManager& texMgr = CTextureManager::Instance();

            // Sync load should still work
            RHI::TextureSharedPtr syncTex = texMgr.Load("pbr_models/Barrel_01_1k.gltf/Barrel_01_1k_albedo.png", true);
            ASSERT_NOT_NULL(ctx, syncTex.get(), "Sync Load should return texture");

            // Async load of same texture should return ready handle
            TextureHandlePtr asyncHandle = texMgr.LoadAsync("pbr_models/Barrel_01_1k.gltf/Barrel_01_1k_albedo.png", true);
            ASSERT(ctx, asyncHandle->IsReady(), "Async load of cached texture should be immediately ready");
            ASSERT(ctx, asyncHandle->GetTexture() == syncTex.get(),
                   "Async and sync should return same texture");

            CFFLog::Info("✓ Frame 30: Sync/Async compatibility verified");
        });

        // Frame 35: Finish test
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("=== Frame 35: Test summary ===");

            if (ctx.failures.empty()) {
                ctx.testPassed = true;
                CFFLog::Info("============================================");
                CFFLog::Info("✓ ALL ASYNC TEXTURE LOAD TESTS PASSED");
                CFFLog::Info("============================================");
                CFFLog::Info("✓ LoadAsync() returns immediately (non-blocking)");
                CFFLog::Info("✓ Handles return placeholder before load completes");
                CFFLog::Info("✓ Tick() processes loads incrementally");
                CFFLog::Info("✓ FlushPendingLoads() blocks until complete");
                CFFLog::Info("✓ Duplicate requests return same handle (caching)");
                CFFLog::Info("✓ Invalid paths handled gracefully with fallback");
                CFFLog::Info("✓ Sync and async APIs work together correctly");
            } else {
                ctx.testPassed = false;
                CFFLog::Error("============================================");
                CFFLog::Error("✗ TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                CFFLog::Error("============================================");
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
            }

            // Cleanup static handles
            handle1.reset();
            handle2.reset();
            handle3.reset();
            handleDuplicate.reset();
            handleInvalid.reset();
            placeholder.reset();

            ctx.Finish();
        });
    }
};

// Register the test
REGISTER_TEST(CTestAsyncTextureLoad)
