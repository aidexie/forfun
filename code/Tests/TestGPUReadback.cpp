#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIResources.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/ShaderCompiler.h"

#include <vector>
#include <cmath>

// Test structure matching compute shader output
struct STestOutput {
    float value;
    float index;
    float padding[2];
};

// Static storage for test buffers
static RHI::IBuffer* s_uavBuffer = nullptr;
static RHI::IBuffer* s_readbackBuffer = nullptr;
static RHI::IShader* s_computeShader = nullptr;
static RHI::IPipelineState* s_computePSO = nullptr;
static RHI::IDescriptorSetLayout* s_dsLayout = nullptr;
static RHI::IDescriptorSet* s_descriptorSet = nullptr;
static uint32_t s_elementCount = 0;

// Simple compute shader that writes index * 10 to each element (SM 5.1 with space)
static const char* s_computeShaderSource = R"(
// Output buffer - structured buffer with float4 elements
RWStructuredBuffer<float4> g_Output : register(u0, space1);

[numthreads(64, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;

    // Write: value = (index + 1) * 10, index = idx
    // e.g., element 0 -> value=10, element 1 -> value=20, etc.
    g_Output[idx] = float4(
        float(idx + 1) * 10.0f,  // value
        float(idx),               // index
        42.0f,                    // marker value
        0.0f                      // padding
    );
}
)";

class CTestGPUReadback : public ITestCase {
public:
    const char* GetName() const override {
        return "TestGPUReadback";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Test basic buffer creation and CPU write/read
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestGPUReadback: Frame 1 - Basic Buffer Test");
            CFFLog::Info("========================================");

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
            ASSERT_NOT_NULL(ctx, rhiCtx, "RHI RenderContext");

            // Test 1: Create a CPU-writable buffer and verify mapping
            CFFLog::Info("Test 1: CPU-writable buffer map/unmap");
            {
                RHI::BufferDesc cbDesc;
                cbDesc.size = sizeof(STestOutput) * 4;
                cbDesc.usage = RHI::EBufferUsage::Constant;
                cbDesc.cpuAccess = RHI::ECPUAccess::Write;
                cbDesc.debugName = "TestCPUWriteBuffer";

                RHI::IBuffer* cpuBuffer = rhiCtx->CreateBuffer(cbDesc, nullptr);
                ASSERT_NOT_NULL(ctx, cpuBuffer, "CPU-writable buffer creation");

                // Write test data
                void* mapped = cpuBuffer->Map();
                ASSERT_NOT_NULL(ctx, mapped, "Map CPU-writable buffer");

                STestOutput* data = static_cast<STestOutput*>(mapped);
                for (int i = 0; i < 4; i++) {
                    data[i].value = (float)(i + 1) * 10.0f;  // 10, 20, 30, 40
                    data[i].index = (float)i;
                }
                cpuBuffer->Unmap();

                CFFLog::Info("  Written values: 10, 20, 30, 40");
                CFFLog::Info("  PASS: CPU-writable buffer works");

                // Cleanup
                delete cpuBuffer;
            }

            CFFLog::Info("Frame 1 complete");
        });

        // Frame 5: Test GPU UAV buffer and readback buffer creation + compile compute shader
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestGPUReadback: Frame 5 - Create Buffers and Compute Shader");
            CFFLog::Info("========================================");

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();

            const uint32_t elementCount = 64;
            const uint32_t bufferSize = elementCount * sizeof(STestOutput);

            // Create UAV buffer (GPU-only)
            CFFLog::Info("Creating UAV buffer (%u bytes, %u elements)", bufferSize, elementCount);
            RHI::BufferDesc uavDesc;
            uavDesc.size = bufferSize;
            uavDesc.usage = RHI::EBufferUsage::UnorderedAccess | RHI::EBufferUsage::Structured;
            uavDesc.cpuAccess = RHI::ECPUAccess::None;
            uavDesc.structureByteStride = sizeof(STestOutput);
            uavDesc.debugName = "TestUAVBuffer";

            s_uavBuffer = rhiCtx->CreateBuffer(uavDesc, nullptr);
            ASSERT_NOT_NULL(ctx, s_uavBuffer, "UAV buffer creation");
            CFFLog::Info("  UAV buffer created: %p", s_uavBuffer);

            // Create readback buffer (CPU-readable)
            CFFLog::Info("Creating readback buffer");
            RHI::BufferDesc readbackDesc;
            readbackDesc.size = bufferSize;
            readbackDesc.usage = RHI::EBufferUsage::Structured;
            readbackDesc.cpuAccess = RHI::ECPUAccess::Read;
            readbackDesc.structureByteStride = sizeof(STestOutput);
            readbackDesc.debugName = "TestReadbackBuffer";

            s_readbackBuffer = rhiCtx->CreateBuffer(readbackDesc, nullptr);
            ASSERT_NOT_NULL(ctx, s_readbackBuffer, "Readback buffer creation");
            CFFLog::Info("  Readback buffer created: %p", s_readbackBuffer);

            s_elementCount = elementCount;

            // Compile compute shader (SM 5.1 for descriptor sets)
            CFFLog::Info("Compiling compute shader...");
            RHI::SCompiledShader compiled = RHI::CompileShaderFromSource(
                s_computeShaderSource,
                "CSMain",
                "cs_5_1",
                nullptr,
                true  // debug
            );

            if (!compiled.success) {
                CFFLog::Error("Compute shader compilation failed: %s", compiled.errorMessage.c_str());
                ASSERT(ctx, false, "Compute shader compilation");
                return;
            }
            CFFLog::Info("  Shader compiled: %zu bytes", compiled.bytecode.size());

            // Create shader object
            RHI::ShaderDesc shaderDesc;
            shaderDesc.type = RHI::EShaderType::Compute;
            shaderDesc.bytecode = compiled.bytecode.data();
            shaderDesc.bytecodeSize = compiled.bytecode.size();

            s_computeShader = rhiCtx->CreateShader(shaderDesc);
            ASSERT_NOT_NULL(ctx, s_computeShader, "Compute shader creation");
            CFFLog::Info("  Compute shader created");

            // Create descriptor set layout for UAV binding (space1)
            RHI::BindingLayoutDesc layoutDesc("TestCompute_PerPass");
            layoutDesc.AddItem(RHI::BindingLayoutItem::Buffer_UAV(0));  // u0 in space1

            s_dsLayout = rhiCtx->CreateDescriptorSetLayout(layoutDesc);
            ASSERT_NOT_NULL(ctx, s_dsLayout, "Descriptor set layout creation");
            CFFLog::Info("  Descriptor set layout created");

            // Allocate descriptor set
            s_descriptorSet = rhiCtx->AllocateDescriptorSet(s_dsLayout);
            ASSERT_NOT_NULL(ctx, s_descriptorSet, "Descriptor set allocation");
            CFFLog::Info("  Descriptor set allocated");

            // Create compute pipeline state with descriptor set layout
            RHI::ComputePipelineDesc psoDesc;
            psoDesc.computeShader = s_computeShader;
            psoDesc.debugName = "TestComputePSO";
            psoDesc.setLayouts[1] = s_dsLayout;  // space1

            s_computePSO = rhiCtx->CreateComputePipelineState(psoDesc);
            ASSERT_NOT_NULL(ctx, s_computePSO, "Compute PSO creation");
            CFFLog::Info("  Compute PSO created");

            CFFLog::Info("Frame 5 complete - buffers and compute shader ready");
        });

        // Frame 10: Dispatch compute shader and copy to readback
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestGPUReadback: Frame 10 - Compute Shader Dispatch");
            CFFLog::Info("========================================");

            if (!s_uavBuffer || !s_readbackBuffer || !s_computePSO) {
                CFFLog::Error("Resources not created!");
                ASSERT_NOT_NULL(ctx, s_uavBuffer, "UAV buffer available");
                ASSERT_NOT_NULL(ctx, s_computePSO, "Compute PSO available");
                return;
            }

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
            auto* cmdList = rhiCtx->GetCommandList();

            // Set compute pipeline state
            CFFLog::Info("Setting compute pipeline state...");
            cmdList->SetPipelineState(s_computePSO);

            // Bind UAV to descriptor set and bind to pipeline
            CFFLog::Info("Binding UAV buffer via descriptor set...");
            s_descriptorSet->Bind({
                RHI::BindingSetItem::Buffer_UAV(0, s_uavBuffer),
            });
            cmdList->BindDescriptorSet(1, s_descriptorSet);

            // Dispatch compute shader
            // 64 elements, 64 threads per group = 1 thread group
            uint32_t threadGroupCount = (s_elementCount + 63) / 64;
            CFFLog::Info("Dispatching compute shader: %u thread groups", threadGroupCount);
            cmdList->Dispatch(threadGroupCount, 1, 1);

            // UAV barrier to ensure compute shader completes
            CFFLog::Info("UAV barrier...");
            cmdList->UAVBarrier(s_uavBuffer);

            // Copy to readback buffer
            CFFLog::Info("Copy UAV to readback buffer...");
            cmdList->CopyBuffer(s_readbackBuffer, 0, s_uavBuffer, 0,
                              s_elementCount * sizeof(STestOutput));

            // Execute and wait
            CFFLog::Info("Executing GPU commands and waiting...");
            rhiCtx->ExecuteAndWait();

            CFFLog::Info("Frame 10 complete - compute shader executed");

            // Verify results immediately
            CFFLog::Info("========================================");
            CFFLog::Info("TestGPUReadback: Verifying Readback");
            CFFLog::Info("========================================");

            // Map readback buffer
            void* mappedData = s_readbackBuffer->Map();
            ASSERT_NOT_NULL(ctx, mappedData, "Map readback buffer");

            CFFLog::Info("Readback buffer mapped: %p", mappedData);

            // Read and verify data
            const STestOutput* data = static_cast<const STestOutput*>(mappedData);

            int correctCount = 0;
            int nonZeroCount = 0;

            CFFLog::Info("First 8 elements:");
            for (uint32_t i = 0; i < 8 && i < s_elementCount; i++) {
                float expectedValue = (float)(i + 1) * 10.0f;
                float expectedIndex = (float)i;

                CFFLog::Info("  [%u] value=%.2f (expect %.2f), index=%.2f (expect %.2f), marker=%.2f",
                           i, data[i].value, expectedValue,
                           data[i].index, expectedIndex,
                           data[i].padding[0]);

                if (data[i].value != 0.0f) nonZeroCount++;
                if (std::abs(data[i].value - expectedValue) < 0.01f &&
                    std::abs(data[i].index - expectedIndex) < 0.01f) {
                    correctCount++;
                }
            }

            s_readbackBuffer->Unmap();

            // Summary
            CFFLog::Info("========================================");
            CFFLog::Info("Results:");
            CFFLog::Info("  Non-zero elements (first 8): %d", nonZeroCount);
            CFFLog::Info("  Correct elements (first 8): %d/8", correctCount);

            if (nonZeroCount == 0) {
                CFFLog::Error("FAIL: All readback data is zero!");
                CFFLog::Error("Possible causes:");
                CFFLog::Error("  1. Compute shader not dispatched");
                CFFLog::Error("  2. UAV binding failed");
                CFFLog::Error("  3. Copy buffer not working");
                CFFLog::Error("  4. GPU commands not executed before readback");
                ASSERT(ctx, false, "Readback data should not be all zeros");
            } else if (correctCount < 8) {
                CFFLog::Warning("PARTIAL: Some data written but values incorrect");
                CFFLog::Info("  Got %d/8 correct values", correctCount);
            } else {
                CFFLog::Info("SUCCESS: Compute shader -> UAV -> Readback working!");
            }

            // Cleanup
            auto* rhiCtx2 = RHI::CRHIManager::Instance().GetRenderContext();
            if (s_descriptorSet) { rhiCtx2->FreeDescriptorSet(s_descriptorSet); s_descriptorSet = nullptr; }
            if (s_dsLayout) { rhiCtx2->DestroyDescriptorSetLayout(s_dsLayout); s_dsLayout = nullptr; }
            delete s_computePSO; s_computePSO = nullptr;
            delete s_computeShader; s_computeShader = nullptr;
            delete s_uavBuffer; s_uavBuffer = nullptr;
            delete s_readbackBuffer; s_readbackBuffer = nullptr;

            CFFLog::Info("========================================");
            CFFLog::Info("TestGPUReadback: Complete");
            CFFLog::Info("========================================");
        });

        // Frame 20: Screenshot and end
        ctx.OnFrame(20, [&ctx]() {
            CFFLog::Info("Test complete, taking screenshot");
            CScreenshot::CaptureTest(ctx.pipeline, "TestGPUReadback", 20);
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestGPUReadback)
