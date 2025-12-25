#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/SphericalHarmonics.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Engine/Scene.h"
#include "Engine/Rendering/VolumetricLightmap.h"
#include "Engine/Rendering/RayTracing/DXRCubemapBaker.h"
#include <DirectXMath.h>
#include <filesystem>
#include <memory>

using namespace DirectX;

// ============================================
// TestDXRCubemapBaker - Test cubemap-based GPU bake
// ============================================
// Uses the new cubemap-based approach: 32x32x6 = 6144 rays per voxel.
// Matches CPU baker sampling for correctness validation.
//
// Frame 5: Build AS, pipeline, buffers (PrepareBakeResources)
// Frame 10: Dispatch rays and readback (DispatchBakeAllVoxels)
//
// Output: E:/forfun/debug/TestDXRCubemapBaker/
//   - cubemap_brick*_voxel*.ktx2 - Raw radiance cubemaps
//   - sh_reconstructed_*.ktx2 - SH-reconstructed cubemaps
// ============================================

class CTestDXRCubemapBaker : public ITestCase
{
public:
    const char* GetName() const override { return "TestDXRCubemapBaker"; }

    void Setup(CTestContext& ctx) override
    {
        ctx.OnFrame(1, [&]() {
            std::string scenePath = FFPath::GetAbsolutePath("scenes/volumetric_lightmap_test.scene");
            CScene::Instance().LoadFromFile(scenePath);
        });

        // Frame 5: Build AS, pipeline, buffers
        ctx.OnFrame(5, [&]() {
            CFFLog::Info("[TestDXRCubemapBaker] Frame 5: Building resources...");

            // Get scene's volumetric lightmap
            CScene& scene = CScene::Instance();
            CVolumetricLightmap& lightmap = scene.GetVolumetricLightmap();

            // Initialize volumetric lightmap with config
            CVolumetricLightmap::Config vlConfig;
            vlConfig.volumeMin = { -10.0f, 0.0f, -10.0f };
            vlConfig.volumeMax = { 10.0f, 10.0f, 10.0f };
            vlConfig.minBrickWorldSize = 10.0f;  // Larger bricks for faster test

            if (!lightmap.Initialize(vlConfig)) {
                ctx.failures.push_back("Failed to initialize volumetric lightmap");
                CFFLog::Error("[TestDXRCubemapBaker] Failed to initialize volumetric lightmap");
                return;
            }

            // Build octree
            lightmap.BuildOctree(scene);

            const auto& bricks = lightmap.GetBricks();
            CFFLog::Info("[TestDXRCubemapBaker] Generated %zu bricks", bricks.size());

            if (bricks.empty()) {
                ctx.failures.push_back("No bricks generated");
                return;
            }

            // Create cubemap baker
            m_baker = std::make_unique<CDXRCubemapBaker>();
            if (!m_baker->Initialize()) {
                ctx.failures.push_back("Failed to initialize cubemap baker");
                CFFLog::Error("[TestDXRCubemapBaker] Failed to initialize cubemap baker");
                return;
            }

            // Configure
            m_config.cubemapResolution = 32;  // 32x32x6 = 6144 rays per voxel
            m_config.maxBounces = 2;
            m_config.skyIntensity = 1.0f;

            // Enable debug cubemap export
            m_config.debug.exportDebugCubemaps = true;
            m_config.debug.maxDebugCubemaps = 1;  // Export first 3 valid voxels
            m_config.debug.debugExportPath = FFPath::GetDebugDir() + "/TestDXRCubemapBaker";
            m_config.debug.logDispatchInfo = true;
            m_config.debug.logReadbackResults = true;
            m_config.debug.exportSHToText = true;

            // Create output directory
            std::filesystem::create_directories(m_config.debug.debugExportPath);

            // Phase 1: Build AS, pipeline, buffers
            CFFLog::Info("[TestDXRCubemapBaker] Building acceleration structures and pipeline...");
            bool success = m_baker->BakeVolumetricLightmap(lightmap, scene, m_config);

            if (!success) {
                ctx.failures.push_back("Failed to prepare bake resources");
                CFFLog::Error("[TestDXRCubemapBaker] Failed to prepare bake resources");
                return;
            }

            CFFLog::Info("[TestDXRCubemapBaker] Frame 5: Resources built successfully");
        });

        // Frame 10: Dispatch rays and readback
        ctx.OnFrame(10, [&]() {
            CFFLog::Info("[TestDXRCubemapBaker] Frame 10: Dispatching rays...");

            if (!m_baker) {
                ctx.failures.push_back("Baker not initialized");
                return;
            }

            CScene& scene = CScene::Instance();
            CVolumetricLightmap& lightmap = scene.GetVolumetricLightmap();

            // Phase 2: Dispatch bake for all voxels
            CFFLog::Info("[TestDXRCubemapBaker] Rays per voxel: %u (32x32x6)",
                m_config.cubemapResolution * m_config.cubemapResolution * 6);

            bool success = m_baker->DispatchBakeAllVoxels(lightmap, m_config);

            if (!success) {
                ctx.failures.push_back("GPU bake dispatch failed");
                CFFLog::Error("[TestDXRCubemapBaker] GPU bake dispatch failed");
                return;
            }

            CFFLog::Info("[TestDXRCubemapBaker] GPU bake completed");

            // Verify results
            const auto& bakedBricks = lightmap.GetBricks();
            if (!bakedBricks.empty()) {
                const SBrick& brick = bakedBricks[0];

                CFFLog::Info("[TestDXRCubemapBaker] Brick 0 bounds: (%.2f, %.2f, %.2f) - (%.2f, %.2f, %.2f)",
                    brick.worldMin.x, brick.worldMin.y, brick.worldMin.z,
                    brick.worldMax.x, brick.worldMax.y, brick.worldMax.z);

                // Find first valid voxel and log its SH
                for (int voxelIdx = VL_BRICK_VOXEL_COUNT-1; voxelIdx < VL_BRICK_VOXEL_COUNT; voxelIdx++) {
                    if (brick.validity[voxelIdx]) {
                        std::array<XMFLOAT3, 9> shCoeffs;
                        for (int i = 0; i < 9; i++) {
                            shCoeffs[i] = brick.shData[voxelIdx][i];
                        }

                        CFFLog::Info("[TestDXRCubemapBaker] First valid voxel %d SH coefficients:", voxelIdx);
                        CFFLog::Info("  L0: (%.4f, %.4f, %.4f)", shCoeffs[0].x, shCoeffs[0].y, shCoeffs[0].z);
                        CFFLog::Info("  L1: (%.4f, %.4f, %.4f), (%.4f, %.4f, %.4f), (%.4f, %.4f, %.4f)",
                            shCoeffs[1].x, shCoeffs[1].y, shCoeffs[1].z,
                            shCoeffs[2].x, shCoeffs[2].y, shCoeffs[2].z,
                            shCoeffs[3].x, shCoeffs[3].y, shCoeffs[3].z);

                        break;  // Only log first valid voxel
                    }
                }
            }

            CFFLog::Info("[TestDXRCubemapBaker] Results exported to: %s", m_config.debug.debugExportPath.c_str());
        });

        ctx.OnFrame(15, [&]() {
            CFFLog::Info("[TestDXRCubemapBaker] Test complete");
            m_baker.reset();  // Clean up
            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }

private:
    std::unique_ptr<CDXRCubemapBaker> m_baker;
    SDXRCubemapBakeConfig m_config;
};

REGISTER_TEST(CTestDXRCubemapBaker)
