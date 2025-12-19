#include "DXRLightmapBaker.h"
#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include "../VolumetricLightmap.h"
#include "../../Scene.h"
#include "../../../RHI/RHIManager.h"
#include "../../../RHI/RHIResources.h"
#include "../../../RHI/RHIDescriptors.h"
#include "../../../RHI/RHIRayTracing.h"
#include "../../../RHI/ShaderCompiler.h"
#include "../../../Core/FFLog.h"
#include "../../../Core/PathManager.h"
#include <chrono>

// ============================================
// CDXRLightmapBaker Implementation
// ============================================

CDXRLightmapBaker::CDXRLightmapBaker() {
    m_asManager = std::make_unique<CDXRAccelerationStructureManager>();
}

CDXRLightmapBaker::~CDXRLightmapBaker() {
    Shutdown();
}

bool CDXRLightmapBaker::Initialize() {
    if (m_isReady) {
        return true;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[DXRBaker] No render context available");
        return false;
    }

    // Check DXR support
    if (!ctx->SupportsRaytracing()) {
        CFFLog::Warning("[DXRBaker] Ray tracing not supported on this device");
        return false;
    }

    // Initialize acceleration structure manager
    if (!m_asManager->Initialize()) {
        CFFLog::Error("[DXRBaker] Failed to initialize AS manager");
        return false;
    }

    // Create constant buffer
    if (!CreateConstantBuffer()) {
        CFFLog::Error("[DXRBaker] Failed to create constant buffer");
        return false;
    }

    // Note: Pipeline and SBT creation requires shader compilation
    // which may be deferred until first bake

    m_isReady = true;
    CFFLog::Info("[DXRBaker] Initialized successfully");
    return true;
}

void CDXRLightmapBaker::Shutdown() {
    ReleasePerBakeResources();

    m_sbt.reset();
    m_pipeline.reset();
    m_shaderLibrary.reset();
    m_constantBuffer.reset();
    m_outputBuffer.reset();
    m_readbackBuffer.reset();

    if (m_asManager) {
        m_asManager->Shutdown();
    }

    m_isReady = false;
}

bool CDXRLightmapBaker::IsAvailable() const {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    return ctx && ctx->SupportsRaytracing();
}

// ============================================
// Initialization Helpers
// ============================================

bool CDXRLightmapBaker::CreateConstantBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_BakeParams);
    cbDesc.usage = RHI::EBufferUsage::Constant;
    cbDesc.cpuAccess = RHI::ECPUAccess::Write;

    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));
    return m_constantBuffer != nullptr;
}

bool CDXRLightmapBaker::CreatePipeline() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Check if DXCompiler is available
    if (!RHI::IsDXCompilerAvailable()) {
        CFFLog::Error("[DXRBaker] DXCompiler not available - cannot compile DXR shaders");
        CFFLog::Error("[DXRBaker] Please ensure dxcompiler.dll is in the application directory");
        return false;
    }

    // Compile DXR shader library
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/DXR/LightmapBake.hlsl";
    CFFLog::Info("[DXRBaker] Compiling DXR shader library: %s", shaderPath.c_str());

    // Create include handler for shader directory
    RHI::CDefaultShaderIncludeHandler includeHandler(FFPath::GetSourceDir() + "/Shader/DXR/");

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    RHI::SCompiledShader compiled = RHI::CompileDXRLibraryFromFile(
        shaderPath,
        &includeHandler,
        debugShaders
    );

    if (!compiled.success) {
        CFFLog::Error("[DXRBaker] Shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    CFFLog::Info("[DXRBaker] Shader compiled successfully (%zu bytes)", compiled.bytecode.size());

    // Create shader from bytecode
    RHI::ShaderDesc shaderDesc;
    shaderDesc.type = RHI::EShaderType::Library;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();

    m_shaderLibrary.reset(ctx->CreateShader(shaderDesc));
    if (!m_shaderLibrary) {
        CFFLog::Error("[DXRBaker] Failed to create shader library");
        return false;
    }

    // Create ray tracing pipeline
    RHI::RayTracingPipelineDesc pipelineDesc;
    pipelineDesc.shaderLibrary = m_shaderLibrary.get();
    pipelineDesc.maxPayloadSize = sizeof(float) * 16;  // SRayPayload: ~64 bytes
    pipelineDesc.maxAttributeSize = sizeof(float) * 2; // Barycentrics
    pipelineDesc.maxRecursionDepth = 2;  // Primary + shadow

    // Export ray generation shader
    RHI::ShaderExport rayGenExport;
    rayGenExport.name = "RayGen";
    rayGenExport.type = RHI::EShaderExportType::RayGeneration;
    pipelineDesc.exports.push_back(rayGenExport);

    // Export miss shaders
    RHI::ShaderExport missExport;
    missExport.name = "Miss";
    missExport.type = RHI::EShaderExportType::Miss;
    pipelineDesc.exports.push_back(missExport);

    RHI::ShaderExport shadowMissExport;
    shadowMissExport.name = "ShadowMiss";
    shadowMissExport.type = RHI::EShaderExportType::Miss;
    pipelineDesc.exports.push_back(shadowMissExport);

    // Export hit groups
    RHI::HitGroupDesc primaryHitGroup;
    primaryHitGroup.name = "HitGroup";
    primaryHitGroup.closestHitShader = "ClosestHit";
    primaryHitGroup.anyHitShader = nullptr;
    primaryHitGroup.intersectionShader = nullptr;
    pipelineDesc.hitGroups.push_back(primaryHitGroup);

    RHI::HitGroupDesc shadowHitGroup;
    shadowHitGroup.name = "ShadowHitGroup";
    shadowHitGroup.closestHitShader = nullptr;
    shadowHitGroup.anyHitShader = "ShadowAnyHit";
    shadowHitGroup.intersectionShader = nullptr;
    pipelineDesc.hitGroups.push_back(shadowHitGroup);

    m_pipeline.reset(ctx->CreateRayTracingPipelineState(pipelineDesc));
    if (!m_pipeline) {
        CFFLog::Error("[DXRBaker] Failed to create ray tracing pipeline");
        return false;
    }

    CFFLog::Info("[DXRBaker] Ray tracing pipeline created successfully");
    return true;
}

bool CDXRLightmapBaker::CreateShaderBindingTable() {
    if (!m_pipeline) {
        return false;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::ShaderBindingTableDesc sbtDesc;
    sbtDesc.pipeline = m_pipeline.get();

    // Ray generation
    RHI::ShaderRecord rayGenRecord;
    rayGenRecord.exportName = "RayGen";
    sbtDesc.rayGenRecords.push_back(rayGenRecord);

    // Miss shaders
    RHI::ShaderRecord missRecord;
    missRecord.exportName = "Miss";
    sbtDesc.missRecords.push_back(missRecord);

    RHI::ShaderRecord shadowMissRecord;
    shadowMissRecord.exportName = "ShadowMiss";
    sbtDesc.missRecords.push_back(shadowMissRecord);

    // Hit groups
    RHI::ShaderRecord hitGroupRecord;
    hitGroupRecord.exportName = "HitGroup";
    sbtDesc.hitGroupRecords.push_back(hitGroupRecord);

    RHI::ShaderRecord shadowHitRecord;
    shadowHitRecord.exportName = "ShadowHitGroup";
    sbtDesc.hitGroupRecords.push_back(shadowHitRecord);

    m_sbt.reset(ctx->CreateShaderBindingTable(sbtDesc));
    return m_sbt != nullptr;
}

// ============================================
// Per-Bake Setup
// ============================================

bool CDXRLightmapBaker::CreateOutputBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Release existing buffer
    m_outputBuffer.reset();

    // Create structured buffer for 64 voxels (one brick)
    // Each voxel outputs SVoxelSHOutput (9 SH coefficients + validity)
    const uint32_t voxelsPerBrick = VL_BRICK_VOXEL_COUNT;  // 64
    const uint32_t bufferSize = voxelsPerBrick * sizeof(SVoxelSHOutput);

    RHI::BufferDesc bufDesc;
    bufDesc.size = bufferSize;
    bufDesc.usage = RHI::EBufferUsage::UnorderedAccess | RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(SVoxelSHOutput);
    bufDesc.debugName = "DXRBaker_OutputBuffer";

    m_outputBuffer.reset(ctx->CreateBuffer(bufDesc, nullptr));
    if (!m_outputBuffer) {
        CFFLog::Error("[DXRBaker] Failed to create output buffer");
        return false;
    }

    CFFLog::Info("[DXRBaker] Created output buffer (%u bytes, stride=%u)",
                 bufferSize, sizeof(SVoxelSHOutput));
    return true;
}

bool CDXRLightmapBaker::CreateReadbackBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Release existing buffer
    m_readbackBuffer.reset();

    // Create staging buffer for CPU readback (same size as output)
    const uint32_t voxelsPerBrick = VL_BRICK_VOXEL_COUNT;
    const uint32_t bufferSize = voxelsPerBrick * sizeof(SVoxelSHOutput);

    RHI::BufferDesc bufDesc;
    bufDesc.size = bufferSize;
    bufDesc.usage = RHI::EBufferUsage::Staging;
    bufDesc.cpuAccess = RHI::ECPUAccess::Read;
    bufDesc.debugName = "DXRBaker_ReadbackBuffer";

    m_readbackBuffer.reset(ctx->CreateBuffer(bufDesc, nullptr));
    if (!m_readbackBuffer) {
        CFFLog::Error("[DXRBaker] Failed to create readback buffer");
        return false;
    }

    // Allocate CPU-side storage
    m_readbackData.resize(voxelsPerBrick);

    CFFLog::Info("[DXRBaker] Created readback buffer");
    return true;
}

bool CDXRLightmapBaker::UploadSceneData(const SRayTracingSceneData& sceneData) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Release existing buffers
    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();

    // Upload materials
    if (!sceneData.materials.empty()) {
        std::vector<SGPUMaterialData> gpuMaterials;
        gpuMaterials.reserve(sceneData.materials.size());

        for (const auto& mat : sceneData.materials) {
            SGPUMaterialData gpuMat = {};
            gpuMat.albedo = mat.albedo;
            gpuMat.metallic = mat.metallic;
            gpuMat.roughness = mat.roughness;
            gpuMaterials.push_back(gpuMat);
        }

        RHI::BufferDesc matDesc;
        matDesc.size = static_cast<uint32_t>(gpuMaterials.size() * sizeof(SGPUMaterialData));
        matDesc.usage = RHI::EBufferUsage::Structured;
        matDesc.cpuAccess = RHI::ECPUAccess::None;
        matDesc.structureByteStride = sizeof(SGPUMaterialData);

        m_materialBuffer.reset(ctx->CreateBuffer(matDesc, gpuMaterials.data()));
    }

    // Upload lights
    if (!sceneData.lights.empty()) {
        std::vector<SGPULightData> gpuLights;
        gpuLights.reserve(sceneData.lights.size());

        for (const auto& light : sceneData.lights) {
            SGPULightData gpuLight = {};
            gpuLight.type = static_cast<uint32_t>(light.type);
            gpuLight.position = light.position;
            gpuLight.direction = light.direction;
            gpuLight.color = light.color;
            gpuLight.intensity = light.intensity;
            gpuLight.range = light.range;
            gpuLight.spotAngle = light.spotAngle;
            gpuLights.push_back(gpuLight);
        }

        RHI::BufferDesc lightDesc;
        lightDesc.size = static_cast<uint32_t>(gpuLights.size() * sizeof(SGPULightData));
        lightDesc.usage = RHI::EBufferUsage::Structured;
        lightDesc.cpuAccess = RHI::ECPUAccess::None;
        lightDesc.structureByteStride = sizeof(SGPULightData);

        m_lightBuffer.reset(ctx->CreateBuffer(lightDesc, gpuLights.data()));
        m_numLights = static_cast<uint32_t>(gpuLights.size());
    } else {
        m_numLights = 0;
    }

    // Upload instance data
    if (!sceneData.instances.empty()) {
        std::vector<SGPUInstanceData> gpuInstances;
        gpuInstances.reserve(sceneData.instances.size());

        for (const auto& inst : sceneData.instances) {
            SGPUInstanceData gpuInst = {};
            gpuInst.materialIndex = inst.materialIndex;
            gpuInstances.push_back(gpuInst);
        }

        RHI::BufferDesc instDesc;
        instDesc.size = static_cast<uint32_t>(gpuInstances.size() * sizeof(SGPUInstanceData));
        instDesc.usage = RHI::EBufferUsage::Structured;
        instDesc.cpuAccess = RHI::ECPUAccess::None;
        instDesc.structureByteStride = sizeof(SGPUInstanceData);

        m_instanceBuffer.reset(ctx->CreateBuffer(instDesc, gpuInstances.data()));
    }

    return true;
}

bool CDXRLightmapBaker::BuildAccelerationStructures(const SRayTracingSceneData& sceneData) {
    return m_asManager->BuildFromSceneData(sceneData);
}

// ============================================
// Main Baking
// ============================================

bool CDXRLightmapBaker::BakeVolumetricLightmap(
    CVolumetricLightmap& lightmap,
    CScene& scene,
    const SDXRBakeConfig& config)
{
    // Export scene geometry
    auto sceneData = CSceneGeometryExporter::ExportScene(scene);
    if (!sceneData) {
        CFFLog::Error("[DXRBaker] Failed to export scene geometry");
        return false;
    }

    return BakeVolumetricLightmap(lightmap, *sceneData, config);
}

bool CDXRLightmapBaker::BakeVolumetricLightmap(
    CVolumetricLightmap& lightmap,
    const SRayTracingSceneData& sceneData,
    const SDXRBakeConfig& config)
{
    if (!m_isReady) {
        if (!Initialize()) {
            CFFLog::Error("[DXRBaker] Failed to initialize baker");
            return false;
        }
    }

    // Verify lightmap has bricks to bake
    const auto& bricks = lightmap.GetBricks();
    if (bricks.empty()) {
        CFFLog::Error("[DXRBaker] Lightmap has no bricks - call BuildOctree first");
        return false;
    }

    m_totalBricks = static_cast<uint32_t>(bricks.size());
    CFFLog::Info("[DXRBaker] Starting volumetric lightmap bake (%u bricks)...", m_totalBricks);
    auto startTime = std::chrono::high_resolution_clock::now();

    // Store volume bounds from scene
    m_volumeMin = sceneData.sceneBoundsMin;
    m_volumeMax = sceneData.sceneBoundsMax;

    // Build acceleration structures
    CFFLog::Info("[DXRBaker] Building acceleration structures...");
    if (!BuildAccelerationStructures(sceneData)) {
        CFFLog::Error("[DXRBaker] Failed to build acceleration structures");
        return false;
    }

    // Upload scene data
    CFFLog::Info("[DXRBaker] Uploading scene data...");
    if (!UploadSceneData(sceneData)) {
        CFFLog::Error("[DXRBaker] Failed to upload scene data");
        return false;
    }

    // Create per-brick output and readback buffers
    CFFLog::Info("[DXRBaker] Creating output buffers...");
    if (!CreateOutputBuffer()) {
        CFFLog::Error("[DXRBaker] Failed to create output buffer");
        return false;
    }
    if (!CreateReadbackBuffer()) {
        CFFLog::Error("[DXRBaker] Failed to create readback buffer");
        return false;
    }

    // Create pipeline if not already done
    if (!m_pipeline) {
        CFFLog::Info("[DXRBaker] Creating ray tracing pipeline...");
        if (!CreatePipeline()) {
            CFFLog::Error("[DXRBaker] Failed to create ray tracing pipeline");
            return false;
        }

        // Create shader binding table
        CFFLog::Info("[DXRBaker] Creating shader binding table...");
        if (!CreateShaderBindingTable()) {
            CFFLog::Error("[DXRBaker] Failed to create shader binding table");
            return false;
        }
    }

    // Get mutable access to bricks for writing results
    auto& mutableBricks = const_cast<std::vector<SBrick>&>(bricks);

    // Per-brick dispatch loop
    CFFLog::Info("[DXRBaker] Dispatching %u bricks...", m_totalBricks);

    for (uint32_t brickIdx = 0; brickIdx < m_totalBricks; brickIdx++) {
        SBrick& brick = mutableBricks[brickIdx];

        // Dispatch ray tracing for this brick
        DispatchBakeBrick(brickIdx, brick, config);

        // Readback results to CPU
        ReadbackBrickResults(brick);

        // Progress callback
        if (config.progressCallback) {
            float progress = static_cast<float>(brickIdx + 1) / static_cast<float>(m_totalBricks);
            config.progressCallback(progress);
        }

        // Log progress periodically
        if ((brickIdx + 1) % 10 == 0 || brickIdx == m_totalBricks - 1) {
            float progress = 100.0f * (brickIdx + 1) / m_totalBricks;
            CFFLog::Info("[DXRBaker] Progress: %.1f%% (%u/%u bricks)", progress, brickIdx + 1, m_totalBricks);
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float elapsedSec = std::chrono::duration<float>(endTime - startTime).count();

    uint32_t totalSamples = config.samplesPerVoxel;
    CFFLog::Info("[DXRBaker] Baking complete in %.2f seconds", elapsedSec);
    CFFLog::Info("[DXRBaker] Total bricks: %u, samples per voxel: %u", m_totalBricks, totalSamples);

    return true;
}

void CDXRLightmapBaker::DispatchBakeBrick(uint32_t brickIndex, const SBrick& brick, const SDXRBakeConfig& config) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = ctx ? ctx->GetCommandList() : nullptr;
    if (!cmdList || !m_pipeline || !m_sbt) {
        return;
    }

    // Update constant buffer with per-brick parameters
    CB_BakeParams params = {};
    params.brickWorldMin = brick.worldMin;
    params.brickWorldMax = brick.worldMax;
    params.samplesPerVoxel = config.samplesPerVoxel;
    params.maxBounces = config.maxBounces;
    params.frameIndex = 0;  // Single pass per brick
    params.numLights = m_numLights;
    params.skyIntensity = config.skyIntensity;
    params.brickIndex = brickIndex;
    params.totalBricks = m_totalBricks;

    // Map and update constant buffer
    void* cbData = m_constantBuffer->Map();
    if (cbData) {
        memcpy(cbData, &params, sizeof(params));
        m_constantBuffer->Unmap();
    }

    // Set ray tracing pipeline state
    cmdList->SetRayTracingPipelineState(m_pipeline.get());

    // TODO: Resource binding for ray tracing requires root signature modifications
    // For now, the resources are not properly bound. Full implementation needs:
    // 1. Create a ray tracing root signature with:
    //    - CBV for CB_BakeParams (b0)
    //    - SRV for TLAS (t0)
    //    - SRV for Skybox texture (t1)
    //    - Sampler (s0)
    //    - SRV for Materials, Lights, Instances (t2-t4)
    //    - UAV for output buffer (u0)
    // 2. Bind resources via SetComputeRootDescriptorTable or SetComputeRootShaderResourceView

    // Bind TLAS (this is partially implemented)
    auto* tlas = m_asManager->GetTLAS();
    if (tlas) {
        cmdList->SetAccelerationStructure(0, tlas);
    }

    // Dispatch rays: 4x4x4 = 64 threads per brick
    RHI::DispatchRaysDesc dispatchDesc;
    dispatchDesc.shaderBindingTable = m_sbt.get();
    dispatchDesc.width = VL_BRICK_SIZE;   // 4
    dispatchDesc.height = VL_BRICK_SIZE;  // 4
    dispatchDesc.depth = VL_BRICK_SIZE;   // 4

    cmdList->DispatchRays(dispatchDesc);

    // Copy output buffer to readback buffer
    cmdList->CopyBuffer(m_readbackBuffer.get(), 0, m_outputBuffer.get(), 0,
                        VL_BRICK_VOXEL_COUNT * sizeof(SVoxelSHOutput));

    // Execute and wait for completion
    // Note: This is synchronous per-brick, which is simpler but not optimal
    // Future optimization: batch multiple bricks before sync
    ctx->ExecuteAndWait();
}

void CDXRLightmapBaker::ReadbackBrickResults(SBrick& brick) {
    if (!m_readbackBuffer) {
        return;
    }

    // Map readback buffer
    void* mappedData = m_readbackBuffer->Map();
    if (!mappedData) {
        CFFLog::Warning("[DXRBaker] Failed to map readback buffer");
        return;
    }

    // Copy to CPU-side storage
    memcpy(m_readbackData.data(), mappedData, VL_BRICK_VOXEL_COUNT * sizeof(SVoxelSHOutput));
    m_readbackBuffer->Unmap();

    // Copy SH data to brick
    for (int voxelIdx = 0; voxelIdx < VL_BRICK_VOXEL_COUNT; voxelIdx++) {
        const SVoxelSHOutput& output = m_readbackData[voxelIdx];

        // Copy 9 SH coefficients
        for (int shIdx = 0; shIdx < VL_SH_COEFF_COUNT; shIdx++) {
            brick.shData[voxelIdx][shIdx] = output.sh[shIdx];
        }

        // Copy validity
        brick.validity[voxelIdx] = output.validity > 0.5f;
    }
}

void CDXRLightmapBaker::CopyResultsToLightmap(CVolumetricLightmap& lightmap) {
    // Results are now copied directly in ReadbackBrickResults()
    // This function is kept for API compatibility but does nothing
    (void)lightmap;
}

void CDXRLightmapBaker::ReleasePerBakeResources() {
    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();
    m_readbackData.clear();

    m_numLights = 0;
    m_totalBricks = 0;
}
