#include "DXRLightmapBaker.h"
#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include "../VolumetricLightmap.h"
#include "../../Scene.h"
#include "../../../RHI/RHIManager.h"
#include "../../../RHI/RHIResources.h"
#include "../../../RHI/RHIDescriptors.h"
#include "../../../RHI/RHIRayTracing.h"
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
    // Pipeline creation requires compiled DXR shader library
    // This would load LightmapBake.hlsl compiled to DXIL
    // For now, return false as shader compilation is not yet integrated

    CFFLog::Warning("[DXRBaker] Pipeline creation not yet implemented - requires DXIL shader compilation");
    return false;
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

bool CDXRLightmapBaker::CreateOutputTextures(uint32_t width, uint32_t height, uint32_t depth) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Release existing textures
    m_outputSH0.reset();
    m_outputSH1.reset();
    m_outputSH2.reset();
    m_outputValidity.reset();

    m_voxelGridWidth = width;
    m_voxelGridHeight = height;
    m_voxelGridDepth = depth;

    // Create 3D UAV textures for SH output
    RHI::TextureDesc texDesc;
    texDesc.width = width;
    texDesc.height = height;
    texDesc.depth = depth;
    texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    texDesc.usage = RHI::ETextureUsage::UnorderedAccess | RHI::ETextureUsage::ShaderResource;
    texDesc.dimension = RHI::ETextureDimension::Tex3D;

    m_outputSH0.reset(ctx->CreateTexture(texDesc, nullptr));
    m_outputSH1.reset(ctx->CreateTexture(texDesc, nullptr));
    m_outputSH2.reset(ctx->CreateTexture(texDesc, nullptr));

    // Validity uses same format for simplicity (only R channel used)
    m_outputValidity.reset(ctx->CreateTexture(texDesc, nullptr));

    return m_outputSH0 && m_outputSH1 && m_outputSH2 && m_outputValidity;
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

    CFFLog::Info("[DXRBaker] Starting volumetric lightmap bake...");
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

    // Get lightmap dimensions
    // For now, use a fixed grid size - in full implementation, this would
    // come from the lightmap's brick structure
    uint32_t gridSize = 64;  // 64^3 voxels for initial testing

    // Create output textures
    CFFLog::Info("[DXRBaker] Creating output textures (%ux%ux%u)...", gridSize, gridSize, gridSize);
    if (!CreateOutputTextures(gridSize, gridSize, gridSize)) {
        CFFLog::Error("[DXRBaker] Failed to create output textures");
        return false;
    }

    // Create pipeline if not already done
    if (!m_pipeline) {
        CFFLog::Warning("[DXRBaker] RT pipeline not available - shader compilation not implemented");
        CFFLog::Warning("[DXRBaker] DXR baking requires compiled DXIL shader library");

        // For now, we'll skip the actual dispatch since shaders aren't compiled
        // In a full implementation, this would:
        // 1. Load pre-compiled DXIL library
        // 2. Create RT pipeline state
        // 3. Create SBT
        // 4. Dispatch rays

        auto endTime = std::chrono::high_resolution_clock::now();
        float elapsedSec = std::chrono::duration<float>(endTime - startTime).count();
        CFFLog::Info("[DXRBaker] Setup complete in %.2f seconds (dispatch skipped - no shader)", elapsedSec);

        return true;  // Return true since setup succeeded
    }

    // Multi-pass accumulation
    CFFLog::Info("[DXRBaker] Dispatching %u accumulation passes...", config.accumulationPasses);

    for (uint32_t pass = 0; pass < config.accumulationPasses; pass++) {
        DispatchBakePass(pass, config);

        // Progress callback
        if (config.progressCallback) {
            float progress = static_cast<float>(pass + 1) / static_cast<float>(config.accumulationPasses);
            config.progressCallback(progress);
        }

        // Log progress periodically
        if ((pass + 1) % 8 == 0 || pass == config.accumulationPasses - 1) {
            float progress = 100.0f * (pass + 1) / config.accumulationPasses;
            CFFLog::Info("[DXRBaker] Progress: %.1f%%", progress);
        }
    }

    // Copy results to lightmap
    CFFLog::Info("[DXRBaker] Copying results to lightmap...");
    CopyResultsToLightmap(lightmap);

    auto endTime = std::chrono::high_resolution_clock::now();
    float elapsedSec = std::chrono::duration<float>(endTime - startTime).count();

    uint32_t totalSamples = config.samplesPerVoxel * config.accumulationPasses;
    CFFLog::Info("[DXRBaker] Baking complete in %.2f seconds", elapsedSec);
    CFFLog::Info("[DXRBaker] Total samples per voxel: %u", totalSamples);

    return true;
}

void CDXRLightmapBaker::DispatchBakePass(uint32_t passIndex, const SDXRBakeConfig& config) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = ctx ? ctx->GetCommandList() : nullptr;
    if (!cmdList || !m_pipeline || !m_sbt) {
        return;
    }

    // Update constant buffer
    CB_BakeParams params = {};
    params.volumeMin = m_volumeMin;
    params.volumeMax = m_volumeMax;
    params.voxelGridSize[0] = m_voxelGridWidth;
    params.voxelGridSize[1] = m_voxelGridHeight;
    params.voxelGridSize[2] = m_voxelGridDepth;
    params.samplesPerVoxel = config.samplesPerVoxel;
    params.maxBounces = config.maxBounces;
    params.frameIndex = passIndex;
    params.numLights = m_numLights;
    params.skyIntensity = config.skyIntensity;

    // Map and update constant buffer
    void* cbData = m_constantBuffer->Map();
    if (cbData) {
        memcpy(cbData, &params, sizeof(params));
        m_constantBuffer->Unmap();
    }

    // Set ray tracing pipeline state
    cmdList->SetRayTracingPipelineState(m_pipeline.get());

    // Bind resources
    // Note: Resource binding for RT is typically done through root signature
    // This is a simplified version - full implementation needs proper descriptor management

    // Dispatch rays
    RHI::DispatchRaysDesc dispatchDesc;
    dispatchDesc.shaderBindingTable = m_sbt.get();
    dispatchDesc.width = m_voxelGridWidth;
    dispatchDesc.height = m_voxelGridHeight;
    dispatchDesc.depth = m_voxelGridDepth;

    cmdList->DispatchRays(dispatchDesc);
}

void CDXRLightmapBaker::CopyResultsToLightmap(CVolumetricLightmap& lightmap) {
    // In a full implementation, this would:
    // 1. Read back GPU textures to CPU
    // 2. Unpack SH coefficients
    // 3. Copy to lightmap's brick data structure

    // For now, this is a placeholder
    CFFLog::Info("[DXRBaker] Result copy not yet implemented - requires GPU readback");
}

void CDXRLightmapBaker::ReleasePerBakeResources() {
    m_outputSH0.reset();
    m_outputSH1.reset();
    m_outputSH2.reset();
    m_outputValidity.reset();
    m_accumSH0.reset();
    m_accumSH1.reset();
    m_accumSH2.reset();
    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();

    m_voxelGridWidth = 0;
    m_voxelGridHeight = 0;
    m_voxelGridDepth = 0;
}
