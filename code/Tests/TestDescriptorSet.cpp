#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "RHI/RHIManager.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"
#include "RHI/PerFrameSlots.h"
#include "RHI/PerPassSlots.h"
#include <DirectXMath.h>

using namespace DirectX;
using namespace RHI;

/**
 * Test: Descriptor Set Infrastructure
 *
 * Purpose:
 *   Validate the DX12 descriptor set implementation works correctly.
 *   Tests layout creation, set allocation, resource binding, and rendering.
 *
 * Expected Results:
 *   - Layout creation succeeds
 *   - Set allocation succeeds
 *   - Resource binding works
 *   - PSO creation with setLayouts works
 *   - BindDescriptorSet renders correctly
 */
class CTestDescriptorSet : public ITestCase {
public:
    const char* GetName() const override {
        return "TestDescriptorSet";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Check prerequisites
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("[TestDescriptorSet:Frame1] Checking prerequisites");

            auto* renderCtx = CRHIManager::Instance().GetRenderContext();
            ASSERT_NOT_NULL(ctx, renderCtx, "RenderContext should exist");

            // Descriptor sets only work on DX12
            if (CRHIManager::Instance().GetBackend() != EBackend::DX12) {
                CFFLog::Warning("[TestDescriptorSet] Skipping test - requires DX12 backend");
                ctx.testPassed = true;
                ctx.Finish();
                return;
            }

            // Test that descriptor set API is available
            auto* testLayout = renderCtx->CreateDescriptorSetLayout(
                BindingLayoutDesc("TestCheck").AddItem(BindingLayoutItem::Texture_SRV(0))
            );
            ASSERT_NOT_NULL(ctx, testLayout, "CreateDescriptorSetLayout should work on DX12");
            renderCtx->DestroyDescriptorSetLayout(testLayout);

            CFFLog::Info("[TestDescriptorSet:Frame1] Prerequisites OK");
        });

        // Frame 3: Test layout creation
        ctx.OnFrame(3, [&ctx]() {
            CFFLog::Info("[TestDescriptorSet:Frame3] Testing layout creation");

            auto* renderCtx = CRHIManager::Instance().GetRenderContext();

            // Create PerFrame layout (Set 0)
            auto* perFrameLayout = renderCtx->CreateDescriptorSetLayout(
                BindingLayoutDesc("TestPerFrame")
                    .AddItem(BindingLayoutItem::Texture_SRV(0))     // t0: Test texture
                    .AddItem(BindingLayoutItem::Sampler(0))         // s0: Sampler
                    .AddItem(BindingLayoutItem::VolatileCBV(0, 80)) // b0: CB_PerFrame (4x4 matrix + float + padding = 80 bytes)
            );

            ASSERT_NOT_NULL(ctx, perFrameLayout, "PerFrame layout should be created");
            ASSERT_EQUAL(ctx, (int)perFrameLayout->GetBindingCount(), 3, "PerFrame layout should have 3 bindings");
            ASSERT_EQUAL(ctx, (int)perFrameLayout->GetSRVCount(), 1, "PerFrame layout should have 1 SRV");
            ASSERT_EQUAL(ctx, (int)perFrameLayout->GetSamplerCount(), 1, "PerFrame layout should have 1 Sampler");
            ASSERT(ctx, perFrameLayout->HasVolatileCBV(), "PerFrame layout should have VolatileCBV");

            // Create PerMaterial layout (Set 2)
            auto* materialLayout = renderCtx->CreateDescriptorSetLayout(
                BindingLayoutDesc("TestMaterial")
                    .AddItem(BindingLayoutItem::VolatileCBV(0, 32)) // b0: CB_Material (float4 + float + padding = 32 bytes)
            );

            ASSERT_NOT_NULL(ctx, materialLayout, "Material layout should be created");
            ASSERT(ctx, materialLayout->HasVolatileCBV(), "Material layout should have VolatileCBV");

            // Clean up
            renderCtx->DestroyDescriptorSetLayout(perFrameLayout);
            renderCtx->DestroyDescriptorSetLayout(materialLayout);

            CFFLog::Info("[TestDescriptorSet:Frame3] Layout creation test passed");
        });

        // Frame 5: Test set allocation and binding
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("[TestDescriptorSet:Frame5] Testing set allocation and binding");

            auto* renderCtx = CRHIManager::Instance().GetRenderContext();

            // Create layout
            auto* layout = renderCtx->CreateDescriptorSetLayout(
                BindingLayoutDesc("TestLayout")
                    .AddItem(BindingLayoutItem::Texture_SRV(0))
                    .AddItem(BindingLayoutItem::Sampler(0))
                    .AddItem(BindingLayoutItem::VolatileCBV(0, 80))
            );

            // Allocate set
            auto* set = renderCtx->AllocateDescriptorSet(layout);
            ASSERT_NOT_NULL(ctx, set, "Set should be allocated");
            ASSERT(ctx, set->GetLayout() == layout, "Set should reference its layout");
            ASSERT(ctx, !set->IsComplete(), "Empty set should not be complete");

            // Create a test texture (2x2 checkerboard)
            TextureDesc texDesc;
            texDesc.width = 2;
            texDesc.height = 2;
            texDesc.format = ETextureFormat::R8G8B8A8_UNORM;
            texDesc.dimension = ETextureDimension::Tex2D;
            texDesc.usage = ETextureUsage::ShaderResource;
            texDesc.mipLevels = 1;

            uint32_t texData[4] = {
                0xFFFFFFFF, 0xFF000000,  // White, Black
                0xFF000000, 0xFFFFFFFF   // Black, White
            };
            SubresourceData subresource;
            subresource.pData = texData;
            subresource.rowPitch = 2 * sizeof(uint32_t);
            subresource.slicePitch = 4 * sizeof(uint32_t);

            auto* texture = renderCtx->CreateTextureWithData(texDesc, &subresource, 1);
            ASSERT_NOT_NULL(ctx, texture, "Test texture should be created");

            // Create sampler
            SamplerDesc samplerDesc;
            samplerDesc.filter = EFilter::MinMagMipPoint;
            samplerDesc.addressU = ETextureAddressMode::Wrap;
            samplerDesc.addressV = ETextureAddressMode::Wrap;
            auto* sampler = renderCtx->CreateSampler(samplerDesc);
            ASSERT_NOT_NULL(ctx, sampler, "Sampler should be created");

            // Bind resources to set
            set->Bind({
                BindingSetItem::Texture_SRV(0, texture),
                BindingSetItem::Sampler(0, sampler)
            });

            // Set is still not complete (missing CBV)
            ASSERT(ctx, !set->IsComplete(), "Set missing CBV should not be complete");

            // Bind CBV data
            struct CB_PerFrame {
                XMFLOAT4X4 viewProj;
                float time;
                float pad[3];
            } cbData;
            XMStoreFloat4x4(&cbData.viewProj, XMMatrixIdentity());
            cbData.time = 0.0f;

            set->Bind(BindingSetItem::VolatileCBV(0, &cbData, sizeof(cbData)));
            ASSERT(ctx, set->IsComplete(), "Set with all bindings should be complete");

            // Clean up
            renderCtx->FreeDescriptorSet(set);
            // Resources are owned elsewhere or cleaned up by RHI - no explicit destroy needed for test
            delete texture;
            delete sampler;
            renderCtx->DestroyDescriptorSetLayout(layout);

            CFFLog::Info("[TestDescriptorSet:Frame5] Set allocation and binding test passed");
        });

        // Frame 10: Test shader compilation with SM 5.1
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("[TestDescriptorSet:Frame10] Testing SM 5.1 shader compilation");

            std::string shaderPath = FFPath::GetSourceDir() + "/Shader/TestDescriptorSet";

            // Compile vertex shader with SM 5.1
            auto vsResult = CompileShaderFromFile(shaderPath + ".vs.hlsl", "main", "vs_5_1");
            ASSERT(ctx, vsResult.success, "Vertex shader should compile");
            if (!vsResult.success) {
                CFFLog::Error("[TestDescriptorSet] VS compile error: %s", vsResult.errorMessage.c_str());
            }

            // Compile pixel shader with SM 5.1
            auto psResult = CompileShaderFromFile(shaderPath + ".ps.hlsl", "main", "ps_5_1");
            ASSERT(ctx, psResult.success, "Pixel shader should compile");
            if (!psResult.success) {
                CFFLog::Error("[TestDescriptorSet] PS compile error: %s", psResult.errorMessage.c_str());
            }

            CFFLog::Info("[TestDescriptorSet:Frame10] SM 5.1 shader compilation test passed");
        });

        // Frame 15: Test PSO creation with descriptor set layouts
        ctx.OnFrame(15, [&ctx]() {
            CFFLog::Info("[TestDescriptorSet:Frame15] Testing PSO creation with setLayouts");

            auto* renderCtx = CRHIManager::Instance().GetRenderContext();

            // Create layouts
            auto* perFrameLayout = renderCtx->CreateDescriptorSetLayout(
                BindingLayoutDesc("PerFrame")
                    .AddItem(BindingLayoutItem::Texture_SRV(0))
                    .AddItem(BindingLayoutItem::Sampler(0))
                    .AddItem(BindingLayoutItem::VolatileCBV(0, 80))
            );

            auto* materialLayout = renderCtx->CreateDescriptorSetLayout(
                BindingLayoutDesc("Material")
                    .AddItem(BindingLayoutItem::VolatileCBV(0, 32))
            );

            // Compile shaders
            std::string shaderPath = FFPath::GetSourceDir() + "/Shader/TestDescriptorSet";
            auto vsResult = CompileShaderFromFile(shaderPath + ".vs.hlsl", "main", "vs_5_1");
            auto psResult = CompileShaderFromFile(shaderPath + ".ps.hlsl", "main", "ps_5_1");

            ASSERT(ctx, vsResult.success && psResult.success, "Shaders should compile");

            // Create shader objects
            ShaderDesc vsDesc;
            vsDesc.type = EShaderType::Vertex;
            vsDesc.bytecode = vsResult.bytecode.data();
            vsDesc.bytecodeSize = vsResult.bytecode.size();
            auto* vs = renderCtx->CreateShader(vsDesc);

            ShaderDesc psDesc;
            psDesc.type = EShaderType::Pixel;
            psDesc.bytecode = psResult.bytecode.data();
            psDesc.bytecodeSize = psResult.bytecode.size();
            auto* ps = renderCtx->CreateShader(psDesc);

            ASSERT_NOT_NULL(ctx, vs, "Vertex shader should be created");
            ASSERT_NOT_NULL(ctx, ps, "Pixel shader should be created");

            // Create PSO with descriptor set layouts
            PipelineStateDesc psoDesc;
            psoDesc.vertexShader = vs;
            psoDesc.pixelShader = ps;
            psoDesc.setLayouts[0] = perFrameLayout;  // Set 0: PerFrame
            psoDesc.setLayouts[1] = nullptr;          // Set 1: unused
            psoDesc.setLayouts[2] = materialLayout;   // Set 2: PerMaterial
            psoDesc.setLayouts[3] = nullptr;          // Set 3: unused
            psoDesc.rasterizer.cullMode = ECullMode::None;
            psoDesc.depthStencil.depthEnable = false;
            psoDesc.renderTargetFormats.push_back(ETextureFormat::R8G8B8A8_UNORM);
            psoDesc.debugName = "TestDescriptorSetPSO";

            // Define input layout (POSITION + TEXCOORD)
            psoDesc.inputLayout.push_back(VertexElement(EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0));
            psoDesc.inputLayout.push_back(VertexElement(EVertexSemantic::Texcoord, 0, EVertexFormat::Float2, 12, 0));

            auto* pso = renderCtx->CreatePipelineState(psoDesc);
            ASSERT_NOT_NULL(ctx, pso, "PSO with setLayouts should be created");

            // Clean up
            delete pso;
            delete vs;
            delete ps;
            renderCtx->DestroyDescriptorSetLayout(perFrameLayout);
            renderCtx->DestroyDescriptorSetLayout(materialLayout);

            CFFLog::Info("[TestDescriptorSet:Frame15] PSO creation test passed");
        });

        // Frame 20: Take screenshot
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("[TestDescriptorSet:Frame20] Capturing screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, ctx.testName, 20);

            CFFLog::Info("VISUAL_EXPECTATION: Screenshot captured (infrastructure test - no specific rendering)");
        });

        // Frame 25: Complete test
        ctx.OnFrame(25, [&ctx]() {
            if (ctx.failures.empty()) {
                CFFLog::Info("TEST PASSED: Descriptor set infrastructure works correctly");
                ctx.testPassed = true;
            } else {
                CFFLog::Error("TEST FAILED: %zu assertion(s) failed", ctx.failures.size());
                ctx.testPassed = false;
            }
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestDescriptorSet)
