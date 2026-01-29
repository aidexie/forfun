#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/RenderConfig.h"
#include "Editor/EditorContext.h"
#include "Engine/Scene.h"
#include "Engine/SceneLightSettings.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Components/DirectionalLight.h"
#include "Engine/Components/PointLight.h"
#include "Engine/Components/SpotLight.h"
#include <DirectXMath.h>
#include <chrono>
#include <vector>

using namespace DirectX;

/**
 * Test: Deferred Pipeline Stress Test
 *
 * Purpose:
 *   Comprehensive stress test that loads Sponza + multiple glTF models with 100+ dynamic lights
 *   to stress-test all rendering features of the deferred pipeline.
 *
 * Features Tested:
 *   - G-Buffer: 5 render targets (WorldPos, Normal, Albedo, Emissive+MaterialID, Velocity)
 *   - Lighting: Clustered Forward+ (100+ lights), CSM shadows, IBL, Point/Spot lights
 *   - Post-Processing: SSAO, SSR, TAA, Bloom, DoF, Motion Blur, Auto Exposure, Color Grading
 *   - Materials: PBR (Cook-Torrance), Alpha modes, Normal/AO/Emissive maps
 *
 * Scene Layout:
 *   - Sponza at origin (scale 0.01 - it's in cm)
 *   - DamagedHelmet center courtyard (0, 1.5, 0)
 *   - SciFiHelmet left arcade (5, 1.2, -3)
 *   - FlightHelmet right arcade (-5, 1.2, -3)
 *   - MetalRoughSpheres gallery (0, 0.5, 8)
 *   - 4x DamagedHelmet copies at corners
 *
 * Light Configuration:
 *   - 1 Directional Light (Sun) - 4 cascade CSM
 *   - 80 Point Lights - Grid along arcades
 *   - 24 Spot Lights - Focused on key objects
 */
class CTestDeferredStress : public ITestCase {
public:
    const char* GetName() const override {
        return "TestDeferredStress";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Load Sponza environment
        ctx.OnFrame(1, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame1] Loading Sponza environment");

            auto& scene = CScene::Instance();

            // Clear existing scene
            while (scene.GetWorld().Count() > 0) {
                scene.GetWorld().Destroy(0);
            }
            scene.SetSelected(-1);

            // Create Sponza at origin (scale 0.01 - it's in cm)
            auto* sponza = scene.GetWorld().Create("Sponza");
            auto* sponzaT = sponza->AddComponent<STransform>();
            sponzaT->position = {0.0f, 0.0f, 0.0f};
            sponzaT->scale = {0.01f, 0.01f, 0.01f};  // Sponza is in cm
            auto* sponzaMesh = sponza->AddComponent<SMeshRenderer>();
            sponzaMesh->path = "E:/forfun/thirdparty/glTF-Sample-Assets-main/Models/Sponza/glTF/Sponza.gltf";

            m_objectCount = 1;
            CFFLog::Info("[TestDeferredStress:Frame1] Sponza loaded");
        });

        // Frame 2: Add glTF models
        ctx.OnFrame(2, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame2] Adding glTF models");

            auto& scene = CScene::Instance();

            // DamagedHelmet - center courtyard
            auto* helmet1 = scene.GetWorld().Create("DamagedHelmet_Center");
            auto* h1t = helmet1->AddComponent<STransform>();
            h1t->position = {0.0f, 1.5f, 0.0f};
            h1t->scale = {0.5f, 0.5f, 0.5f};
            auto* h1m = helmet1->AddComponent<SMeshRenderer>();
            h1m->path = "E:/forfun/thirdparty/glTF-Sample-Assets-main/Models/DamagedHelmet/glTF/DamagedHelmet.gltf";
            m_objectCount++;

            // SciFiHelmet - left arcade
            auto* scifi = scene.GetWorld().Create("SciFiHelmet");
            auto* scifiT = scifi->AddComponent<STransform>();
            scifiT->position = {5.0f, 1.2f, -3.0f};
            scifiT->scale = {0.3f, 0.3f, 0.3f};
            auto* scifiM = scifi->AddComponent<SMeshRenderer>();
            scifiM->path = "E:/forfun/thirdparty/glTF-Sample-Assets-main/Models/SciFiHelmet/glTF/SciFiHelmet.gltf";
            m_objectCount++;

            // FlightHelmet - right arcade
            auto* flight = scene.GetWorld().Create("FlightHelmet");
            auto* flightT = flight->AddComponent<STransform>();
            flightT->position = {-5.0f, 1.2f, -3.0f};
            flightT->scale = {1.5f, 1.5f, 1.5f};
            auto* flightM = flight->AddComponent<SMeshRenderer>();
            flightM->path = "E:/forfun/thirdparty/glTF-Sample-Assets-main/Models/FlightHelmet/glTF/FlightHelmet.gltf";
            m_objectCount++;

            // MetalRoughSpheres - gallery
            auto* spheres = scene.GetWorld().Create("MetalRoughSpheres");
            auto* spheresT = spheres->AddComponent<STransform>();
            spheresT->position = {0.0f, 0.5f, 8.0f};
            spheresT->scale = {0.3f, 0.3f, 0.3f};
            auto* spheresM = spheres->AddComponent<SMeshRenderer>();
            spheresM->path = "E:/forfun/thirdparty/glTF-Sample-Assets-main/Models/MetalRoughSpheres/glTF/MetalRoughSpheres.gltf";
            m_objectCount++;

            // 4x DamagedHelmet copies at corners for stress testing
            XMFLOAT3 cornerPositions[] = {
                {8.0f, 2.0f, 6.0f},
                {-8.0f, 2.0f, 6.0f},
                {8.0f, 2.0f, -6.0f},
                {-8.0f, 2.0f, -6.0f}
            };

            for (int i = 0; i < 4; ++i) {
                char name[64];
                snprintf(name, sizeof(name), "DamagedHelmet_Corner%d", i);

                auto* corner = scene.GetWorld().Create(name);
                auto* cornerT = corner->AddComponent<STransform>();
                cornerT->position = cornerPositions[i];
                cornerT->scale = {0.4f, 0.4f, 0.4f};
                auto* cornerM = corner->AddComponent<SMeshRenderer>();
                cornerM->path = "E:/forfun/thirdparty/glTF-Sample-Assets-main/Models/DamagedHelmet/glTF/DamagedHelmet.gltf";
                m_objectCount++;
            }

            CFFLog::Info("[TestDeferredStress:Frame2] Added %d objects total", m_objectCount);
        });

        // Frame 3: Add directional light (sun)
        ctx.OnFrame(3, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame3] Adding directional light");

            auto& scene = CScene::Instance();

            auto* sunObj = scene.GetWorld().Create("Sun");
            auto* sunT = sunObj->AddComponent<STransform>();
            sunT->SetRotation(-45.0f, 30.0f, 0.0f);
            auto* sun = sunObj->AddComponent<SDirectionalLight>();
            sun->color = XMFLOAT3(1.0f, 0.95f, 0.9f);
            sun->intensity = 3.0f;
            sun->ibl_intensity = 0.5f;

            m_objectCount++;
            CFFLog::Info("[TestDeferredStress:Frame3] Directional light added");
        });

        // Frame 4: Add 80 point lights (grid along arcades)
        ctx.OnFrame(4, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame4] Adding 80 point lights");

            auto& scene = CScene::Instance();

            // Point lights along Sponza arcades
            // 10 columns x 2 sides x 4 heights = 80 lights
            float columnSpacing = 2.5f;  // Distance between columns
            float startX = -12.0f;
            float sideZ[] = {-4.0f, 4.0f};  // Left and right arcades
            float heights[] = {1.0f, 3.0f, 5.0f, 7.0f};

            // Color palette for variety
            XMFLOAT3 warmColors[] = {
                {1.0f, 0.8f, 0.4f},   // Warm yellow
                {1.0f, 0.6f, 0.3f},   // Orange
                {1.0f, 0.4f, 0.2f},   // Red-orange
                {0.9f, 0.7f, 0.5f}    // Tan
            };

            int lightIndex = 0;
            for (int col = 0; col < 10; ++col) {
                for (int side = 0; side < 2; ++side) {
                    for (int h = 0; h < 4; ++h) {
                        char name[64];
                        snprintf(name, sizeof(name), "PointLight_%d", lightIndex);

                        auto* lightObj = scene.GetWorld().Create(name);
                        auto* t = lightObj->AddComponent<STransform>();
                        t->position = XMFLOAT3(
                            startX + col * columnSpacing,
                            heights[h],
                            sideZ[side]
                        );

                        auto* light = lightObj->AddComponent<SPointLight>();
                        // Cycle through colors
                        light->color = warmColors[(col + side + h) % 4];
                        light->intensity = 2.0f + (h * 0.5f);  // Brighter at top
                        light->range = 5.0f;

                        m_pointLightCount++;
                        lightIndex++;
                    }
                }
            }

            m_objectCount += m_pointLightCount;
            CFFLog::Info("[TestDeferredStress:Frame4] Added %d point lights", m_pointLightCount);
        });

        // Frame 5: Add 24 spot lights (focused on key objects)
        ctx.OnFrame(5, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame5] Adding 24 spot lights");

            auto& scene = CScene::Instance();

            // Spot lights focusing on key objects
            struct SpotConfig {
                XMFLOAT3 position;
                XMFLOAT3 direction;
                XMFLOAT3 color;
                float intensity;
            };

            // 8 spotlights on center helmet (circular arrangement)
            for (int i = 0; i < 8; ++i) {
                float angle = i * (XM_2PI / 8.0f);
                float radius = 3.0f;

                char name[64];
                snprintf(name, sizeof(name), "SpotLight_Center%d", i);

                auto* spotObj = scene.GetWorld().Create(name);
                auto* t = spotObj->AddComponent<STransform>();
                t->position = XMFLOAT3(
                    cosf(angle) * radius,
                    4.0f,
                    sinf(angle) * radius
                );

                auto* spot = spotObj->AddComponent<SSpotLight>();
                // Direction toward center (0, 1.5, 0)
                XMFLOAT3 toCenter = {
                    -t->position.x,
                    1.5f - t->position.y,
                    -t->position.z
                };
                // Normalize direction
                float len = sqrtf(toCenter.x * toCenter.x + toCenter.y * toCenter.y + toCenter.z * toCenter.z);
                spot->direction = {toCenter.x / len, toCenter.y / len, toCenter.z / len};
                spot->color = XMFLOAT3(1.0f, 0.95f, 0.9f);
                spot->intensity = 10.0f;
                spot->range = 8.0f;
                spot->innerConeAngle = 15.0f;
                spot->outerConeAngle = 30.0f;

                m_spotLightCount++;
            }

            // 4 spotlights on each corner helmet (16 total)
            XMFLOAT3 cornerTargets[] = {
                {8.0f, 2.0f, 6.0f},
                {-8.0f, 2.0f, 6.0f},
                {8.0f, 2.0f, -6.0f},
                {-8.0f, 2.0f, -6.0f}
            };

            XMFLOAT3 spotColors[] = {
                {1.0f, 0.5f, 0.3f},   // Orange
                {0.3f, 0.8f, 1.0f},   // Cyan
                {1.0f, 0.3f, 0.5f},   // Pink
                {0.5f, 1.0f, 0.3f}    // Lime
            };

            for (int c = 0; c < 4; ++c) {
                for (int i = 0; i < 4; ++i) {
                    float angle = i * (XM_2PI / 4.0f) + XM_PIDIV4;
                    float radius = 2.0f;

                    char name[64];
                    snprintf(name, sizeof(name), "SpotLight_Corner%d_%d", c, i);

                    auto* spotObj = scene.GetWorld().Create(name);
                    auto* t = spotObj->AddComponent<STransform>();
                    t->position = XMFLOAT3(
                        cornerTargets[c].x + cosf(angle) * radius,
                        cornerTargets[c].y + 2.5f,
                        cornerTargets[c].z + sinf(angle) * radius
                    );

                    auto* spot = spotObj->AddComponent<SSpotLight>();
                    XMFLOAT3 toTarget = {
                        cornerTargets[c].x - t->position.x,
                        cornerTargets[c].y - t->position.y,
                        cornerTargets[c].z - t->position.z
                    };
                    float len = sqrtf(toTarget.x * toTarget.x + toTarget.y * toTarget.y + toTarget.z * toTarget.z);
                    spot->direction = {toTarget.x / len, toTarget.y / len, toTarget.z / len};
                    spot->color = spotColors[c];
                    spot->intensity = 8.0f;
                    spot->range = 6.0f;
                    spot->innerConeAngle = 12.0f;
                    spot->outerConeAngle = 25.0f;

                    m_spotLightCount++;
                }
            }

            m_objectCount += m_spotLightCount;
            CFFLog::Info("[TestDeferredStress:Frame5] Added %d spot lights", m_spotLightCount);
            CFFLog::Info("[TestDeferredStress:Frame5] Total lights: %d (1 dir + %d point + %d spot)",
                1 + m_pointLightCount + m_spotLightCount, m_pointLightCount, m_spotLightCount);
        });

        // Frame 10: Configure post-processing
        ctx.OnFrame(10, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame10] Configuring post-processing");

            auto& settings = CScene::Instance().GetLightSettings();
            auto& showFlags = CEditorContext::Instance().GetShowFlags();

            // Enable all post-processing effects
            showFlags.Lighting = true;
            showFlags.Shadows = true;
            showFlags.IBL = true;
            showFlags.ClusteredLighting = true;
            showFlags.PostProcessing = true;

            // SSAO (GTAO)
            showFlags.SSAO = true;

            // SSR (requires HiZ)
            showFlags.HiZ = true;
            showFlags.SSR = true;

            // Bloom
            showFlags.Bloom = true;
            settings.bloom.threshold = 1.2f;
            settings.bloom.intensity = 0.8f;
            settings.bloom.scatter = 0.65f;

            // TAA
            showFlags.TAA = true;

            // Depth of Field
            showFlags.DepthOfField = true;
            settings.depthOfField.focusDistance = 8.0f;
            settings.depthOfField.focalRange = 4.0f;
            settings.depthOfField.aperture = 4.0f;
            settings.depthOfField.maxBlurRadius = 6.0f;

            // Motion Blur
            showFlags.MotionBlur = true;
            settings.motionBlur.intensity = 0.3f;
            settings.motionBlur.sampleCount = 10;
            settings.motionBlur.maxBlurPixels = 24.0f;

            // Auto Exposure
            showFlags.AutoExposure = true;
            settings.autoExposure.minEV = -2.0f;
            settings.autoExposure.maxEV = 4.0f;
            settings.autoExposure.adaptSpeedUp = 1.5f;
            settings.autoExposure.adaptSpeedDown = 3.0f;

            // Color Grading - Cinematic look
            showFlags.ColorGrading = true;
            settings.colorGrading.ApplyPreset(EColorGradingPreset::Cinematic);

            // Anti-aliasing (FXAA as backup with TAA)
            showFlags.AntiAliasing = true;
            settings.antiAliasing.mode = EAntiAliasingMode::FXAA;

            CFFLog::Info("[TestDeferredStress:Frame10] Post-processing enabled: SSAO, SSR, Bloom, TAA, DoF, MotionBlur, AutoExposure, ColorGrading, FXAA");
        });

        // Frame 15: Start benchmark and capture camera position 1 (overview)
        ctx.OnFrame(15, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame15] Starting benchmark - Camera 1: Overview");

            m_benchmarkStartTime = std::chrono::high_resolution_clock::now();
            m_benchmarkStartFrame = 15;

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {-12.0f, 6.0f, -10.0f},  // Eye: outside Sponza
                {0.0f, 2.0f, 0.0f},       // Target: center
                {0.0f, 1.0f, 0.0f}
            );
        });

        // Frame 30: Screenshot camera 1
        ctx.OnFrame(30, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame30] Capturing overview screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 30);
            CFFLog::Info("VISUAL_EXPECTATION: Sponza overview with multiple colored lights");
        });

        // Frame 35: Camera 2 - Center courtyard (looking at DamagedHelmet)
        ctx.OnFrame(35, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame35] Camera 2: Center courtyard");

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {3.0f, 2.5f, 3.0f},
                {0.0f, 1.5f, 0.0f},
                {0.0f, 1.0f, 0.0f}
            );
        });

        // Frame 50: Screenshot camera 2
        ctx.OnFrame(50, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame50] Capturing courtyard screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 50);
            CFFLog::Info("VISUAL_EXPECTATION: DamagedHelmet in center with spot lights and DoF blur");
        });

        // Frame 55: Camera 3 - Left arcade (SciFiHelmet)
        ctx.OnFrame(55, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame55] Camera 3: Left arcade");

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {7.0f, 2.0f, -1.0f},
                {5.0f, 1.2f, -3.0f},
                {0.0f, 1.0f, 0.0f}
            );
        });

        // Frame 70: Screenshot camera 3
        ctx.OnFrame(70, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame70] Capturing left arcade screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 70);
            CFFLog::Info("VISUAL_EXPECTATION: SciFiHelmet with warm point light colors");
        });

        // Frame 75: Camera 4 - Right arcade (FlightHelmet)
        ctx.OnFrame(75, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame75] Camera 4: Right arcade");

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {-7.0f, 2.0f, -1.0f},
                {-5.0f, 1.2f, -3.0f},
                {0.0f, 1.0f, 0.0f}
            );
        });

        // Frame 90: Screenshot camera 4
        ctx.OnFrame(90, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame90] Capturing right arcade screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 90);
            CFFLog::Info("VISUAL_EXPECTATION: FlightHelmet with transparency materials");
        });

        // Frame 95: Camera 5 - Gallery (MetalRoughSpheres)
        ctx.OnFrame(95, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame95] Camera 5: Gallery");

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {-2.0f, 1.5f, 6.0f},
                {0.0f, 0.5f, 8.0f},
                {0.0f, 1.0f, 0.0f}
            );
        });

        // Frame 110: Screenshot camera 5
        ctx.OnFrame(110, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame110] Capturing gallery screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 110);
            CFFLog::Info("VISUAL_EXPECTATION: MetalRoughSpheres showing PBR material variations");
        });

        // Frame 115: Camera 6 - Top-down view
        ctx.OnFrame(115, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame115] Camera 6: Top-down view");

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {0.0f, 15.0f, 0.0f},
                {0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 1.0f}
            );
        });

        // Frame 130: Screenshot camera 6
        ctx.OnFrame(130, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame130] Capturing top-down screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 130);
            CFFLog::Info("VISUAL_EXPECTATION: Bird's eye view showing all 100+ lights as points");
        });

        // Frame 135: Camera 7 - Long corridor view
        ctx.OnFrame(135, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame135] Camera 7: Long corridor view");

            auto& scene = CScene::Instance();
            scene.GetEditorCamera().SetLookAt(
                {-14.0f, 3.0f, 0.0f},
                {14.0f, 3.0f, 0.0f},
                {0.0f, 1.0f, 0.0f}
            );
        });

        // Frame 150: End benchmark and log results
        ctx.OnFrame(150, [this, &ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame150] Capturing corridor screenshot and ending benchmark");

            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 150);
            CFFLog::Info("VISUAL_EXPECTATION: Long view with all point lights visible in rows");

            // Calculate benchmark metrics
            auto endTime = std::chrono::high_resolution_clock::now();
            int framesRendered = 150 - m_benchmarkStartFrame;

            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                endTime - m_benchmarkStartTime);
            double totalSeconds = duration.count() / 1000000.0;
            double avgFPS = framesRendered / totalSeconds;
            double avgFrameTimeMs = (totalSeconds * 1000.0) / framesRendered;

            // Get pipeline type
            SRenderConfig config;
            SRenderConfig::Load(SRenderConfig::GetDefaultPath(), config);
            const char* pipelineType = (config.pipeline == ERenderPipeline::Deferred)
                ? "Deferred" : "Forward";

            // Log benchmark results
            CFFLog::Info("========================================");
            CFFLog::Info("BENCHMARK RESULTS: %s Pipeline Stress Test", pipelineType);
            CFFLog::Info("========================================");
            CFFLog::Info("Scene Complexity:");
            CFFLog::Info("  Objects: %d", m_objectCount);
            CFFLog::Info("  Point Lights: %d", m_pointLightCount);
            CFFLog::Info("  Spot Lights: %d", m_spotLightCount);
            CFFLog::Info("  Total Lights: %d", 1 + m_pointLightCount + m_spotLightCount);
            CFFLog::Info("Performance:");
            CFFLog::Info("  Frames rendered: %d", framesRendered);
            CFFLog::Info("  Total time: %.2f seconds", totalSeconds);
            CFFLog::Info("  Average FPS: %.1f", avgFPS);
            CFFLog::Info("  Average frame time: %.2f ms", avgFrameTimeMs);
            CFFLog::Info("Post-Processing: SSAO, SSR, Bloom, TAA, DoF, MotionBlur, AutoExposure, ColorGrading, FXAA");
            CFFLog::Info("========================================");

            // Performance metric for parsing
            CFFLog::Info("PERF_METRIC: pipeline=%s fps=%.1f frametime=%.2fms objects=%d lights=%d",
                pipelineType, avgFPS, avgFrameTimeMs, m_objectCount,
                1 + m_pointLightCount + m_spotLightCount);

            // Assertions
            ASSERT(ctx, m_pointLightCount >= 80, "Should have 80+ point lights");
            ASSERT(ctx, m_spotLightCount >= 24, "Should have 24+ spot lights");
            ASSERT(ctx, m_objectCount >= 10, "Should have 10+ objects");

            // FPS target (30+ with 100+ lights)
            if (avgFPS >= 30.0) {
                CFFLog::Info("PERF_PASS: FPS >= 30 target met (%.1f FPS)", avgFPS);
            } else {
                CFFLog::Warning("PERF_WARN: FPS below 30 target (%.1f FPS)", avgFPS);
            }
        });

        // Frame 160: Finish test
        ctx.OnFrame(160, [&ctx]() {
            CFFLog::Info("[TestDeferredStress:Frame160] Finalizing test");

            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Deferred pipeline stress test completed");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                for (const auto& failure : ctx.failures) {
                    CFFLog::Error("  - %s", failure.c_str());
                }
                ctx.testPassed = false;
            }

            ctx.Finish();
        });
    }

private:
    std::chrono::high_resolution_clock::time_point m_benchmarkStartTime;
    int m_benchmarkStartFrame = 0;
    int m_objectCount = 0;
    int m_pointLightCount = 0;
    int m_spotLightCount = 0;
};

REGISTER_TEST(CTestDeferredStress)
