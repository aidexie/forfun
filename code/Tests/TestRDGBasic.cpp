#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/FFLog.h"
#include "Core/RDG/RDGBuilder.h"

using namespace RDG;

/**
 * Test: RDG Basic API
 *
 * Purpose:
 *   Verify that the RDG (Render Dependency Graph) basic API works correctly.
 *   Tests pass registration, resource creation, and dependency tracking.
 *
 * Expected Results:
 *   - CRDGBuilder creates and manages passes
 *   - RDGTextureHandle and RDGBufferHandle are type-safe
 *   - Pass dependencies are correctly recorded
 *   - Graph compiles without errors
 */
class CTestRDGBasic : public ITestCase {
public:
    const char* GetName() const override {
        return "TestRDGBasic";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Test basic handle creation
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestRDGBasic:Frame1] Testing handle creation");

            CRDGBuilder rdg;
            rdg.BeginFrame(1);

            // Create transient textures
            auto albedo = rdg.CreateTexture("GBuffer.Albedo",
                RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));
            auto normal = rdg.CreateTexture("GBuffer.Normal",
                RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT));
            auto depth = rdg.CreateTexture("GBuffer.Depth",
                RDGTextureDesc::CreateDepthStencil(1280, 720, DXGI_FORMAT_D32_FLOAT));

            ASSERT(ctx, albedo.IsValid(), "Albedo handle should be valid");
            ASSERT(ctx, normal.IsValid(), "Normal handle should be valid");
            ASSERT(ctx, depth.IsValid(), "Depth handle should be valid");

            // Verify handles have different indices
            ASSERT(ctx, albedo.GetIndex() == 0, "Albedo should be index 0");
            ASSERT(ctx, normal.GetIndex() == 1, "Normal should be index 1");
            ASSERT(ctx, depth.GetIndex() == 2, "Depth should be index 2");

            // Verify frame ID
            ASSERT(ctx, albedo.GetFrameId() == 1, "Albedo should have frame ID 1");

            CFFLog::Info("[TestRDGBasic:Frame1] Handle creation test passed");
        });

        // Frame 2: Test pass registration with dependencies
        ctx.OnFrame(2, [&ctx]() {
            CFFLog::Info("[TestRDGBasic:Frame2] Testing pass registration");

            CRDGBuilder rdg;
            rdg.BeginFrame(2);

            // Define pass data structures
            struct FGBufferPassData {
                RDGTextureHandle Albedo;
                RDGTextureHandle Normal;
                RDGTextureHandle Depth;
            };

            struct FLightingPassData {
                RDGTextureHandle Albedo;
                RDGTextureHandle Normal;
                RDGTextureHandle Depth;
                RDGTextureHandle HDROutput;
            };

            struct FToneMapPassData {
                RDGTextureHandle HDRInput;
                RDGTextureHandle LDROutput;
            };

            // Create resources and register passes
            RDGTextureHandle albedo, normal, depth, hdrOutput, ldrOutput;

            // GBuffer Pass
            rdg.AddPass<FGBufferPassData>("GBuffer",
                [&](FGBufferPassData& data, RDGPassBuilder& builder) {
                    data.Albedo = builder.CreateTexture("GBuffer.Albedo",
                        RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));
                    data.Normal = builder.CreateTexture("GBuffer.Normal",
                        RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT));
                    data.Depth = builder.CreateTexture("GBuffer.Depth",
                        RDGTextureDesc::CreateDepthStencil(1280, 720, DXGI_FORMAT_D32_FLOAT));

                    builder.WriteRTV(data.Albedo);
                    builder.WriteRTV(data.Normal);
                    builder.WriteDSV(data.Depth);

                    // Export for later passes
                    albedo = data.Albedo;
                    normal = data.Normal;
                    depth = data.Depth;
                },
                [](const FGBufferPassData& data, RDGContext& ctx) {
                    CFFLog::Info("[TestRDGBasic] GBuffer pass executed");
                }
            );

            // Lighting Pass
            rdg.AddPass<FLightingPassData>("Lighting",
                [&](FLightingPassData& data, RDGPassBuilder& builder) {
                    data.Albedo = builder.ReadTexture(albedo);
                    data.Normal = builder.ReadTexture(normal);
                    data.Depth = builder.ReadTexture(depth);
                    data.HDROutput = builder.CreateTexture("HDR.Output",
                        RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT));

                    builder.WriteRTV(data.HDROutput);
                    hdrOutput = data.HDROutput;
                },
                [](const FLightingPassData& data, RDGContext& ctx) {
                    CFFLog::Info("[TestRDGBasic] Lighting pass executed");
                }
            );

            // ToneMap Pass
            rdg.AddPass<FToneMapPassData>("ToneMap",
                [&](FToneMapPassData& data, RDGPassBuilder& builder) {
                    data.HDRInput = builder.ReadTexture(hdrOutput);
                    data.LDROutput = builder.CreateTexture("LDR.Output",
                        RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));

                    builder.WriteRTV(data.LDROutput);
                    ldrOutput = data.LDROutput;
                },
                [](const FToneMapPassData& data, RDGContext& ctx) {
                    CFFLog::Info("[TestRDGBasic] ToneMap pass executed");
                }
            );

            // Verify passes registered
            ASSERT(ctx, rdg.GetPasses().size() == 3, "Should have 3 passes");
            ASSERT(ctx, rdg.GetTextures().size() == 5, "Should have 5 textures");

            // Verify pass names
            ASSERT(ctx, std::string(rdg.GetPasses()[0]->Name) == "GBuffer", "Pass 0 should be GBuffer");
            ASSERT(ctx, std::string(rdg.GetPasses()[1]->Name) == "Lighting", "Pass 1 should be Lighting");
            ASSERT(ctx, std::string(rdg.GetPasses()[2]->Name) == "ToneMap", "Pass 2 should be ToneMap");

            // Verify dependencies recorded
            ASSERT(ctx, rdg.GetPasses()[0]->TextureAccesses.size() == 3, "GBuffer should have 3 texture accesses");
            ASSERT(ctx, rdg.GetPasses()[1]->TextureAccesses.size() == 4, "Lighting should have 4 texture accesses");
            ASSERT(ctx, rdg.GetPasses()[2]->TextureAccesses.size() == 2, "ToneMap should have 2 texture accesses");

            // Dump graph for debugging
            rdg.DumpGraph();

            CFFLog::Info("[TestRDGBasic:Frame2] Pass registration test passed");
        });

        // Frame 3: Test compilation
        ctx.OnFrame(3, [&ctx]() {
            CFFLog::Info("[TestRDGBasic:Frame3] Testing graph compilation");

            CRDGBuilder rdg;
            rdg.BeginFrame(3);

            struct FSimplePassData {
                RDGTextureHandle Output;
            };

            rdg.AddPass<FSimplePassData>("PassA",
                [](FSimplePassData& data, RDGPassBuilder& builder) {
                    data.Output = builder.CreateTexture("OutputA",
                        RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));
                    builder.WriteRTV(data.Output);
                },
                [](const FSimplePassData& data, RDGContext& ctx) {}
            );

            rdg.AddPass<FSimplePassData>("PassB",
                [](FSimplePassData& data, RDGPassBuilder& builder) {
                    data.Output = builder.CreateTexture("OutputB",
                        RDGTextureDesc::CreateRenderTarget(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));
                    builder.WriteRTV(data.Output);
                },
                [](const FSimplePassData& data, RDGContext& ctx) {}
            );

            // Compile should succeed
            rdg.Compile();

            CFFLog::Info("[TestRDGBasic:Frame3] Compilation test passed");
        });

        // Frame 4: Test buffer creation
        ctx.OnFrame(4, [&ctx]() {
            CFFLog::Info("[TestRDGBasic:Frame4] Testing buffer creation");

            CRDGBuilder rdg;
            rdg.BeginFrame(4);

            auto structuredBuffer = rdg.CreateBuffer("LightBuffer",
                RDGBufferDesc::CreateStructured(100, sizeof(float) * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
            auto rawBuffer = rdg.CreateBuffer("RawBuffer",
                RDGBufferDesc::CreateRaw(1024));

            ASSERT(ctx, structuredBuffer.IsValid(), "Structured buffer should be valid");
            ASSERT(ctx, rawBuffer.IsValid(), "Raw buffer should be valid");
            ASSERT(ctx, rdg.GetBuffers().size() == 2, "Should have 2 buffers");

            CFFLog::Info("[TestRDGBasic:Frame4] Buffer creation test passed");
        });

        // Frame 10: End test
        ctx.OnFrame(10, [&ctx]() {
            if (ctx.failures.empty()) {
                ctx.testPassed = true;
                CFFLog::Info("[TestRDGBasic] ✓ ALL TESTS PASSED!");
            } else {
                CFFLog::Error("[TestRDGBasic] TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestRDGBasic)
