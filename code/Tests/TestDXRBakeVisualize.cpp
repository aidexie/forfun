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

using namespace DirectX;

// ============================================
// TestDXRBakeVisualize - Visualize GPU bake results
// ============================================
// Tests GPU path tracing bake and exports SH-reconstructed cubemaps.
//
// Output: E:/forfun/debug/TestDXRBakeVisualize/
//   - sh_reconstructed_brick0_voxel0.ktx2 - SH L2 reconstruction
// ============================================

class CTestDXRBakeVisualize : public ITestCase
{
public:
    const char* GetName() const override { return "TestDXRBakeVisualize"; }

    void Setup(CTestContext& ctx) override
    {
        ctx.OnFrame(5, [&]() {
            CFFLog::Info("[TestDXRBakeVisualize] Frame 5: Starting GPU bake visualization test");

            // Get scene's volumetric lightmap
            CScene& scene = CScene::Instance();
            CVolumetricLightmap& lightmap = scene.GetVolumetricLightmap();

            // Initialize volumetric lightmap with config
            CVolumetricLightmap::Config vlConfig;
            vlConfig.volumeMin = { -10.0f, -1.0f, -10.0f };
            vlConfig.volumeMax = { 10.0f, 5.0f, 10.0f };
            vlConfig.minBrickWorldSize = 2.0f;

            if (!lightmap.Initialize(vlConfig)) {
                ctx.failures.push_back("Failed to initialize volumetric lightmap");
                CFFLog::Error("[TestDXRBakeVisualize] Failed to initialize volumetric lightmap");
                return;
            }

            // Build octree
            lightmap.BuildOctree(scene);

            const auto& bricks = lightmap.GetBricks();
            CFFLog::Info("[TestDXRBakeVisualize] Generated %zu bricks", bricks.size());

            if (bricks.empty()) {
                ctx.failures.push_back("No bricks generated");
                return;
            }

            // Create DXR cubemap baker
            CDXRCubemapBaker baker;
            if (!baker.Initialize()) {
                ctx.failures.push_back("Failed to initialize DXR cubemap baker");
                CFFLog::Error("[TestDXRBakeVisualize] Failed to initialize DXR cubemap baker");
                return;
            }

            // Configure for quick test
            SDXRCubemapBakeConfig config;
            config.maxBounces = 2;
            config.skyIntensity = 1.0f;
            config.debug.logReadbackResults = true;

            // Create output directory
            std::string outputDir = FFPath::GetDebugDir() + "/TestDXRBakeVisualize";
            std::filesystem::create_directories(outputDir);

            // Bake
            CFFLog::Info("[TestDXRBakeVisualize] Starting GPU bake...");
            bool success = baker.BakeVolumetricLightmap(lightmap, scene, config);

            if (!success) {
                ctx.failures.push_back("GPU bake failed");
                CFFLog::Error("[TestDXRBakeVisualize] GPU bake failed");
                return;
            }

            CFFLog::Info("[TestDXRBakeVisualize] GPU bake completed");

            // Export SH-reconstructed cubemaps for visualization
            const auto& bakedBricks = lightmap.GetBricks();
            if (!bakedBricks.empty()) {
                const SBrick& brick = bakedBricks[0];

                CFFLog::Info("[TestDXRBakeVisualize] Brick 0 bounds: (%.2f, %.2f, %.2f) - (%.2f, %.2f, %.2f)",
                    brick.worldMin.x, brick.worldMin.y, brick.worldMin.z,
                    brick.worldMax.x, brick.worldMax.y, brick.worldMax.z);

                // Export SH-reconstructed cubemap for voxel 0
                std::array<XMFLOAT3, 9> shCoeffs;
                for (int i = 0; i < 9; i++) {
                    shCoeffs[i] = brick.shData[0][i];
                }

                // Log SH coefficients
                CFFLog::Info("[TestDXRBakeVisualize] Voxel 0 SH coefficients:");
                CFFLog::Info("  L0: (%.4f, %.4f, %.4f)", shCoeffs[0].x, shCoeffs[0].y, shCoeffs[0].z);
                CFFLog::Info("  L1: (%.4f, %.4f, %.4f), (%.4f, %.4f, %.4f), (%.4f, %.4f, %.4f)",
                    shCoeffs[1].x, shCoeffs[1].y, shCoeffs[1].z,
                    shCoeffs[2].x, shCoeffs[2].y, shCoeffs[2].z,
                    shCoeffs[3].x, shCoeffs[3].y, shCoeffs[3].z);

                // Check if coefficients are valid
                float totalMagnitude = 0.0f;
                for (int i = 0; i < 9; i++) {
                    totalMagnitude += fabsf(shCoeffs[i].x) + fabsf(shCoeffs[i].y) + fabsf(shCoeffs[i].z);
                }

                if (totalMagnitude < 0.001f) {
                    CFFLog::Warning("[TestDXRBakeVisualize] Voxel 0 has near-zero SH");
                } else {
                    // Export SH-reconstructed cubemap
                    SphericalHarmonics::DebugExportSHAsCubemap(shCoeffs, 32, outputDir, "sh_reconstructed_brick0_voxel0");
                    CFFLog::Info("[TestDXRBakeVisualize] Exported sh_reconstructed_brick0_voxel0.ktx2");
                }
            }

            CFFLog::Info("[TestDXRBakeVisualize] Results exported to: %s", outputDir.c_str());
        });

        ctx.OnFrame(15, [&]() {
            CFFLog::Info("[TestDXRBakeVisualize] Test complete");
            ctx.testPassed = ctx.failures.empty();
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestDXRBakeVisualize)
