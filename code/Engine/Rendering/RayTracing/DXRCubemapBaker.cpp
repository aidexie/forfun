#include "DXRCubemapBaker.h"
#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include "../VolumetricLightmap.h"
#include "../../Scene.h"
#include "../../../RHI/RHIManager.h"
#include "../../../RHI/RHIResources.h"
#include "../../../RHI/RHIRayTracing.h"
#include "../../../RHI/ShaderCompiler.h"
#include "../../../Core/FFLog.h"
#include "../../../Core/PathManager.h"
#include "../../../Core/SphericalHarmonics.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <ktx.h>

// ============================================
// CDXRCubemapBaker Implementation
// ============================================

CDXRCubemapBaker::CDXRCubemapBaker() {
    m_asManager = std::make_unique<CDXRAccelerationStructureManager>();
}

CDXRCubemapBaker::~CDXRCubemapBaker() {
    Shutdown();
}

bool CDXRCubemapBaker::Initialize() {
    if (m_isReady) {
        return true;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[CubemapBaker] No render context available");
        return false;
    }

    if (!ctx->SupportsRaytracing()) {
        CFFLog::Warning("[CubemapBaker] Ray tracing not supported");
        return false;
    }

    // Initialize acceleration structure manager
    if (!m_asManager->Initialize()) {
        CFFLog::Error("[CubemapBaker] Failed to initialize AS manager");
        return false;
    }

    // Create constant buffer
    if (!CreateConstantBuffer()) {
        CFFLog::Error("[CubemapBaker] Failed to create constant buffer");
        return false;
    }

    // Create cubemap output buffer
    if (!CreateCubemapOutputBuffer()) {
        CFFLog::Error("[CubemapBaker] Failed to create cubemap output buffer");
        return false;
    }

    // Create readback buffer
    if (!CreateCubemapReadbackBuffer()) {
        CFFLog::Error("[CubemapBaker] Failed to create readback buffer");
        return false;
    }

    m_isReady = true;
    CFFLog::Info("[CubemapBaker] Initialized successfully");
    return true;
}

void CDXRCubemapBaker::Shutdown() {

    ReleasePerBakeResources();

    m_sbt.reset();
    m_pipeline.reset();
    m_shaderLibrary.reset();
    m_constantBuffer.reset();
    m_cubemapOutputBuffer.reset();
    m_cubemapReadbackBuffer.reset();

    if (m_asManager) {
        m_asManager->Shutdown();
    }

    m_isReady = false;
}

bool CDXRCubemapBaker::IsAvailable() const {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    return ctx && ctx->SupportsRaytracing();
}

// ============================================
// Initialization Helpers
// ============================================

bool CDXRCubemapBaker::CreateConstantBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_CubemapBakeParams);
    cbDesc.usage = RHI::EBufferUsage::Constant;
    cbDesc.cpuAccess = RHI::ECPUAccess::Write;

    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));
    return m_constantBuffer != nullptr;
}

bool CDXRCubemapBaker::CreateCubemapOutputBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // 32x32x6 = 6144 pixels, each float4 (16 bytes)
    const uint32_t bufferSize = CUBEMAP_TOTAL_PIXELS * sizeof(DirectX::XMFLOAT4);

    RHI::BufferDesc bufDesc;
    bufDesc.size = bufferSize;
    bufDesc.usage = RHI::EBufferUsage::UnorderedAccess | RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(DirectX::XMFLOAT4);
    bufDesc.debugName = "CubemapBaker_Output";

    m_cubemapOutputBuffer.reset(ctx->CreateBuffer(bufDesc, nullptr));
    return m_cubemapOutputBuffer != nullptr;
}

bool CDXRCubemapBaker::CreateCubemapReadbackBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    const uint32_t bufferSize = CUBEMAP_TOTAL_PIXELS * sizeof(DirectX::XMFLOAT4);

    RHI::BufferDesc bufDesc;
    bufDesc.size = bufferSize;
    bufDesc.usage = RHI::EBufferUsage::Staging;
    bufDesc.cpuAccess = RHI::ECPUAccess::Read;
    bufDesc.debugName = "CubemapBaker_Readback";

    m_cubemapReadbackBuffer.reset(ctx->CreateBuffer(bufDesc, nullptr));
    if (!m_cubemapReadbackBuffer) return false;

    // Allocate CPU-side storage
    m_cubemapData.resize(CUBEMAP_TOTAL_PIXELS);
    return true;
}

bool CDXRCubemapBaker::CreatePipeline() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    if (!RHI::IsDXCompilerAvailable()) {
        CFFLog::Error("[CubemapBaker] DXCompiler not available");
        return false;
    }

    // Compile cubemap shader
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/DXR/LightmapBakeCubemap.hlsl";
    CFFLog::Info("[CubemapBaker] Compiling shader: %s", shaderPath.c_str());

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
        CFFLog::Error("[CubemapBaker] Shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    CFFLog::Info("[CubemapBaker] Shader compiled (%zu bytes)", compiled.bytecode.size());

    // Create shader from bytecode
    RHI::ShaderDesc shaderDesc;
    shaderDesc.type = RHI::EShaderType::Library;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();

    m_shaderLibrary.reset(ctx->CreateShader(shaderDesc));
    if (!m_shaderLibrary) {
        CFFLog::Error("[CubemapBaker] Failed to create shader library");
        return false;
    }

    // Create ray tracing pipeline
    RHI::RayTracingPipelineDesc pipelineDesc;
    pipelineDesc.shaderLibrary = m_shaderLibrary.get();
    pipelineDesc.maxPayloadSize = sizeof(float) * 16;
    pipelineDesc.maxAttributeSize = sizeof(float) * 2;
    pipelineDesc.maxRecursionDepth = 2;

    // Ray generation
    RHI::ShaderExport rayGenExport;
    rayGenExport.name = "RayGen";
    rayGenExport.type = RHI::EShaderExportType::RayGeneration;
    pipelineDesc.exports.push_back(rayGenExport);

    // Miss shaders
    RHI::ShaderExport missExport;
    missExport.name = "Miss";
    missExport.type = RHI::EShaderExportType::Miss;
    pipelineDesc.exports.push_back(missExport);

    RHI::ShaderExport shadowMissExport;
    shadowMissExport.name = "ShadowMiss";
    shadowMissExport.type = RHI::EShaderExportType::Miss;
    pipelineDesc.exports.push_back(shadowMissExport);

    // Hit groups
    RHI::HitGroupDesc primaryHitGroup;
    primaryHitGroup.name = "HitGroup";
    primaryHitGroup.closestHitShader = "ClosestHit";
    pipelineDesc.hitGroups.push_back(primaryHitGroup);

    RHI::HitGroupDesc shadowHitGroup;
    shadowHitGroup.name = "ShadowHitGroup";
    shadowHitGroup.anyHitShader = "ShadowAnyHit";
    pipelineDesc.hitGroups.push_back(shadowHitGroup);

    m_pipeline.reset(ctx->CreateRayTracingPipelineState(pipelineDesc));
    if (!m_pipeline) {
        CFFLog::Error("[CubemapBaker] Failed to create pipeline");
        return false;
    }

    CFFLog::Info("[CubemapBaker] Pipeline created successfully");
    return true;
}

bool CDXRCubemapBaker::CreateShaderBindingTable() {
    if (!m_pipeline) return false;

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::ShaderBindingTableDesc sbtDesc;
    sbtDesc.pipeline = m_pipeline.get();

    RHI::ShaderRecord rayGenRecord;
    rayGenRecord.exportName = "RayGen";
    sbtDesc.rayGenRecords.push_back(rayGenRecord);

    RHI::ShaderRecord missRecord;
    missRecord.exportName = "Miss";
    sbtDesc.missRecords.push_back(missRecord);

    RHI::ShaderRecord shadowMissRecord;
    shadowMissRecord.exportName = "ShadowMiss";
    sbtDesc.missRecords.push_back(shadowMissRecord);

    RHI::ShaderRecord hitGroupRecord;
    hitGroupRecord.exportName = "HitGroup";
    sbtDesc.hitGroupRecords.push_back(hitGroupRecord);

    RHI::ShaderRecord shadowHitRecord;
    shadowHitRecord.exportName = "ShadowHitGroup";
    sbtDesc.hitGroupRecords.push_back(shadowHitRecord);

    m_sbt.reset(ctx->CreateShaderBindingTable(sbtDesc));
    return m_sbt != nullptr;
}

bool CDXRCubemapBaker::UploadSceneData(const SRayTracingSceneData& sceneData) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();
    m_vertexBuffer.reset();
    m_indexBuffer.reset();

    // Upload materials
    if (!sceneData.materials.empty()) {
        std::vector<SGPUMaterialDataCubemap> gpuMaterials;
        gpuMaterials.reserve(sceneData.materials.size());

        for (const auto& mat : sceneData.materials) {
            SGPUMaterialDataCubemap gpuMat = {};
            gpuMat.albedo = mat.albedo;
            gpuMat.metallic = mat.metallic;
            gpuMat.roughness = mat.roughness;
            gpuMaterials.push_back(gpuMat);
        }

        RHI::BufferDesc matDesc;
        matDesc.size = static_cast<uint32_t>(gpuMaterials.size() * sizeof(SGPUMaterialDataCubemap));
        matDesc.usage = RHI::EBufferUsage::Structured;
        matDesc.cpuAccess = RHI::ECPUAccess::None;
        matDesc.structureByteStride = sizeof(SGPUMaterialDataCubemap);
        matDesc.debugName = "gpuMaterials";

        m_materialBuffer.reset(ctx->CreateBuffer(matDesc, gpuMaterials.data()));
    }

    // Upload lights
    if (!sceneData.lights.empty()) {
        std::vector<SGPULightDataCubemap> gpuLights;
        gpuLights.reserve(sceneData.lights.size());

        for (const auto& light : sceneData.lights) {
            SGPULightDataCubemap gpuLight = {};
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
        lightDesc.size = static_cast<uint32_t>(gpuLights.size() * sizeof(SGPULightDataCubemap));
        lightDesc.usage = RHI::EBufferUsage::Structured;
        lightDesc.cpuAccess = RHI::ECPUAccess::None;
        lightDesc.structureByteStride = sizeof(SGPULightDataCubemap);
        lightDesc.debugName = "gpuLights";

        m_lightBuffer.reset(ctx->CreateBuffer(lightDesc, gpuLights.data()));
        m_numLights = static_cast<uint32_t>(gpuLights.size());
    } else {
        m_numLights = 0;
    }

    // Upload instances (with buffer offsets for geometry lookup)
    if (!sceneData.instances.empty()) {
        std::vector<SGPUInstanceDataCubemap> gpuInstances;
        gpuInstances.reserve(sceneData.instances.size());

        for (const auto& inst : sceneData.instances) {
            SGPUInstanceDataCubemap gpuInst = {};
            gpuInst.materialIndex = inst.materialIndex;
            gpuInst.vertexBufferOffset = inst.vertexBufferOffset;
            gpuInst.indexBufferOffset = inst.indexBufferOffset;
            gpuInstances.push_back(gpuInst);
        }

        RHI::BufferDesc instDesc;
        instDesc.size = static_cast<uint32_t>(gpuInstances.size() * sizeof(SGPUInstanceDataCubemap));
        instDesc.usage = RHI::EBufferUsage::Structured;
        instDesc.cpuAccess = RHI::ECPUAccess::None;
        instDesc.structureByteStride = sizeof(SGPUInstanceDataCubemap);
        instDesc.debugName = "gpuInstances";

        m_instanceBuffer.reset(ctx->CreateBuffer(instDesc, gpuInstances.data()));
    }

    // Upload global vertex positions (float4 for alignment)
    if (!sceneData.globalVertexPositions.empty()) {
        RHI::BufferDesc vertexDesc;
        vertexDesc.size = static_cast<uint32_t>(sceneData.globalVertexPositions.size() * sizeof(DirectX::XMFLOAT4));
        vertexDesc.usage = RHI::EBufferUsage::Structured;
        vertexDesc.cpuAccess = RHI::ECPUAccess::None;
        vertexDesc.structureByteStride = sizeof(DirectX::XMFLOAT4);
        vertexDesc.debugName = "globalVertexPositions";

        m_vertexBuffer.reset(ctx->CreateBuffer(vertexDesc, sceneData.globalVertexPositions.data()));
        CFFLog::Info("[CubemapBaker] Uploaded %zu vertex positions", sceneData.globalVertexPositions.size());
    }

    // Upload global indices
    if (!sceneData.globalIndices.empty()) {
        RHI::BufferDesc indexDesc;
        indexDesc.size = static_cast<uint32_t>(sceneData.globalIndices.size() * sizeof(uint32_t));
        indexDesc.usage = RHI::EBufferUsage::Structured;
        indexDesc.cpuAccess = RHI::ECPUAccess::None;
        indexDesc.structureByteStride = sizeof(uint32_t);
        indexDesc.debugName = "globalIndices";

        m_indexBuffer.reset(ctx->CreateBuffer(indexDesc, sceneData.globalIndices.data()));
        CFFLog::Info("[CubemapBaker] Uploaded %zu indices", sceneData.globalIndices.size());
    }

    return true;
}

bool CDXRCubemapBaker::BuildAccelerationStructures(const SRayTracingSceneData& sceneData) {
    return m_asManager->BuildFromSceneData(sceneData);
}

// ============================================
// Main Baking
// ============================================

bool CDXRCubemapBaker::BakeVolumetricLightmap(
    CVolumetricLightmap& lightmap,
    CScene& scene,
    const SDXRCubemapBakeConfig& config)
{
    m_skyboxTexture = scene.GetSkybox().GetEnvironmentTexture();
    m_skyboxTextureSampler = scene.GetSkybox().GetEnvironmentTextureSampler();

    auto sceneData = CSceneGeometryExporter::ExportScene(scene);
    if (!sceneData) {
        CFFLog::Error("[CubemapBaker] Failed to export scene geometry");
        return false;
    }

    return BakeVolumetricLightmap(lightmap, *sceneData, config);
}

bool CDXRCubemapBaker::BakeVolumetricLightmap(
    CVolumetricLightmap& lightmap,
    const SRayTracingSceneData& sceneData,
    const SDXRCubemapBakeConfig& config)
{
    if (!m_isReady) {
        if (!Initialize()) {
            CFFLog::Error("[CubemapBaker] Failed to initialize");
            return false;
        }
    }

    const auto& bricks = lightmap.GetBricks();
    if (bricks.empty()) {
        CFFLog::Error("[CubemapBaker] No bricks to bake");
        return false;
    }

    // Phase 1: Prepare all resources (AS, pipeline, buffers)
    if (!PrepareBakeResources(sceneData)) {
        return false;
    }else{
        return true;
    }

    // // Phase 2: Dispatch bake for all voxels
    // return DispatchBakeAllVoxels(lightmap, config);
}

// ============================================
// Resource Preparation
// ============================================

bool CDXRCubemapBaker::PrepareBakeResources(const SRayTracingSceneData& sceneData) {
    // Build acceleration structures
    CFFLog::Info("[CubemapBaker] Building acceleration structures...");
    if (!BuildAccelerationStructures(sceneData)) {
        CFFLog::Error("[CubemapBaker] Failed to build AS");
        return false;
    }

    // Upload scene data
    if (!UploadSceneData(sceneData)) {
        CFFLog::Error("[CubemapBaker] Failed to upload scene data");
        return false;
    }

    // Create pipeline if needed
    if (!m_pipeline) {
        if (!CreatePipeline()) {
            CFFLog::Error("[CubemapBaker] Failed to create pipeline");
            return false;
        }
        if (!CreateShaderBindingTable()) {
            CFFLog::Error("[CubemapBaker] Failed to create SBT");
            return false;
        }
    }

    return true;
}

// ============================================
// Voxel Dispatch Loop
// ============================================

bool CDXRCubemapBaker::DispatchBakeAllVoxels(
    CVolumetricLightmap& lightmap,
    const SDXRCubemapBakeConfig& config)
{
    const auto& bricks = lightmap.GetBricks();
    auto& mutableBricks = const_cast<std::vector<SBrick>&>(bricks);

    auto startTime = std::chrono::high_resolution_clock::now();

    uint32_t totalVoxels = static_cast<uint32_t>(bricks.size()) * VL_BRICK_VOXEL_COUNT;
    uint32_t processedVoxels = 0;
    uint32_t debugCubemapsExported = 0;

    CFFLog::Info("[CubemapBaker] Starting cubemap-based bake: %zu bricks, %u voxels",
                 bricks.size(), totalVoxels);

    // Process each brick
    for (size_t brickIdx = 0; brickIdx < mutableBricks.size(); brickIdx++) {
        SBrick& brick = mutableBricks[brickIdx];

        // Calculate voxel size
        DirectX::XMFLOAT3 brickSize = {
            brick.worldMax.x - brick.worldMin.x,
            brick.worldMax.y - brick.worldMin.y,
            brick.worldMax.z - brick.worldMin.z
        };
        DirectX::XMFLOAT3 voxelSize = {
            brickSize.x / VL_BRICK_SIZE,
            brickSize.y / VL_BRICK_SIZE,
            brickSize.z / VL_BRICK_SIZE
        };

        // Process each voxel in the brick (4x4x4 = 64)
        for (uint32_t voxelIdx = VL_BRICK_VOXEL_COUNT-1; voxelIdx < VL_BRICK_VOXEL_COUNT; voxelIdx++) {
            // Calculate voxel local position
            uint32_t lx = voxelIdx % VL_BRICK_SIZE;
            uint32_t ly = (voxelIdx / VL_BRICK_SIZE) % VL_BRICK_SIZE;
            uint32_t lz = voxelIdx / (VL_BRICK_SIZE * VL_BRICK_SIZE);

            // Calculate world position (voxel sample point)
            // For 4 voxels (0,1,2,3), positions are at 0, 1/3, 2/3, 1 of brick size
            // This places voxel 0 and 3 at the brick edges
            float tx = (VL_BRICK_SIZE > 1) ? (float)lx / (VL_BRICK_SIZE - 1) : 0.5f;
            float ty = (VL_BRICK_SIZE > 1) ? (float)ly / (VL_BRICK_SIZE - 1) : 0.5f;
            float tz = (VL_BRICK_SIZE > 1) ? (float)lz / (VL_BRICK_SIZE - 1) : 0.5f;

            DirectX::XMFLOAT3 worldPos = {
                brick.worldMin.x + tx * brickSize.x,
                brick.worldMin.y + ty * brickSize.y,
                brick.worldMin.z + tz * brickSize.z
            };

            // Check validity first
            float validity = CheckVoxelValidity(worldPos);
            brick.validity[voxelIdx] = (validity > 0.5f);

            if (validity <= 0.5f) {
                // Invalid voxel - set zero SH
                for (int i = 0; i < VL_SH_COEFF_COUNT; i++) {
                    brick.shData[voxelIdx][i] = {0, 0, 0};
                }
                processedVoxels++;
                continue;
            }

            // Dispatch cubemap bake for this voxel
            uint32_t globalVoxelIdx = static_cast<uint32_t>(brickIdx) * VL_BRICK_VOXEL_COUNT + voxelIdx;
            DispatchBakeVoxel(worldPos, globalVoxelIdx, config);

            // Readback cubemap
            ReadbackCubemap();

            // Project cubemap to SH
            std::array<DirectX::XMFLOAT3, 9> sh;
            ProjectCubemapToSH(sh);

            // Store SH in brick
            for (int i = 0; i < VL_SH_COEFF_COUNT; i++) {
                brick.shData[voxelIdx][i] = sh[i];
            }

            // Export debug cubemap if requested
            if (config.debug.exportDebugCubemaps &&
                (config.debug.maxDebugCubemaps == 0 || debugCubemapsExported < config.debug.maxDebugCubemaps)) {

                std::string exportPath = config.debug.debugExportPath.empty()
                    ? FFPath::GetDebugDir() + "/CubemapBaker"
                    : config.debug.debugExportPath;

                ExportDebugCubemap(exportPath, static_cast<uint32_t>(brickIdx), voxelIdx);
                debugCubemapsExported++;
            }

            processedVoxels++;
        }

        // Progress callback and logging
        float progress = static_cast<float>(brickIdx + 1) / static_cast<float>(bricks.size());
        if (config.progressCallback) {
            config.progressCallback(progress);
        }

        if ((brickIdx + 1) % 10 == 0 || brickIdx == bricks.size() - 1) {
            CFFLog::Info("[CubemapBaker] Progress: %.1f%% (%zu/%zu bricks)",
                        progress * 100.0f, brickIdx + 1, bricks.size());
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float elapsedSec = std::chrono::duration<float>(endTime - startTime).count();

    CFFLog::Info("[CubemapBaker] Bake complete in %.2f seconds", elapsedSec);
    CFFLog::Info("[CubemapBaker] Processed %u voxels (%.1f voxels/sec)",
                 processedVoxels, processedVoxels / elapsedSec);

    return true;
}

// ============================================
// Voxel Baking
// ============================================

void CDXRCubemapBaker::DispatchBakeVoxel(
    const DirectX::XMFLOAT3& worldPos,
    uint32_t voxelIndex,
    const SDXRCubemapBakeConfig& config)
{
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = ctx ? ctx->GetCommandList() : nullptr;
    if (!cmdList || !m_pipeline || !m_sbt) return;

    // Update constant buffer
    CB_CubemapBakeParams params = {};
    params.voxelWorldPos = worldPos;
    params.maxBounces = config.maxBounces;
    params.frameIndex = 0;
    params.numLights = m_numLights;
    params.skyIntensity = config.skyIntensity;
    params.voxelIndex = voxelIndex;

    // Set pipeline
    cmdList->SetRayTracingPipelineState(m_pipeline.get());

    // b0: Constant buffer
    cmdList->SetConstantBufferData(RHI::EShaderStage::Compute, 0, &params, sizeof(params));

    // t0: TLAS
    auto* tlas = m_asManager->GetTLAS();
    if (tlas) {
        cmdList->SetAccelerationStructure(0, tlas);
    }

    // t1: Skybox
    if (m_skyboxTexture) {
        cmdList->SetShaderResource(RHI::EShaderStage::Compute, 1, m_skyboxTexture);
    }

    // t2-t4: Scene buffers
    if (m_materialBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 2, m_materialBuffer.get());
    }
    if (m_lightBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 3, m_lightBuffer.get());
    }
    if (m_instanceBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 4, m_instanceBuffer.get());
    }

    // t5-t6: Global geometry buffers (for normal computation)
    if (m_vertexBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 5, m_vertexBuffer.get());
    }
    if (m_indexBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 6, m_indexBuffer.get());
    }

    // u0: Cubemap output
    cmdList->SetUnorderedAccess(0, m_cubemapOutputBuffer.get());

    // s0: Sampler
    if (m_skyboxTextureSampler) {
        cmdList->SetSampler(RHI::EShaderStage::Compute, 0, m_skyboxTextureSampler);
    }

    // Dispatch: 32 x 32 x 6 (cubemap resolution x faces)
    RHI::DispatchRaysDesc dispatchDesc;
    dispatchDesc.shaderBindingTable = m_sbt.get();
    dispatchDesc.width = CUBEMAP_BAKE_RES;
    dispatchDesc.height = CUBEMAP_BAKE_RES;
    dispatchDesc.depth = CUBEMAP_BAKE_FACES;

    cmdList->DispatchRays(dispatchDesc);

    // Barriers and copy
    cmdList->UAVBarrier(m_cubemapOutputBuffer.get());
    cmdList->Barrier(m_cubemapOutputBuffer.get(), RHI::EResourceState::UnorderedAccess, RHI::EResourceState::CopySource);
    cmdList->CopyBuffer(m_cubemapReadbackBuffer.get(), 0, m_cubemapOutputBuffer.get(), 0,
                        CUBEMAP_TOTAL_PIXELS * sizeof(DirectX::XMFLOAT4));
    cmdList->Barrier(m_cubemapOutputBuffer.get(), RHI::EResourceState::CopySource, RHI::EResourceState::UnorderedAccess);

    ctx->ExecuteAndWait();
}

void CDXRCubemapBaker::ReadbackCubemap() {
    if (!m_cubemapReadbackBuffer) return;

    void* mappedData = m_cubemapReadbackBuffer->Map();
    if (mappedData) {
        memcpy(m_cubemapData.data(), mappedData, CUBEMAP_TOTAL_PIXELS * sizeof(DirectX::XMFLOAT4));
        m_cubemapReadbackBuffer->Unmap();
    }
}

void CDXRCubemapBaker::ProjectCubemapToSH(std::array<DirectX::XMFLOAT3, 9>& outSH) {
    // Initialize SH to zero
    for (auto& coeff : outSH) {
        coeff = {0, 0, 0};
    }

    // Project cubemap to SH using SphericalHarmonics utility
    // This matches the CPU baker's approach
    SphericalHarmonics::ProjectCubemapToSH(
        m_cubemapData.data(),
        CUBEMAP_BAKE_RES,
        outSH
    );
}

float CDXRCubemapBaker::CheckVoxelValidity(const DirectX::XMFLOAT3& worldPos) {
    // Simple validity check using 6 axis-aligned rays
    // Returns ratio of rays that don't hit geometry within short distance
    // This is a simplified version - could use GPU for this too

    // For now, assume all voxels are valid
    // Full implementation would trace rays similar to old shader
    return 1.0f;
}

void CDXRCubemapBaker::ExportDebugCubemap(const std::string& path, uint32_t brickIdx, uint32_t voxelIdx) {
    // Create directory
    std::filesystem::create_directories(path);

    std::string filename = path + "/cubemap_brick" + std::to_string(brickIdx) +
                          "_voxel" + std::to_string(voxelIdx) + ".ktx2";

    // Helper: float to half
    auto floatToHalf = [](float f) -> uint16_t {
        uint32_t x = *reinterpret_cast<uint32_t*>(&f);
        uint32_t sign = (x >> 16) & 0x8000;
        int exp = ((x >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = (x >> 13) & 0x3FF;
        if (exp <= 0) return (uint16_t)sign;
        if (exp >= 31) return (uint16_t)(sign | 0x7C00);
        return (uint16_t)(sign | (exp << 10) | mant);
    };

    // Prepare face data (RGBA16F)
    std::vector<std::vector<uint16_t>> faceData(6);
    for (int f = 0; f < 6; f++) {
        faceData[f].resize(CUBEMAP_BAKE_RES * CUBEMAP_BAKE_RES * 4);

        for (uint32_t i = 0; i < CUBEMAP_PIXELS_PER_FACE; i++) {
            uint32_t pixelIdx = f * CUBEMAP_PIXELS_PER_FACE + i;
            const auto& pixel = m_cubemapData[pixelIdx];

            int idx = i * 4;
            faceData[f][idx + 0] = floatToHalf(pixel.x);
            faceData[f][idx + 1] = floatToHalf(pixel.y);
            faceData[f][idx + 2] = floatToHalf(pixel.z);
            faceData[f][idx + 3] = floatToHalf(1.0f);
        }
    }

    // Create KTX2 texture
    ktxTextureCreateInfo createInfo = {};
    createInfo.vkFormat = 97;  // VK_FORMAT_R16G16B16A16_SFLOAT
    createInfo.baseWidth = CUBEMAP_BAKE_RES;
    createInfo.baseHeight = CUBEMAP_BAKE_RES;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1;
    createInfo.numLayers = 1;
    createInfo.numFaces = 6;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (result != KTX_SUCCESS) {
        CFFLog::Error("[CubemapBaker] Failed to create KTX2: %d", result);
        return;
    }

    for (int f = 0; f < 6; f++) {
        ktxTexture_SetImageFromMemory(
            ktxTexture(texture), 0, 0, f,
            reinterpret_cast<const ktx_uint8_t*>(faceData[f].data()),
            faceData[f].size() * sizeof(uint16_t)
        );
    }

    result = ktxTexture_WriteToNamedFile(ktxTexture(texture), filename.c_str());
    ktxTexture_Destroy(ktxTexture(texture));

    if (result == KTX_SUCCESS) {
        CFFLog::Info("[CubemapBaker] Exported: %s", filename.c_str());
    }
}

void CDXRCubemapBaker::ReleasePerBakeResources() {
    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();
    m_vertexBuffer.reset();
    m_indexBuffer.reset();
    m_numLights = 0;
}
