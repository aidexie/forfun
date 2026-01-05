#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/RenderConfig.h"
#include "Engine/Scene.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Components/PointLight.h"
#include <DirectXMath.h>
#include <chrono>

using namespace DirectX;

/**
 * Test: Deferred Rendering Performance Benchmark
 *
 * Purpose:
 *   Measure rendering performance of the current pipeline (Forward or Deferred).
 *   Run with both pipeline configs to compare:
 *     - render.json: "pipeline": "Forward"
 *     - render.json: "pipeline": "Deferred"
 *
 * Metrics:
 *   - Average FPS over benchmark frames
 *   - Frame time (ms)
 *   - Scene complexity (objects, lights)
 *
 * Usage:
 *   1. Set render.json pipeline to "Forward", run test, note FPS
 *   2. Set render.json pipeline to "Deferred", run test, note FPS
 *   3. Compare results
 */
class CTestDeferredPerf : public ITestCase {
public:
    const char* GetName() const override {
        return "TestDeferredPerf";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Create benchmark scene with multiple lights
        ctx.OnFrame(1, [this, &ctx]() {
            CFFLog::Info("[TestDeferredPerf:Frame1] Setting up benchmark scene");

            auto& scene = CScene::Instance();

            // Directional light
            auto* lightObj = scene.GetWorld().Create("DirectionalLight");
            auto* lightTransform = lightObj->AddComponent<STransform>();
            lightTransform->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* dirLight = lightObj->AddComponent<SDirectionalLight>();
            dirLight->color = XMFLOAT3(1.0f, 0.98f, 0.95f);
            dirLight->intensity = 2.0f;

            // Create grid of objects (5x5 = 25 objects)
            const int gridSize = 5;
            const float spacing = 3.0f;
            m_objectCount = 0;

            for (int x = 0; x < gridSize; ++x) {
                for (int z = 0; z < gridSize; ++z) {
                    char name[64];
                    snprintf(name, sizeof(name), "Sphere_%d_%d", x, z);

                    auto* sphere = scene.GetWorld().Create(name);
                    auto* t = sphere->AddComponent<STransform>();
                    t->position = XMFLOAT3(
                        (x - gridSize/2) * spacing,
                        0.0f,
                        (z - gridSize/2) * spacing + 10.0f
                    );
                    t->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);

                    auto* mesh = sphere->AddComponent<SMeshRenderer>();
                    mesh->path = "sphere.obj";

                    m_objectCount++;
                }
            }

            // Create point lights (16 lights in a grid)
            const int lightGridSize = 4;
            m_lightCount = 0;

            for (int x = 0; x < lightGridSize; ++x) {
                for (int z = 0; z < lightGridSize; ++z) {
                    char name[64];
                    snprintf(name, sizeof(name), "PointLight_%d_%d", x, z);

                    auto* lightObj = scene.GetWorld().Create(name);
                    auto* t = lightObj->AddComponent<STransform>();
                    t->position = XMFLOAT3(
                        (x - lightGridSize/2) * spacing * 1.5f,
                        2.0f,
                        (z - lightGridSize/2) * spacing * 1.5f + 10.0f
                    );

                    auto* pointLight = lightObj->AddComponent<SPointLight>();
                    // Vary colors
                    float r = (x % 2 == 0) ? 1.0f : 0.3f;
                    float g = (z % 2 == 0) ? 1.0f : 0.3f;
                    float b = ((x + z) % 2 == 0) ? 1.0f : 0.3f;
                    pointLight->color = XMFLOAT3(r, g, b);
                    pointLight->intensity = 5.0f;
                    pointLight->range = 10.0f;

                    m_lightCount++;
                }
            }

            // Ground plane
            auto* ground = scene.GetWorld().Create("Ground");
            auto* groundT = ground->AddComponent<STransform>();
            groundT->position = XMFLOAT3(0.0f, -1.5f, 10.0f);
            groundT->scale = XMFLOAT3(20.0f, 0.1f, 20.0f);
            auto* groundM = ground->AddComponent<SMeshRenderer>();
            groundM->path = "cube.obj";
            m_objectCount++;

            CFFLog::Info("[TestDeferredPerf:Frame1] Scene created: %d objects, %d point lights",
                m_objectCount, m_lightCount);
        });

        // Frame 10: Start benchmark timing
        ctx.OnFrame(10, [this]() {
            CFFLog::Info("[TestDeferredPerf:Frame10] Starting benchmark...");
            m_benchmarkStartTime = std::chrono::high_resolution_clock::now();
            m_benchmarkStartFrame = 10;
        });

        // Frame 110: End benchmark (100 frames measured)
        ctx.OnFrame(110, [this, &ctx]() {
            auto endTime = std::chrono::high_resolution_clock::now();
            int framesRendered = 110 - m_benchmarkStartFrame;

            // Calculate metrics
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                endTime - m_benchmarkStartTime);
            double totalSeconds = duration.count() / 1000000.0;
            double avgFPS = framesRendered / totalSeconds;
            double avgFrameTimeMs = (totalSeconds * 1000.0) / framesRendered;

            // Get pipeline type from config
            SRenderConfig config;
            SRenderConfig::Load(SRenderConfig::GetDefaultPath(), config);
            const char* pipelineType = (config.pipeline == ERenderPipeline::Deferred)
                ? "Deferred" : "Forward";

            // Log results
            CFFLog::Info("========================================");
            CFFLog::Info("BENCHMARK RESULTS: %s Pipeline", pipelineType);
            CFFLog::Info("========================================");
            CFFLog::Info("Scene: %d objects, %d point lights", m_objectCount, m_lightCount);
            CFFLog::Info("Frames rendered: %d", framesRendered);
            CFFLog::Info("Total time: %.2f seconds", totalSeconds);
            CFFLog::Info("Average FPS: %.1f", avgFPS);
            CFFLog::Info("Average frame time: %.2f ms", avgFrameTimeMs);
            CFFLog::Info("========================================");

            // Take screenshot
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 110);

            // Store for comparison (written to test log)
            CFFLog::Info("PERF_METRIC: pipeline=%s fps=%.1f frametime=%.2fms objects=%d lights=%d",
                pipelineType, avgFPS, avgFrameTimeMs, m_objectCount, m_lightCount);
        });

        // Frame 115: Finish test
        ctx.OnFrame(115, [&ctx]() {
            CFFLog::Info("TEST PASSED: Benchmark complete");
            CFFLog::Info("Compare results by running with different pipeline configs:");
            CFFLog::Info("  1. render.json: \"pipeline\": \"Forward\"");
            CFFLog::Info("  2. render.json: \"pipeline\": \"Deferred\"");
            ctx.testPassed = true;
            ctx.Finish();
        });
    }

private:
    std::chrono::high_resolution_clock::time_point m_benchmarkStartTime;
    int m_benchmarkStartFrame = 0;
    int m_objectCount = 0;
    int m_lightCount = 0;
};

REGISTER_TEST(CTestDeferredPerf)
