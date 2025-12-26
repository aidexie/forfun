#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/Mesh.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Engine/Rendering/Lightmap/LightmapTypes.h"
#include "Engine/Rendering/Lightmap/LightmapUV2.h"
#include "Engine/Rendering/Lightmap/LightmapAtlas.h"
#include "Engine/Rendering/Lightmap/LightmapRasterizer.h"
#include "Engine/Rendering/Lightmap/LightmapBaker.h"
#include <DirectXMath.h>

using namespace DirectX;

// ============================================
// Test Helper Functions
// ============================================

// Create a simple cube mesh with proper normals per face
static SMeshCPU_PNT CreateTestCubeMesh(float size = 1.0f) {
    SMeshCPU_PNT mesh;
    float h = size * 0.5f;

    // 24 vertices for proper normals per face
    SVertexPNT vertices[] = {
        // Front face (+Z)
        {-h,-h, h,  0,0,1,  0,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h,-h, h,  0,0,1,  1,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h, h, h,  0,0,1,  1,0,  1,0,0,1,  1,1,1,1, 0,0},
        {-h, h, h,  0,0,1,  0,0,  1,0,0,1,  1,1,1,1, 0,0},
        // Back face (-Z)
        { h,-h,-h,  0,0,-1,  0,1,  -1,0,0,1,  1,1,1,1, 0,0},
        {-h,-h,-h,  0,0,-1,  1,1,  -1,0,0,1,  1,1,1,1, 0,0},
        {-h, h,-h,  0,0,-1,  1,0,  -1,0,0,1,  1,1,1,1, 0,0},
        { h, h,-h,  0,0,-1,  0,0,  -1,0,0,1,  1,1,1,1, 0,0},
        // Top face (+Y)
        {-h, h, h,  0,1,0,  0,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h, h, h,  0,1,0,  1,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h, h,-h,  0,1,0,  1,0,  1,0,0,1,  1,1,1,1, 0,0},
        {-h, h,-h,  0,1,0,  0,0,  1,0,0,1,  1,1,1,1, 0,0},
        // Bottom face (-Y)
        {-h,-h,-h,  0,-1,0,  0,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h,-h,-h,  0,-1,0,  1,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h,-h, h,  0,-1,0,  1,0,  1,0,0,1,  1,1,1,1, 0,0},
        {-h,-h, h,  0,-1,0,  0,0,  1,0,0,1,  1,1,1,1, 0,0},
        // Right face (+X)
        { h,-h, h,  1,0,0,  0,1,  0,0,1,1,  1,1,1,1, 0,0},
        { h,-h,-h,  1,0,0,  1,1,  0,0,1,1,  1,1,1,1, 0,0},
        { h, h,-h,  1,0,0,  1,0,  0,0,1,1,  1,1,1,1, 0,0},
        { h, h, h,  1,0,0,  0,0,  0,0,1,1,  1,1,1,1, 0,0},
        // Left face (-X)
        {-h,-h,-h,  -1,0,0,  0,1,  0,0,-1,1,  1,1,1,1, 0,0},
        {-h,-h, h,  -1,0,0,  1,1,  0,0,-1,1,  1,1,1,1, 0,0},
        {-h, h, h,  -1,0,0,  1,0,  0,0,-1,1,  1,1,1,1, 0,0},
        {-h, h,-h,  -1,0,0,  0,0,  0,0,-1,1,  1,1,1,1, 0,0},
    };

    uint32_t indices[] = {
        0, 1, 2, 0, 2, 3,       // Front
        4, 5, 6, 4, 6, 7,       // Back
        8, 9, 10, 8, 10, 11,    // Top
        12, 13, 14, 12, 14, 15, // Bottom
        16, 17, 18, 16, 18, 19, // Right
        20, 21, 22, 20, 22, 23, // Left
    };

    mesh.vertices.assign(std::begin(vertices), std::end(vertices));
    mesh.indices.assign(std::begin(indices), std::end(indices));

    return mesh;
}

// Create a simple plane mesh
static SMeshCPU_PNT CreateTestPlaneMesh(float size = 2.0f) {
    SMeshCPU_PNT mesh;
    float h = size * 0.5f;

    SVertexPNT vertices[] = {
        {-h, 0, -h,  0,1,0,  0,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h, 0, -h,  0,1,0,  1,1,  1,0,0,1,  1,1,1,1, 0,0},
        { h, 0,  h,  0,1,0,  1,0,  1,0,0,1,  1,1,1,1, 0,0},
        {-h, 0,  h,  0,1,0,  0,0,  1,0,0,1,  1,1,1,1, 0,0},
    };

    uint32_t indices[] = {
        0, 2, 1,  // First triangle
        0, 3, 2,  // Second triangle
    };

    mesh.vertices.assign(std::begin(vertices), std::end(vertices));
    mesh.indices.assign(std::begin(indices), std::end(indices));

    return mesh;
}

// ============================================
// Test Class
// ============================================
class CTestLightmap2D : public ITestCase {
public:
    const char* GetName() const override {
        return "TestLightmap2D";
    }

    void Setup(CTestContext& ctx) override {
        // ============================================
        // Frame 1: Test UV2 Generation
        // ============================================
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("=== TestLightmap2D ===");
            CFFLog::Info("Frame 1: Testing UV2 Generation");

            // Create cube mesh
            SMeshCPU_PNT cubeMesh = CreateTestCubeMesh(2.0f);
            CFFLog::Info("Created cube mesh: %d vertices, %d indices",
                        (int)cubeMesh.vertices.size(),
                        (int)cubeMesh.indices.size());

            // Generate UV2
            SUV2GenerationResult result = GenerateUV2ForMesh(cubeMesh, 16);

            ASSERT(ctx, result.success, "UV2 generation should succeed");
            ASSERT(ctx, result.atlasWidth > 0, "Atlas width should be > 0");
            ASSERT(ctx, result.atlasHeight > 0, "Atlas height should be > 0");
            ASSERT(ctx, result.chartCount > 0, "Chart count should be > 0");
            ASSERT(ctx, !result.uv2.empty(), "UV2 array should not be empty");

            if (result.success) {
                CFFLog::Info("UV2 Generation Results:");
                CFFLog::Info("  Atlas: %dx%d", result.atlasWidth, result.atlasHeight);
                CFFLog::Info("  Charts: %d", result.chartCount);
                CFFLog::Info("  Output vertices: %d", (int)result.positions.size());
                CFFLog::Info("  Output indices: %d", (int)result.indices.size());

                // Verify UV2 values are in [0,1] range
                bool allValid = true;
                for (const auto& uv : result.uv2) {
                    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
                        allValid = false;
                        break;
                    }
                }
                ASSERT(ctx, allValid, "All UV2 values should be in [0,1] range");
            }

            CFFLog::Info("✓ UV2 Generation Test Passed");
        });

        // ============================================
        // Frame 3: Test Atlas Packing
        // ============================================
        ctx.OnFrame(3, [&ctx]() {
            CFFLog::Info("Frame 3: Testing Atlas Packing");

            CLightmapAtlas atlas;
            SLightmapAtlasConfig config;
            config.resolution = 512;
            config.padding = 2;
            config.texelsPerUnit = 16;

            // Simulate packing 4 meshes with different sizes
            std::vector<std::pair<int, int>> meshSizes = {
                {128, 64},   // Mesh 0: wide rectangle
                {64, 128},   // Mesh 1: tall rectangle
                {96, 96},    // Mesh 2: square
                {48, 48},    // Mesh 3: small square
            };

            bool packSuccess = atlas.Pack(meshSizes, config);
            ASSERT(ctx, packSuccess, "Atlas packing should succeed");

            const auto& entries = atlas.GetEntries();
            ASSERT(ctx, entries.size() == 4, "Should have 4 entries");

            if (packSuccess) {
                CFFLog::Info("Atlas Packing Results:");
                CFFLog::Info("  Resolution: %d", atlas.GetAtlasResolution());
                CFFLog::Info("  Atlas count: %d", atlas.GetAtlasCount());

                for (size_t i = 0; i < entries.size(); i++) {
                    const auto& e = entries[i];
                    CFFLog::Info("  Entry %d: pos(%d,%d) size(%dx%d)",
                                (int)i, e.atlasX, e.atlasY, e.width, e.height);

                    // Verify entry is within atlas bounds
                    ASSERT(ctx, e.atlasX >= 0, "Entry X should be >= 0");
                    ASSERT(ctx, e.atlasY >= 0, "Entry Y should be >= 0");
                    ASSERT(ctx, e.atlasX + e.width <= config.resolution, "Entry should fit in atlas width");
                    ASSERT(ctx, e.atlasY + e.height <= config.resolution, "Entry should fit in atlas height");
                }

                // Test scale/offset computation
                XMFLOAT4 scaleOffset = CLightmapAtlas::ComputeScaleOffset(entries[0], config.resolution);
                CFFLog::Info("  Entry 0 scale/offset: (%.4f, %.4f, %.4f, %.4f)",
                            scaleOffset.x, scaleOffset.y, scaleOffset.z, scaleOffset.w);

                ASSERT(ctx, scaleOffset.x > 0 && scaleOffset.x <= 1.0f, "Scale X should be in (0,1]");
                ASSERT(ctx, scaleOffset.y > 0 && scaleOffset.y <= 1.0f, "Scale Y should be in (0,1]");
            }

            CFFLog::Info("✓ Atlas Packing Test Passed");
        });

        // ============================================
        // Frame 5: Test Rasterization
        // ============================================
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("Frame 5: Testing Rasterization");

            CLightmapRasterizer rasterizer;
            const int atlasSize = 64;
            rasterizer.Initialize(atlasSize, atlasSize);

            // Create a simple quad (2 triangles) with UV2 covering full atlas
            std::vector<XMFLOAT3> positions = {
                {0.0f, 0.0f, 0.0f},
                {1.0f, 0.0f, 0.0f},
                {1.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f},
            };

            std::vector<XMFLOAT3> normals = {
                {0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
            };

            std::vector<XMFLOAT2> uv2 = {
                {0.0f, 0.0f},
                {1.0f, 0.0f},
                {1.0f, 1.0f},
                {0.0f, 1.0f},
            };

            std::vector<uint32_t> indices = {
                0, 1, 2,
                0, 2, 3,
            };

            // Identity matrix
            XMMATRIX worldMatrix = XMMatrixIdentity();

            // Rasterize to full atlas
            rasterizer.RasterizeMesh(
                positions, normals, uv2, indices,
                worldMatrix,
                0, 0,  // offset
                atlasSize, atlasSize  // region size
            );

            int validCount = rasterizer.GetValidTexelCount();
            CFFLog::Info("Rasterization Results:");
            CFFLog::Info("  Atlas size: %dx%d", atlasSize, atlasSize);
            CFFLog::Info("  Valid texels: %d / %d", validCount, atlasSize * atlasSize);

            // For a quad covering the full atlas, most texels should be valid
            ASSERT(ctx, validCount > 0, "Should have some valid texels");

            // Check texel data
            const auto& texels = rasterizer.GetTexels();
            ASSERT(ctx, texels.size() == atlasSize * atlasSize, "Texel count should match atlas size");

            // Verify some texels have reasonable world positions
            int validWithGoodPos = 0;
            for (const auto& texel : texels) {
                if (texel.valid) {
                    // World pos should be in [0,1] range for our unit quad
                    if (texel.worldPos.x >= -0.1f && texel.worldPos.x <= 1.1f &&
                        texel.worldPos.z >= -0.1f && texel.worldPos.z <= 1.1f) {
                        validWithGoodPos++;
                    }
                }
            }

            CFFLog::Info("  Valid texels with good positions: %d", validWithGoodPos);
            ASSERT(ctx, validWithGoodPos > 0, "Should have valid texels with correct positions");

            CFFLog::Info("✓ Rasterization Test Passed");
        });

        // ============================================
        // Frame 7: Test Atlas Builder
        // ============================================
        ctx.OnFrame(7, [&ctx]() {
            CFFLog::Info("Frame 7: Testing Atlas Builder");

            CLightmapAtlasBuilder builder;

            // Add simulated mesh infos
            SLightmapMeshInfo mesh1;
            mesh1.meshRendererIndex = 0;
            mesh1.boundsMin = {0.0f, 0.0f, 0.0f};
            mesh1.boundsMax = {2.0f, 2.0f, 2.0f};
            mesh1.hasUV2 = false;
            builder.AddMesh(mesh1);

            SLightmapMeshInfo mesh2;
            mesh2.meshRendererIndex = 1;
            mesh2.boundsMin = {-1.0f, 0.0f, -1.0f};
            mesh2.boundsMax = {1.0f, 0.0f, 1.0f};  // Flat plane
            mesh2.hasUV2 = false;
            builder.AddMesh(mesh2);

            SLightmapAtlasConfig config;
            config.resolution = 256;
            config.texelsPerUnit = 8;
            config.padding = 2;

            bool buildSuccess = builder.Build(config);
            ASSERT(ctx, buildSuccess, "Atlas builder should succeed");

            if (buildSuccess) {
                const auto& lightmapInfos = builder.GetLightmapInfos();
                CFFLog::Info("Atlas Builder Results:");
                CFFLog::Info("  Mesh count: %d", (int)lightmapInfos.size());

                for (size_t i = 0; i < lightmapInfos.size(); i++) {
                    const auto& info = lightmapInfos[i];
                    CFFLog::Info("  Mesh %d: lightmapIndex=%d, scale=(%.3f,%.3f), offset=(%.3f,%.3f)",
                                (int)i, info.lightmapIndex,
                                info.scaleOffset.x, info.scaleOffset.y,
                                info.scaleOffset.z, info.scaleOffset.w);

                    ASSERT(ctx, info.lightmapIndex >= 0, "Lightmap index should be valid");
                }
            }

            CFFLog::Info("✓ Atlas Builder Test Passed");
        });

        // ============================================
        // Frame 10: Test Compute Mesh Lightmap Size
        // ============================================
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("Frame 10: Testing Mesh Lightmap Size Computation");

            // Test 1: 2x2x2 cube at 16 texels/unit = 32x32 on each face
            XMFLOAT3 min1 = {0, 0, 0};
            XMFLOAT3 max1 = {2, 2, 2};
            auto size1 = CLightmapAtlas::ComputeMeshLightmapSize(min1, max1, 16);
            CFFLog::Info("2x2x2 cube @ 16 texels/unit: %dx%d", size1.first, size1.second);
            ASSERT(ctx, size1.first >= 32, "Cube width should be >= 32 texels");
            ASSERT(ctx, size1.second >= 32, "Cube height should be >= 32 texels");

            // Test 2: Flat plane 4x4 at 16 texels/unit
            XMFLOAT3 min2 = {-2, 0, -2};
            XMFLOAT3 max2 = {2, 0, 2};  // Y extent is 0
            auto size2 = CLightmapAtlas::ComputeMeshLightmapSize(min2, max2, 16);
            CFFLog::Info("4x4 plane @ 16 texels/unit: %dx%d", size2.first, size2.second);
            ASSERT(ctx, size2.first >= 4, "Plane width should be >= minSize");
            ASSERT(ctx, size2.second >= 4, "Plane height should be >= minSize");

            // Test 3: Small object should respect minSize
            XMFLOAT3 min3 = {0, 0, 0};
            XMFLOAT3 max3 = {0.1f, 0.1f, 0.1f};
            auto size3 = CLightmapAtlas::ComputeMeshLightmapSize(min3, max3, 16, 8, 512);
            CFFLog::Info("0.1x0.1x0.1 cube @ 16 texels/unit: %dx%d (minSize=8)", size3.first, size3.second);
            ASSERT(ctx, size3.first >= 8, "Small cube should respect minSize");
            ASSERT(ctx, size3.second >= 8, "Small cube should respect minSize");

            // Test 4: Large object should respect maxSize
            XMFLOAT3 min4 = {0, 0, 0};
            XMFLOAT3 max4 = {100, 100, 100};
            auto size4 = CLightmapAtlas::ComputeMeshLightmapSize(min4, max4, 16, 4, 256);
            CFFLog::Info("100x100x100 cube @ 16 texels/unit: %dx%d (maxSize=256)", size4.first, size4.second);
            ASSERT(ctx, size4.first <= 256, "Large cube should respect maxSize");
            ASSERT(ctx, size4.second <= 256, "Large cube should respect maxSize");

            CFFLog::Info("✓ Mesh Lightmap Size Test Passed");
        });

        // ============================================
        // Frame 15: Take Screenshot
        // ============================================
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("Frame 15: Taking screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 15);
        });

        // ============================================
        // Frame 20: Finish Test
        // ============================================
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("=== TestLightmap2D Complete ===");
            CFFLog::Info("All lightmap 2D pipeline tests passed!");
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestLightmap2D)
