#include "Core/Testing/TestCase.h"
#include "Core/Testing/TestRegistry.h"
#include "Core/Testing/Screenshot.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIResources.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/RHIRayTracing.h"
#include "RHI/ShaderCompiler.h"

// DX12-specific includes (only needed for DX12 context check)
#include "RHI/DX12/DX12Context.h"

#include <cmath>
#include <vector>

// ============================================
// Test Mode Selection
// ============================================
enum class ETestMode {
    RayTracing = 1,  // Mode 1: DXR dispatch rays
    Compute = 2      // Mode 2: Compute shader dispatch
};

// Change this to switch between test modes
static const ETestMode s_testMode = ETestMode::RayTracing;

// Output buffer layout (shared between both modes):
// [0]: Magic value (0xDEADBEEF)
// [4-67]: Results per thread (16 uint32_t)
// [68-131]: Additional data per thread (16 float)
static const uint32_t OUTPUT_BUFFER_SIZE = 132;  // 4 + 64 + 64 bytes

// Static test resources (shared)
static RHI::IBuffer* s_outputBuffer = nullptr;
static RHI::IBuffer* s_readbackBuffer = nullptr;
static RHI::IBuffer* s_constantBuffer = nullptr;

// Ray tracing resources (Mode 1)
static RHI::IRayTracingPipelineState* s_pipeline = nullptr;
static RHI::IShaderBindingTable* s_sbt = nullptr;
static RHI::IShader* s_shaderLib = nullptr;

// Compute shader resources (Mode 2)
static RHI::IShader* s_computeShader = nullptr;
static RHI::IPipelineState* s_computePSO = nullptr;

// Acceleration structure resources
static RHI::IBuffer* s_cubeVertexBuffer = nullptr;
static RHI::IBuffer* s_cubeIndexBuffer = nullptr;
static RHI::IBuffer* s_blasScratchBuffer = nullptr;
static RHI::IBuffer* s_blasResultBuffer = nullptr;
static RHI::IBuffer* s_tlasScratchBuffer = nullptr;
static RHI::IBuffer* s_tlasResultBuffer = nullptr;
static RHI::IBuffer* s_tlasInstanceBuffer = nullptr;
static RHI::IAccelerationStructure* s_blas = nullptr;
static RHI::IAccelerationStructure* s_tlas = nullptr;

static const uint32_t DISPATCH_WIDTH = 4;
static const uint32_t DISPATCH_HEIGHT = 4;
static const uint32_t DISPATCH_DEPTH = 1;
static const uint32_t TOTAL_THREADS = DISPATCH_WIDTH * DISPATCH_HEIGHT * DISPATCH_DEPTH;

// DXR shader with simple ray tracing against TLAS
static const char* s_minimalShaderSource = R"(
// Simple DXR test - traces rays against TLAS and reports hit/miss

struct SRayPayload {
    float3 color;
    float hitT;      // Distance to hit, -1 if miss
};

// TLAS at t0
RaytracingAccelerationStructure g_Scene : register(t0);

// Output buffer at u0
RWByteAddressBuffer g_Output : register(u0);

[shader("raygeneration")]
void MinimalRayGen() {
    // Write magic value first to confirm shader execution
    g_Output.Store(0, 0xDEADBEEFu);

    uint3 threadId = DispatchRaysIndex();
    uint3 dims = DispatchRaysDimensions();
    uint linearIdx = threadId.x + threadId.y * dims.x;

    // Setup ray - shoot from camera position toward cube at origin
    // Camera at (0, 0, -3), looking at (0, 0, 0)
    float3 rayOrigin = float3(0.0f, 0.0f, -3.0f);

    // Compute ray direction based on thread ID (simple grid pattern)
    float u = (float(threadId.x) + 0.5f) / float(dims.x) - 0.5f;  // [-0.5, 0.5]
    float v = (float(threadId.y) + 0.5f) / float(dims.y) - 0.5f;  // [-0.5, 0.5]
    float3 rayDir = normalize(float3(u, v, 1.0f));  // Looking toward +Z

    // Initialize payload
    SRayPayload payload;
    payload.color = float3(0, 0, 0);
    payload.hitT = -1.0f;

    // Trace ray
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = rayDir;
    ray.TMin = 0.001f;
    ray.TMax = 1000.0f;

    // TraceRay parameters:
    // - AccelerationStructure
    // - RayFlags (use RAY_FLAG_NONE for simple test)
    // - InstanceInclusionMask (0xFF = all instances)
    // - RayContributionToHitGroupIndex (0)
    // - MultiplierForGeometryContributionToShaderIndex (1)
    // - MissShaderIndex (0)
    // - Ray descriptor
    // - Payload
    uint hitResult = 0u;
    while(hitResult<10000u){
    TraceRay(
        g_Scene,
        RAY_FLAG_NONE,
        0xFF,
        0,  // RayContributionToHitGroupIndex
        1,  // MultiplierForGeometryContributionToShaderIndex
        0,  // MissShaderIndex
        ray,
        payload
    );
    hitResult+=1u;
    }
    // Output results
    // Offset 4: hit results per thread (1 = hit, 0 = miss)
    // hitResult = (payload.hitT > 0.0f) ? 1u : 0u;
    g_Output.Store(4 + linearIdx * 4, hitResult);

    // Store hit distance as float bits at offset 68 (after 16 hit results + magic)
    g_Output.Store(68 + linearIdx * 4, asuint(payload.hitT));
}

[shader("closesthit")]
void MinimalClosestHit(inout SRayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    // Record hit distance
    payload.hitT = RayTCurrent();
    payload.color = float3(1, 0, 0);  // Red = hit
}

[shader("miss")]
void MinimalMiss(inout SRayPayload payload : SV_RayPayload) {
    payload.hitT = -1.0f;
    payload.color = float3(0, 0, 1);  // Blue = miss
}
)";

// ============================================
// Compute Shader (Mode 2) - Same output format as DXR
// ============================================
static const char* s_computeShaderSource = R"(
// Output buffer - same layout as DXR shader
RWByteAddressBuffer g_Output : register(u0);

[numthreads(4, 4, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID, uint3 dims : SV_GroupID)
{
    // Write magic value first to confirm shader execution
    g_Output.Store(0, 0xDEADBEEFu);

    uint linearIdx = DTid.x + DTid.y * 4;

    // Simulate ray hit/miss based on thread position
    // Center threads (1,1), (1,2), (2,1), (2,2) "hit"
    uint hitResult = 0u;
    if (DTid.x >= 1 && DTid.x <= 2 && DTid.y >= 1 && DTid.y <= 2) {
        hitResult = 1u;
    }

    // Store hit result at offset 4 (after magic value)
    g_Output.Store(4 + linearIdx * 4, hitResult);

    // Store simulated distance (2.5 for hits, -1.0 for misses)
    float hitDist = (hitResult == 1u) ? 2.5f : -1.0f;
    g_Output.Store(68 + linearIdx * 4, asuint(hitDist));
}
)";

static void Cleanup() {
    // Shared resources
    delete s_outputBuffer; s_outputBuffer = nullptr;
    delete s_readbackBuffer; s_readbackBuffer = nullptr;
    delete s_constantBuffer; s_constantBuffer = nullptr;

    // Ray tracing resources
    delete s_pipeline; s_pipeline = nullptr;
    delete s_sbt; s_sbt = nullptr;
    delete s_shaderLib; s_shaderLib = nullptr;

    // Compute shader resources
    delete s_computeShader; s_computeShader = nullptr;
    delete s_computePSO; s_computePSO = nullptr;

    // Cleanup acceleration structure resources
    delete s_tlas; s_tlas = nullptr;
    delete s_blas; s_blas = nullptr;
    delete s_cubeVertexBuffer; s_cubeVertexBuffer = nullptr;
    delete s_cubeIndexBuffer; s_cubeIndexBuffer = nullptr;
    delete s_blasScratchBuffer; s_blasScratchBuffer = nullptr;
    delete s_blasResultBuffer; s_blasResultBuffer = nullptr;
    delete s_tlasScratchBuffer; s_tlasScratchBuffer = nullptr;
    delete s_tlasResultBuffer; s_tlasResultBuffer = nullptr;
    delete s_tlasInstanceBuffer; s_tlasInstanceBuffer = nullptr;
}

// ============================================
// Shared Readback Verification
// ============================================
static void VerifyReadbackResults(CTestContext& ctx, const char* modeName) {
    CFFLog::Info("========================================");
    CFFLog::Info("TestDXRReadback: Verifying Results (%s)", modeName);
    CFFLog::Info("========================================");

    void* mappedData = s_readbackBuffer->Map();
    if (!mappedData) {
        CFFLog::Error("FAIL: Failed to map readback buffer!");
        ASSERT(ctx, false, "Map readback buffer");
        return;
    }

    const uint32_t* rawData = static_cast<const uint32_t*>(mappedData);

    // Check magic value at offset 0
    CFFLog::Info("Magic value check:");
    CFFLog::Info("  [0] = 0x%08X (expect 0xDEADBEEF if shader ran)", rawData[0]);

    bool shaderRan = (rawData[0] == 0xDEADBEEF);

    if (shaderRan) {
        CFFLog::Info("SUCCESS: Magic value 0xDEADBEEF found! Shader executed!");

        // Check hit results (offset 4-67 = indices 1-16)
        CFFLog::Info("Results per thread (1=hit, 0=miss):");
        int hitCount = 0;
        for (uint32_t i = 0; i < TOTAL_THREADS; i++) {
            uint32_t hitResult = rawData[1 + i];
            if (hitResult == 1) hitCount++;
            CFFLog::Info("  Thread[%u,%u]: %s (value=%u)",
                i % DISPATCH_WIDTH, i / DISPATCH_WIDTH,
                hitResult == 1 ? "HIT" : "MISS", hitResult);
        }
        CFFLog::Info("Total hits: %d/%d", hitCount, TOTAL_THREADS);

        // Check hit distances (offset 68-131 = indices 17-32)
        CFFLog::Info("Distances per thread:");
        const float* hitDistances = reinterpret_cast<const float*>(&rawData[17]);
        for (uint32_t i = 0; i < TOTAL_THREADS; i++) {
            float dist = hitDistances[i];
            CFFLog::Info("  Thread[%u,%u]: %.3f",
                i % DISPATCH_WIDTH, i / DISPATCH_WIDTH, dist);
        }
    } else {
        CFFLog::Error("FAIL: Magic value NOT found - shader did not execute!");
        CFFLog::Error("  Expected: 0xDEADBEEF");
        CFFLog::Error("  Got:      0x%08X", rawData[0]);

        // Print all raw data for debugging
        CFFLog::Info("Raw buffer contents:");
        for (uint32_t i = 0; i < OUTPUT_BUFFER_SIZE / 4; i++) {
            CFFLog::Info("  [%2u] = 0x%08X", i, rawData[i]);
        }
    }

    s_readbackBuffer->Unmap();

    CFFLog::Info("========================================");
    if (shaderRan) {
        CFFLog::Info("TEST RESULT: %s EXECUTED SUCCESSFULLY!", modeName);
    } else {
        CFFLog::Error("TEST RESULT: %s DID NOT EXECUTE", modeName);
        CFFLog::Error("Possible causes:");
        CFFLog::Error("  1. Dispatch not executing shader");
        CFFLog::Error("  2. UAV not bound correctly");
        CFFLog::Error("  3. Pipeline state issue");
        if (s_testMode == ETestMode::RayTracing) {
            CFFLog::Error("  4. TLAS/BLAS issue");
            CFFLog::Error("  5. SBT issue");
        }
    }
    CFFLog::Info("========================================");
}

// Cube geometry data (8 vertices, 36 indices for 12 triangles)
struct SCubeVertex {
    float x, y, z;
};

static const SCubeVertex s_cubeVertices[] = {
    // Front face
    {-0.5f, -0.5f,  0.5f},  // 0
    { 0.5f, -0.5f,  0.5f},  // 1
    { 0.5f,  0.5f,  0.5f},  // 2
    {-0.5f,  0.5f,  0.5f},  // 3
    // Back face
    {-0.5f, -0.5f, -0.5f},  // 4
    { 0.5f, -0.5f, -0.5f},  // 5
    { 0.5f,  0.5f, -0.5f},  // 6
    {-0.5f,  0.5f, -0.5f},  // 7
};

static const uint32_t s_cubeIndices[] = {
    // Front face
    0, 1, 2,  0, 2, 3,
    // Back face
    5, 4, 7,  5, 7, 6,
    // Top face
    3, 2, 6,  3, 6, 7,
    // Bottom face
    4, 5, 1,  4, 1, 0,
    // Right face
    1, 5, 6,  1, 6, 2,
    // Left face
    4, 0, 3,  4, 3, 7,
};

static const uint32_t CUBE_VERTEX_COUNT = sizeof(s_cubeVertices) / sizeof(SCubeVertex);
static const uint32_t CUBE_INDEX_COUNT = sizeof(s_cubeIndices) / sizeof(uint32_t);

class CTestDXRReadback : public ITestCase {
public:
    const char* GetName() const override {
        return "TestDXRReadback";
    }

    void Setup(CTestContext& ctx) override {
        // Frame 1: Check availability and create resources
        ctx.OnFrame(1, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestDXRReadback: Frame 1 - Setup");
            CFFLog::Info("========================================");
            CFFLog::Info("Test Mode: %s", s_testMode == ETestMode::RayTracing ? "RAY TRACING (Mode 1)" : "COMPUTE SHADER (Mode 2)");

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
            ASSERT_NOT_NULL(ctx, rhiCtx, "RHI RenderContext");

            // Check if we're on DX12
            if (RHI::CRHIManager::Instance().GetBackend() != RHI::EBackend::DX12) {
                CFFLog::Warning("TestDXRReadback requires DX12 backend, skipping");
                ctx.Finish();
                return;
            }

            // Check DXR support (only needed for ray tracing mode)
            if (s_testMode == ETestMode::RayTracing) {
                auto& dx12Ctx = RHI::DX12::CDX12Context::Instance();
                if (!dx12Ctx.SupportsRaytracing()) {
                    CFFLog::Warning("Ray tracing not supported on this device, skipping");
                    ctx.Finish();
                    return;
                }
                CFFLog::Info("DX12 + DXR available, proceeding with ray tracing test");
            } else {
                CFFLog::Info("DX12 available, proceeding with compute shader test");
            }

            // Create output buffer (UAV) with pre-fill data
            CFFLog::Info("Creating output buffer: %u bytes", OUTPUT_BUFFER_SIZE);

            // Pre-fill data with 0xBAADF00D to detect if buffer is being written to
            static uint32_t outputInitData[OUTPUT_BUFFER_SIZE / sizeof(uint32_t)];
            for (uint32_t i = 0; i < OUTPUT_BUFFER_SIZE / sizeof(uint32_t); i++) {
                outputInitData[i] = 0xBAADF00D;
            }

            RHI::BufferDesc outputDesc;
            outputDesc.size = OUTPUT_BUFFER_SIZE;
            outputDesc.usage = RHI::EBufferUsage::UnorderedAccess | RHI::EBufferUsage::Structured;
            outputDesc.cpuAccess = RHI::ECPUAccess::None;
            outputDesc.structureByteStride = sizeof(uint32_t);
            outputDesc.debugName = "TestDXR_OutputBuffer";

            s_outputBuffer = rhiCtx->CreateBuffer(outputDesc, outputInitData);
            ASSERT_NOT_NULL(ctx, s_outputBuffer, "Output buffer creation");
            CFFLog::Info("Pre-filled output buffer with 0xBAADF00D");

            // Create readback buffer and pre-fill with known pattern
            RHI::BufferDesc readbackDesc;
            readbackDesc.size = OUTPUT_BUFFER_SIZE;
            readbackDesc.usage = RHI::EBufferUsage::Structured;  // For GPU readback
            readbackDesc.cpuAccess = RHI::ECPUAccess::Read;
            readbackDesc.structureByteStride = sizeof(uint32_t);
            readbackDesc.debugName = "TestDXR_ReadbackBuffer";

            s_readbackBuffer = rhiCtx->CreateBuffer(readbackDesc, nullptr);
            ASSERT_NOT_NULL(ctx, s_readbackBuffer, "Readback buffer creation");

            // Pre-fill readback buffer with 0xCAFEBABE to detect if copy actually happened
            void* prefillData = s_readbackBuffer->Map();
            if (prefillData) {
               uint32_t* words = static_cast<uint32_t*>(prefillData);
               for (uint32_t i = 0; i < OUTPUT_BUFFER_SIZE / 4; i++) {
                   words[i] = 0xCAFEBABE;
               }
               s_readbackBuffer->Unmap();
               CFFLog::Info("Pre-filled readback buffer with 0xCAFEBABE");
            }

            // Create constant buffer
            struct CB_Test {
                uint32_t dispatchWidth;
                uint32_t dispatchHeight;
                uint32_t dispatchDepth;
                float testMultiplier;
            };

            RHI::BufferDesc cbDesc;
            cbDesc.size = sizeof(CB_Test);
            cbDesc.usage = RHI::EBufferUsage::Constant;
            cbDesc.cpuAccess = RHI::ECPUAccess::Write;
            cbDesc.debugName = "TestDXR_ConstantBuffer";

            s_constantBuffer = rhiCtx->CreateBuffer(cbDesc, nullptr);
            ASSERT_NOT_NULL(ctx, s_constantBuffer, "Constant buffer creation");

            // Fill constant buffer
            CB_Test* cb = static_cast<CB_Test*>(s_constantBuffer->Map());
            ASSERT_NOT_NULL(ctx, cb, "Map constant buffer");
            cb->dispatchWidth = DISPATCH_WIDTH;
            cb->dispatchHeight = DISPATCH_HEIGHT;
            cb->dispatchDepth = DISPATCH_DEPTH;
            cb->testMultiplier = 1.0f;
            s_constantBuffer->Unmap();

            // ========================================
            // Ray Tracing Only: Create Acceleration Structures
            // ========================================
            if (s_testMode == ETestMode::RayTracing) {
                // ========================================
                // Create Cube Geometry Buffers
                // ========================================
                CFFLog::Info("Creating cube geometry buffers...");

                // Vertex buffer (for BLAS building)
                RHI::BufferDesc vbDesc;
                vbDesc.size = sizeof(s_cubeVertices);
                vbDesc.usage = RHI::EBufferUsage::Structured;  // For BLAS building (SRV access)
                vbDesc.cpuAccess = RHI::ECPUAccess::None;
                vbDesc.structureByteStride = sizeof(SCubeVertex);
                vbDesc.debugName = "TestDXR_CubeVertexBuffer";

                s_cubeVertexBuffer = rhiCtx->CreateBuffer(vbDesc, s_cubeVertices);
                ASSERT_NOT_NULL(ctx, s_cubeVertexBuffer, "Cube vertex buffer creation");
                CFFLog::Info("Uploaded %u vertices (%zu bytes)", CUBE_VERTEX_COUNT, sizeof(s_cubeVertices));

                // Index buffer (for BLAS building)
                RHI::BufferDesc ibDesc;
                ibDesc.size = sizeof(s_cubeIndices);
                ibDesc.usage = RHI::EBufferUsage::Index;  // For BLAS building
                ibDesc.cpuAccess = RHI::ECPUAccess::None;
                ibDesc.structureByteStride = 0;
                ibDesc.debugName = "TestDXR_CubeIndexBuffer";

                s_cubeIndexBuffer = rhiCtx->CreateBuffer(ibDesc, s_cubeIndices);
                ASSERT_NOT_NULL(ctx, s_cubeIndexBuffer, "Cube index buffer creation");
                CFFLog::Info("Uploaded %u indices (%zu bytes)", CUBE_INDEX_COUNT, sizeof(s_cubeIndices));

                // ========================================
                // Create BLAS (Bottom Level Acceleration Structure)
                // ========================================
                CFFLog::Info("Creating BLAS for cube...");

                // Setup geometry descriptor
                RHI::GeometryDesc geomDesc;
                geomDesc.type = RHI::EGeometryType::Triangles;
                geomDesc.flags = RHI::EGeometryFlags::Opaque;
                geomDesc.triangles.vertexBuffer = s_cubeVertexBuffer;
                geomDesc.triangles.vertexBufferOffset = 0;
                geomDesc.triangles.vertexCount = CUBE_VERTEX_COUNT;
                geomDesc.triangles.vertexStride = sizeof(SCubeVertex);
                geomDesc.triangles.vertexFormat = RHI::ETextureFormat::R32G32B32_FLOAT;
                geomDesc.triangles.indexBuffer = s_cubeIndexBuffer;
                geomDesc.triangles.indexBufferOffset = 0;
                geomDesc.triangles.indexCount = CUBE_INDEX_COUNT;
                geomDesc.triangles.indexFormat = RHI::EIndexFormat::UInt32;

                RHI::BLASDesc blasDesc;
                blasDesc.geometries.push_back(geomDesc);
                blasDesc.buildFlags = RHI::EAccelerationStructureBuildFlags::PreferFastTrace;

                // Get prebuild info for buffer sizes
                auto blasPrebuild = rhiCtx->GetAccelerationStructurePrebuildInfo(blasDesc);
                CFFLog::Info("BLAS prebuild: result=%llu, scratch=%llu",
                            blasPrebuild.resultDataMaxSizeInBytes,
                            blasPrebuild.scratchDataSizeInBytes);

                // Create scratch buffer
                RHI::BufferDesc blasScratchDesc;
                blasScratchDesc.size = blasPrebuild.scratchDataSizeInBytes;
                blasScratchDesc.usage = RHI::EBufferUsage::UnorderedAccess;
                blasScratchDesc.cpuAccess = RHI::ECPUAccess::None;
                blasScratchDesc.debugName = "TestDXR_BLASScratch";
                s_blasScratchBuffer = rhiCtx->CreateBuffer(blasScratchDesc, nullptr);
                ASSERT_NOT_NULL(ctx, s_blasScratchBuffer, "BLAS scratch buffer");

                // Create result buffer
                RHI::BufferDesc blasResultDesc;
                blasResultDesc.size = blasPrebuild.resultDataMaxSizeInBytes;
                blasResultDesc.usage = RHI::EBufferUsage::AccelerationStructure;
                blasResultDesc.cpuAccess = RHI::ECPUAccess::None;
                blasResultDesc.debugName = "TestDXR_BLASResult";
                s_blasResultBuffer = rhiCtx->CreateBuffer(blasResultDesc, nullptr);
                ASSERT_NOT_NULL(ctx, s_blasResultBuffer, "BLAS result buffer");

                // Create BLAS
                s_blas = rhiCtx->CreateBLAS(blasDesc, s_blasScratchBuffer, s_blasResultBuffer);
                ASSERT_NOT_NULL(ctx, s_blas, "BLAS creation");
                CFFLog::Info("BLAS created, GPU VA: 0x%llx", s_blas->GetGPUVirtualAddress());

                // ========================================
                // Create TLAS (Top Level Acceleration Structure)
                // ========================================
                CFFLog::Info("Creating TLAS with single cube instance...");

                // Setup instance (identity transform)
                RHI::AccelerationStructureInstance instance;
                instance.transform[0][0] = 1.0f; instance.transform[0][1] = 0.0f; instance.transform[0][2] = 0.0f; instance.transform[0][3] = 0.0f;
                instance.transform[1][0] = 0.0f; instance.transform[1][1] = 1.0f; instance.transform[1][2] = 0.0f; instance.transform[1][3] = 0.0f;
                instance.transform[2][0] = 0.0f; instance.transform[2][1] = 0.0f; instance.transform[2][2] = 1.0f; instance.transform[2][3] = 0.0f;
                instance.instanceID = 0;
                instance.instanceMask = 0xFF;
                instance.instanceContributionToHitGroupIndex = 0;
                instance.flags = 0;
                instance.blas = s_blas;

                RHI::TLASDesc tlasDesc;
                tlasDesc.instances.push_back(instance);
                tlasDesc.buildFlags = RHI::EAccelerationStructureBuildFlags::PreferFastTrace;

                // Get prebuild info
                auto tlasPrebuild = rhiCtx->GetAccelerationStructurePrebuildInfo(tlasDesc);
                CFFLog::Info("TLAS prebuild: result=%llu, scratch=%llu",
                            tlasPrebuild.resultDataMaxSizeInBytes,
                            tlasPrebuild.scratchDataSizeInBytes);

                // Create scratch buffer
                RHI::BufferDesc tlasScratchDesc;
                tlasScratchDesc.size = tlasPrebuild.scratchDataSizeInBytes;
                tlasScratchDesc.usage = RHI::EBufferUsage::UnorderedAccess;
                tlasScratchDesc.cpuAccess = RHI::ECPUAccess::None;
                tlasScratchDesc.debugName = "TestDXR_TLASScratch";
                s_tlasScratchBuffer = rhiCtx->CreateBuffer(tlasScratchDesc, nullptr);
                ASSERT_NOT_NULL(ctx, s_tlasScratchBuffer, "TLAS scratch buffer");

                // Create result buffer
                RHI::BufferDesc tlasResultDesc;
                tlasResultDesc.size = tlasPrebuild.resultDataMaxSizeInBytes;
                tlasResultDesc.usage = RHI::EBufferUsage::AccelerationStructure;
                tlasResultDesc.cpuAccess = RHI::ECPUAccess::None;
                tlasResultDesc.debugName = "TestDXR_TLASResult";
                s_tlasResultBuffer = rhiCtx->CreateBuffer(tlasResultDesc, nullptr);
                ASSERT_NOT_NULL(ctx, s_tlasResultBuffer, "TLAS result buffer");

                // Create instance buffer (TLAS needs GPU-visible instance data)
                RHI::BufferDesc tlasInstanceDesc;
                tlasInstanceDesc.size = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);  // 64 bytes per instance
                tlasInstanceDesc.usage = RHI::EBufferUsage::Structured;  // Instance buffer for TLAS
                tlasInstanceDesc.cpuAccess = RHI::ECPUAccess::Write;
                tlasInstanceDesc.structureByteStride = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
                tlasInstanceDesc.debugName = "TestDXR_TLASInstance";
                s_tlasInstanceBuffer = rhiCtx->CreateBuffer(tlasInstanceDesc, nullptr);
                ASSERT_NOT_NULL(ctx, s_tlasInstanceBuffer, "TLAS instance buffer");

                // Create TLAS
                s_tlas = rhiCtx->CreateTLAS(tlasDesc, s_tlasScratchBuffer, s_tlasResultBuffer, s_tlasInstanceBuffer);
                ASSERT_NOT_NULL(ctx, s_tlas, "TLAS creation");
                CFFLog::Info("TLAS created, GPU VA: 0x%llx", s_tlas->GetGPUVirtualAddress());

                CFFLog::Info("Frame 1 complete - buffers and acceleration structures created");
            } else {
                CFFLog::Info("Frame 1 complete - buffers created (compute mode)");
            }
        });

        // Frame 5: Compile shader and create pipeline
        ctx.OnFrame(5, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestDXRReadback: Frame 5 - Create Pipeline");
            CFFLog::Info("========================================");

            if (!s_outputBuffer) {
                CFFLog::Warning("Skipping - resources not created");
                return;
            }

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();

            if (s_testMode == ETestMode::RayTracing) {
                // ========================================
                // Ray Tracing Pipeline (Mode 1)
                // ========================================

                // Check DXCompiler
                if (!RHI::IsDXCompilerAvailable()) {
                    CFFLog::Error("DXCompiler not available!");
                    ASSERT(ctx, false, "DXCompiler required for DXR");
                    return;
                }

                // Compile shader from string
                CFFLog::Info("Compiling minimal DXR shader...");
                RHI::SCompiledShader compiled = RHI::CompileDXRLibraryFromSource(
                    s_minimalShaderSource,
                    "MinimalDXRTest",  // Source name for error reporting
                    nullptr,  // No include handler
                    true      // Debug mode
                );

                if (!compiled.success) {
                    CFFLog::Error("Shader compilation failed: %s", compiled.errorMessage.c_str());
                    ASSERT(ctx, false, "Shader compilation");
                    return;
                }

                CFFLog::Info("Shader compiled: %zu bytes", compiled.bytecode.size());

                // Create shader library
                RHI::ShaderDesc shaderDesc;
                shaderDesc.type = RHI::EShaderType::Library;
                shaderDesc.bytecode = compiled.bytecode.data();
                shaderDesc.bytecodeSize = compiled.bytecode.size();

                s_shaderLib = rhiCtx->CreateShader(shaderDesc);
                ASSERT_NOT_NULL(ctx, s_shaderLib, "Shader library creation");

                // Create ray tracing pipeline
                RHI::RayTracingPipelineDesc pipelineDesc;
                pipelineDesc.shaderLibrary = s_shaderLib;

                // Add shader exports
                RHI::ShaderExport rayGenExport;
                rayGenExport.name = "MinimalRayGen";
                rayGenExport.type = RHI::EShaderExportType::RayGeneration;
                pipelineDesc.exports.push_back(rayGenExport);

                RHI::ShaderExport missExport;
                missExport.name = "MinimalMiss";
                missExport.type = RHI::EShaderExportType::Miss;
                pipelineDesc.exports.push_back(missExport);

                RHI::ShaderExport closestHitExport;
                closestHitExport.name = "MinimalClosestHit";
                closestHitExport.type = RHI::EShaderExportType::ClosestHit;
                pipelineDesc.exports.push_back(closestHitExport);

                // Add hit group that uses the closest hit shader
                RHI::HitGroupDesc hitGroup;
                hitGroup.name = "HitGroup";
                hitGroup.closestHitShader = "MinimalClosestHit";
                hitGroup.anyHitShader = nullptr;
                hitGroup.intersectionShader = nullptr;
                pipelineDesc.hitGroups.push_back(hitGroup);

                // Payload size: float3 color + float hitT = 16 bytes
                pipelineDesc.maxPayloadSize = sizeof(float) * 4;
                pipelineDesc.maxAttributeSize = sizeof(float) * 2;  // Barycentrics
                pipelineDesc.maxRecursionDepth = 1;

                s_pipeline = rhiCtx->CreateRayTracingPipelineState(pipelineDesc);
                ASSERT_NOT_NULL(ctx, s_pipeline, "Ray tracing pipeline creation");

                // Create shader binding table
                RHI::ShaderBindingTableDesc sbtDesc;
                sbtDesc.pipeline = s_pipeline;

                RHI::ShaderRecord rayGenRecord;
                rayGenRecord.exportName = "MinimalRayGen";
                sbtDesc.rayGenRecords.push_back(rayGenRecord);

                RHI::ShaderRecord missRecord;
                missRecord.exportName = "MinimalMiss";
                sbtDesc.missRecords.push_back(missRecord);

                // Hit group record - must use the hit group name, not the shader name
                RHI::ShaderRecord hitGroupRecord;
                hitGroupRecord.exportName = "HitGroup";
                sbtDesc.hitGroupRecords.push_back(hitGroupRecord);

                s_sbt = rhiCtx->CreateShaderBindingTable(sbtDesc);
                ASSERT_NOT_NULL(ctx, s_sbt, "Shader binding table creation");

                CFFLog::Info("Frame 5 complete - ray tracing pipeline created");

            } else {
                // ========================================
                // Compute Pipeline (Mode 2)
                // ========================================
                CFFLog::Info("Compiling compute shader...");
                RHI::SCompiledShader compiled = RHI::CompileShaderFromSource(
                    s_computeShaderSource,
                    "CSMain",
                    "cs_5_0",
                    nullptr,
                    true  // debug
                );

                if (!compiled.success) {
                    CFFLog::Error("Compute shader compilation failed: %s", compiled.errorMessage.c_str());
                    ASSERT(ctx, false, "Compute shader compilation");
                    return;
                }
                CFFLog::Info("Shader compiled: %zu bytes", compiled.bytecode.size());

                // Create shader object
                RHI::ShaderDesc shaderDesc;
                shaderDesc.type = RHI::EShaderType::Compute;
                shaderDesc.bytecode = compiled.bytecode.data();
                shaderDesc.bytecodeSize = compiled.bytecode.size();

                s_computeShader = rhiCtx->CreateShader(shaderDesc);
                ASSERT_NOT_NULL(ctx, s_computeShader, "Compute shader creation");

                // Create compute pipeline state
                RHI::ComputePipelineDesc psoDesc;
                psoDesc.computeShader = s_computeShader;
                psoDesc.debugName = "TestDXR_ComputePSO";

                s_computePSO = rhiCtx->CreateComputePipelineState(psoDesc);
                ASSERT_NOT_NULL(ctx, s_computePSO, "Compute PSO creation");

                CFFLog::Info("Frame 5 complete - compute pipeline created");
            }
        });

        // Frame 7: Build acceleration structures (Ray Tracing mode only)
        ctx.OnFrame(7, [&ctx]() {
            // Skip in compute mode
            if (s_testMode == ETestMode::Compute) {
                CFFLog::Info("Frame 7: Skipping AS build (compute mode)");
                return;
            }

            CFFLog::Info("========================================");
            CFFLog::Info("TestDXRReadback: Frame 7 - Build Acceleration Structures");
            CFFLog::Info("========================================");

            if (!s_blas || !s_tlas) {
                CFFLog::Warning("Skipping - acceleration structures not created");
                return;
            }

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
            auto* cmdList = rhiCtx->GetCommandList();

            // Build BLAS first
            CFFLog::Info("Building BLAS...");
            cmdList->BuildAccelerationStructure(s_blas);

            // UAV barrier to ensure BLAS build completes before TLAS build
            CFFLog::Info("UAV barrier after BLAS build...");
            cmdList->UAVBarrier(nullptr);  // Global barrier

            // Build TLAS
            CFFLog::Info("Building TLAS...");
            cmdList->BuildAccelerationStructure(s_tlas);

            // UAV barrier after TLAS build
            CFFLog::Info("UAV barrier after TLAS build...");
            cmdList->UAVBarrier(nullptr);

            // Execute and wait for GPU to complete
            CFFLog::Info("Execute and wait for acceleration structure builds...");
            rhiCtx->ExecuteAndWait();

            // Flush debug messages
            RHI::DX12::CDX12Context::Instance().FlushDebugMessages();

            CFFLog::Info("Frame 7 complete - acceleration structures built");
            CFFLog::Info("BLAS GPU VA: 0x%llx", s_blas->GetGPUVirtualAddress());
            CFFLog::Info("TLAS GPU VA: 0x%llx", s_tlas->GetGPUVirtualAddress());
        });

        // Frame 10: Dispatch and readback
        ctx.OnFrame(10, [&ctx]() {
            CFFLog::Info("========================================");
            CFFLog::Info("TestDXRReadback: Frame 10 - Dispatch");
            CFFLog::Info("========================================");

            if (!s_outputBuffer) {
                CFFLog::Warning("Skipping - output buffer not created");
                return;
            }

            auto* rhiCtx = RHI::CRHIManager::Instance().GetRenderContext();
            auto* cmdList = rhiCtx->GetCommandList();

            if (s_testMode == ETestMode::RayTracing) {
                // ========================================
                // Ray Tracing Dispatch (Mode 1)
                // ========================================
                if (!s_pipeline || !s_sbt || !s_tlas) {
                    CFFLog::Warning("Skipping - ray tracing resources not created");
                    return;
                }

                // Ensure output buffer is in UAV state before ray tracing
                CFFLog::Info("Transition output buffer to UAV state...");
                cmdList->Barrier(s_outputBuffer, RHI::EResourceState::Common, RHI::EResourceState::UnorderedAccess);

                // Set ray tracing pipeline
                CFFLog::Info("Setting ray tracing pipeline...");
                cmdList->SetRayTracingPipelineState(s_pipeline);

                // Bind resources using uniform interface
                CFFLog::Info("Binding TLAS to t0...");
                cmdList->SetAccelerationStructure(0, s_tlas);

                CFFLog::Info("Binding UAV buffer to u0...");
                cmdList->SetUnorderedAccess(0, s_outputBuffer);

                // Dispatch rays
                CFFLog::Info("Dispatching rays: %u x %u x %u", DISPATCH_WIDTH, DISPATCH_HEIGHT, DISPATCH_DEPTH);

                RHI::DispatchRaysDesc dispatchDesc;
                dispatchDesc.shaderBindingTable = s_sbt;
                dispatchDesc.width = DISPATCH_WIDTH;
                dispatchDesc.height = DISPATCH_HEIGHT;
                dispatchDesc.depth = DISPATCH_DEPTH;

                CFFLog::Info("Calling DispatchRays...");
                cmdList->DispatchRays(dispatchDesc);
                CFFLog::Info("DispatchRays returned");

            } else {
                // ========================================
                // Compute Dispatch (Mode 2)
                // ========================================
                if (!s_computePSO) {
                    CFFLog::Warning("Skipping - compute PSO not created");
                    return;
                }

                // Set compute pipeline
                CFFLog::Info("Setting compute pipeline...");
                cmdList->SetPipelineState(s_computePSO);

                // Bind UAV
                CFFLog::Info("Binding UAV buffer to u0...");
                cmdList->SetUnorderedAccess(0, s_outputBuffer);

                // Dispatch compute shader (4x4 threads, 1 group)
                CFFLog::Info("Dispatching compute: 1 thread group (4x4 threads)");
                cmdList->Dispatch(1, 1, 1);
                CFFLog::Info("Dispatch returned");
            }

            // ========================================
            // Shared: Barrier, Copy, Readback
            // ========================================

            // UAV barrier
            CFFLog::Info("UAV barrier...");
            cmdList->UAVBarrier(s_outputBuffer);

            // Transition to copy source
            CFFLog::Info("Transition to copy source...");
            cmdList->Barrier(s_outputBuffer, RHI::EResourceState::UnorderedAccess, RHI::EResourceState::CopySource);

            // Copy to readback
            CFFLog::Info("Copy to readback buffer...");
            cmdList->CopyBuffer(s_readbackBuffer, 0, s_outputBuffer, 0, OUTPUT_BUFFER_SIZE);

            // Transition back
            cmdList->Barrier(s_outputBuffer, RHI::EResourceState::CopySource, RHI::EResourceState::UnorderedAccess);

            // Execute and wait
            CFFLog::Info("Execute and wait...");
            rhiCtx->ExecuteAndWait();

            // Flush D3D12 debug messages after GPU work completes
            RHI::DX12::CDX12Context::Instance().FlushDebugMessages();

            CFFLog::Info("Frame 10 complete - dispatch done");

            // Verify results using shared function
            const char* modeName = (s_testMode == ETestMode::RayTracing) ? "RAY TRACING" : "COMPUTE SHADER";
            VerifyReadbackResults(ctx, modeName);
        });
        // Frame 15: End test
        ctx.OnFrame(15, [&ctx]() {
            Cleanup();
            CFFLog::Info("TestDXRReadback complete");
            CScreenshot::CaptureTest(ctx.pipeline, "TestDXRReadback", 15);
            ctx.Finish();
        });
    }
};

REGISTER_TEST(CTestDXRReadback)
