#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/Mesh.h"
#include "Engine/Rendering/Lightmap/LightmapTypes.h"
#include "Engine/Rendering/Lightmap/LightmapUV2.h"
#include <DirectXMath.h>

using namespace DirectX;

// Helper: Create a simple cube mesh for testing
static SMeshCPU_PNT CreateCubeMesh() {
    SMeshCPU_PNT mesh;

    // Cube vertices (24 vertices for proper normals per face)
    // SVertexPNT: {px,py,pz, nx,ny,nz, u,v, tx,ty,tz,tw, r,g,b,a}
    SVertexPNT vertices[] = {
        // Front face (normal +Z)
        {-1,-1, 1,  0,0,1,  0,1,  1,0,0,1,  1,1,1,1},
        { 1,-1, 1,  0,0,1,  1,1,  1,0,0,1,  1,1,1,1},
        { 1, 1, 1,  0,0,1,  1,0,  1,0,0,1,  1,1,1,1},
        {-1, 1, 1,  0,0,1,  0,0,  1,0,0,1,  1,1,1,1},
        // Back face (normal -Z)
        { 1,-1,-1,  0,0,-1,  0,1,  -1,0,0,1,  1,1,1,1},
        {-1,-1,-1,  0,0,-1,  1,1,  -1,0,0,1,  1,1,1,1},
        {-1, 1,-1,  0,0,-1,  1,0,  -1,0,0,1,  1,1,1,1},
        { 1, 1,-1,  0,0,-1,  0,0,  -1,0,0,1,  1,1,1,1},
        // Top face (normal +Y)
        {-1, 1, 1,  0,1,0,  0,1,  1,0,0,1,  1,1,1,1},
        { 1, 1, 1,  0,1,0,  1,1,  1,0,0,1,  1,1,1,1},
        { 1, 1,-1,  0,1,0,  1,0,  1,0,0,1,  1,1,1,1},
        {-1, 1,-1,  0,1,0,  0,0,  1,0,0,1,  1,1,1,1},
        // Bottom face (normal -Y)
        {-1,-1,-1,  0,-1,0,  0,1,  1,0,0,1,  1,1,1,1},
        { 1,-1,-1,  0,-1,0,  1,1,  1,0,0,1,  1,1,1,1},
        { 1,-1, 1,  0,-1,0,  1,0,  1,0,0,1,  1,1,1,1},
        {-1,-1, 1,  0,-1,0,  0,0,  1,0,0,1,  1,1,1,1},
        // Right face (normal +X)
        { 1,-1, 1,  1,0,0,  0,1,  0,0,1,1,  1,1,1,1},
        { 1,-1,-1,  1,0,0,  1,1,  0,0,1,1,  1,1,1,1},
        { 1, 1,-1,  1,0,0,  1,0,  0,0,1,1,  1,1,1,1},
        { 1, 1, 1,  1,0,0,  0,0,  0,0,1,1,  1,1,1,1},
        // Left face (normal -X)
        {-1,-1,-1,  -1,0,0,  0,1,  0,0,-1,1,  1,1,1,1},
        {-1,-1, 1,  -1,0,0,  1,1,  0,0,-1,1,  1,1,1,1},
        {-1, 1, 1,  -1,0,0,  1,0,  0,0,-1,1,  1,1,1,1},
        {-1, 1,-1,  -1,0,0,  0,0,  0,0,-1,1,  1,1,1,1},
    };

    // Indices (6 faces × 2 triangles × 3 vertices)
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

class CTestLightmapUV2 : public ITestCase {
public:
    const char* GetName() const override {
        return "TestLightmapUV2";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Test UV2 generation for a simple cube
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("=== TestLightmapUV2 ===");
            CFFLog::Info("Frame 1: Testing UV2 generation for cube mesh");

            // Create cube mesh
            SMeshCPU_PNT cubeMesh = CreateCubeMesh();

            CFFLog::Info("Cube mesh created: %d vertices, %d indices",
                        (int)cubeMesh.vertices.size(),
                        (int)cubeMesh.indices.size());

            // Generate UV2
            SUV2GenerationResult result = GenerateUV2ForMesh(cubeMesh, 16);

            ASSERT(ctx, result.success, "UV2 generation succeeded");

            if (result.success) {
                CFFLog::Info("UV2 generation successful:");
                CFFLog::Info("  - Atlas size: %dx%d", result.atlasWidth, result.atlasHeight);
                CFFLog::Info("  - Chart count: %d", result.chartCount);
                CFFLog::Info("  - Output vertices: %d (was %d)",
                            (int)result.positions.size(),
                            (int)cubeMesh.vertices.size());
                CFFLog::Info("  - Output indices: %d", (int)result.indices.size());

                // Verify UV2 coordinates are in valid range [0,1]
                bool uv2Valid = true;
                int invalidCount = 0;
                for (size_t i = 0; i < result.uv2.size(); i++) {
                    const auto& uv = result.uv2[i];
                    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
                        uv2Valid = false;
                        invalidCount++;
                        if (invalidCount <= 5) {
                            CFFLog::Error("Invalid UV2[%d]: (%.3f, %.3f)", (int)i, uv.x, uv.y);
                        }
                    }
                }

                ASSERT(ctx, uv2Valid, "All UV2 coordinates in [0,1] range");

                // Verify HasValidUV2 function
                bool hasValidUV2 = HasValidUV2(result.uv2, result.indices);
                ASSERT(ctx, hasValidUV2, "HasValidUV2 returns true");

                CFFLog::Info("✓ UV2 validation passed");
            }
        });

        // Frame 5: Test UV2 generation with raw vertex data (programmatic plane)
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("Frame 5: Testing UV2 generation for programmatic plane");

            // Create a simple plane (2 triangles)
            std::vector<XMFLOAT3> positions = {
                {-1.0f, 0.0f, -1.0f},  // 0: bottom-left
                { 1.0f, 0.0f, -1.0f},  // 1: bottom-right
                { 1.0f, 0.0f,  1.0f},  // 2: top-right
                {-1.0f, 0.0f,  1.0f},  // 3: top-left
            };

            std::vector<XMFLOAT3> normals = {
                {0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f},
            };

            std::vector<XMFLOAT2> uvs = {
                {0.0f, 1.0f},
                {1.0f, 1.0f},
                {1.0f, 0.0f},
                {0.0f, 0.0f},
            };

            std::vector<uint32_t> indices = {
                0, 2, 1,  // First triangle
                0, 3, 2,  // Second triangle
            };

            SUV2GenerationResult result = GenerateUV2(positions, normals, uvs, indices, 32);

            ASSERT(ctx, result.success, "Plane UV2 generation succeeded");

            if (result.success) {
                CFFLog::Info("Plane UV2 generation successful:");
                CFFLog::Info("  - Atlas size: %dx%d", result.atlasWidth, result.atlasHeight);
                CFFLog::Info("  - Chart count: %d", result.chartCount);
                CFFLog::Info("  - Output vertices: %d", (int)result.positions.size());

                // For a simple plane, should have 1 chart
                ASSERT(ctx, result.chartCount >= 1, "Plane has at least 1 chart");

                // Print UV2 values for debugging
                CFFLog::Info("UV2 coordinates:");
                for (size_t i = 0; i < result.uv2.size(); i++) {
                    CFFLog::Info("  UV2[%d]: (%.4f, %.4f)", (int)i, result.uv2[i].x, result.uv2[i].y);
                }
            }
        });

        // Frame 10: Take screenshot and finish
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("Frame 10: Test complete");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 10);
        });

        ctx.OnFrame(15, [&ctx]() {
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestLightmapUV2)
