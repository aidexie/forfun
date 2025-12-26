#include "Lightmap2DGPUBaker.h"
#include "LightmapRasterizer.h"
#include "../RayTracing/DXRAccelerationStructureManager.h"
#include "../RayTracing/SceneGeometryExport.h"
#include "../../Scene.h"
#include "../../../RHI/RHIManager.h"
#include "../../../RHI/RHIResources.h"
#include "../../../RHI/RHIRayTracing.h"
#include "../../../RHI/ShaderCompiler.h"
#include "../../../Core/FFLog.h"
#include "../../../Core/PathManager.h"
#include <chrono>

using namespace DirectX;

// ============================================
// Constants
// ============================================

static constexpr uint32_t BATCH_SIZE = 1024;  // Texels per batch

// ============================================
// CLightmap2DGPUBaker Implementation
// ============================================

CLightmap2DGPUBaker::CLightmap2DGPUBaker() {
    m_asManager = std::make_unique<CDXRAccelerationStructureManager>();
}

CLightmap2DGPUBaker::~CLightmap2DGPUBaker() {
    Shutdown();
}

bool CLightmap2DGPUBaker::Initialize() {
    if (m_isReady) {
        return true;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[Lightmap2DGPUBaker] No render context available");
        return false;
    }

    if (!ctx->SupportsRaytracing()) {
        CFFLog::Warning("[Lightmap2DGPUBaker] Ray tracing not supported");
        return false;
    }

    // Initialize acceleration structure manager
    if (!m_asManager->Initialize()) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to initialize AS manager");
        return false;
    }

    // Create constant buffer
    if (!CreateConstantBuffer()) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create constant buffer");
        return false;
    }

    m_isReady = true;
    CFFLog::Info("[Lightmap2DGPUBaker] Initialized successfully");
    return true;
}

void CLightmap2DGPUBaker::Shutdown() {
    ReleasePerBakeResources();

    m_sbt.reset();
    m_rtPipeline.reset();
    m_rtShaderLibrary.reset();
    m_finalizePipeline.reset();
    m_finalizeShader.reset();
    m_constantBuffer.reset();

    if (m_asManager) {
        m_asManager->Shutdown();
    }

    m_isReady = false;
}

bool CLightmap2DGPUBaker::IsAvailable() const {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    return ctx && ctx->SupportsRaytracing();
}

// ============================================
// Initialization Helpers
// ============================================

bool CLightmap2DGPUBaker::CreateConstantBuffer() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::BufferDesc cbDesc;
    cbDesc.size = sizeof(CB_Lightmap2DBakeParams);
    cbDesc.usage = RHI::EBufferUsage::Constant;
    cbDesc.cpuAccess = RHI::ECPUAccess::Write;
    cbDesc.debugName = "Lightmap2D_ConstantBuffer";

    m_constantBuffer.reset(ctx->CreateBuffer(cbDesc, nullptr));
    return m_constantBuffer != nullptr;
}

bool CLightmap2DGPUBaker::CreatePipeline() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    if (!RHI::IsDXCompilerAvailable()) {
        CFFLog::Error("[Lightmap2DGPUBaker] DXCompiler not available");
        return false;
    }

    // Compile shader
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/DXR/Lightmap2DBake.hlsl";
    CFFLog::Info("[Lightmap2DGPUBaker] Compiling shader: %s", shaderPath.c_str());

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
        CFFLog::Error("[Lightmap2DGPUBaker] Shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    CFFLog::Info("[Lightmap2DGPUBaker] Shader compiled (%zu bytes)", compiled.bytecode.size());

    // Create shader from bytecode
    RHI::ShaderDesc shaderDesc;
    shaderDesc.type = RHI::EShaderType::Library;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();

    m_rtShaderLibrary.reset(ctx->CreateShader(shaderDesc));
    if (!m_rtShaderLibrary) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create shader library");
        return false;
    }

    // Create ray tracing pipeline
    RHI::RayTracingPipelineDesc pipelineDesc;
    pipelineDesc.shaderLibrary = m_rtShaderLibrary.get();
    pipelineDesc.maxPayloadSize = sizeof(float) * 16;  // SRayPayload
    pipelineDesc.maxAttributeSize = sizeof(float) * 2; // Barycentric
    pipelineDesc.maxRecursionDepth = 2;  // Primary + Shadow

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

    m_rtPipeline.reset(ctx->CreateRayTracingPipelineState(pipelineDesc));
    if (!m_rtPipeline) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create RT pipeline");
        return false;
    }

    // Create shader binding table
    RHI::ShaderBindingTableDesc sbtDesc;
    sbtDesc.pipeline = m_rtPipeline.get();

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
    if (!m_sbt) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create SBT");
        return false;
    }

    CFFLog::Info("[Lightmap2DGPUBaker] RT pipeline created successfully");
    return true;
}

bool CLightmap2DGPUBaker::CreateFinalizePipeline() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Compile finalize compute shader
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/Lightmap2DFinalize.cs.hlsl";
    CFFLog::Info("[Lightmap2DGPUBaker] Compiling finalize shader: %s", shaderPath.c_str());

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    RHI::SCompiledShader compiled = RHI::CompileShaderFromFile(
        shaderPath,
        "CSMain",
        "cs_5_0",
        nullptr,
        debugShaders
    );

    if (!compiled.success) {
        CFFLog::Error("[Lightmap2DGPUBaker] Finalize shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    // Create shader
    RHI::ShaderDesc shaderDesc;
    shaderDesc.type = RHI::EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();

    m_finalizeShader.reset(ctx->CreateShader(shaderDesc));
    if (!m_finalizeShader) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create finalize shader");
        return false;
    }

    // Create compute pipeline
    RHI::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = m_finalizeShader.get();
    pipelineDesc.debugName = "Lightmap2DFinalize";

    m_finalizePipeline.reset(ctx->CreateComputePipelineState(pipelineDesc));
    if (!m_finalizePipeline) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create finalize pipeline");
        return false;
    }

    CFFLog::Info("[Lightmap2DGPUBaker] Finalize pipeline created successfully");
    return true;
}

// ============================================
// Per-Bake Setup
// ============================================

bool CLightmap2DGPUBaker::PrepareBakeResources(const SRayTracingSceneData& sceneData) {
    // Build acceleration structures
    CFFLog::Info("[Lightmap2DGPUBaker] Building acceleration structures...");
    if (!BuildAccelerationStructures(sceneData)) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to build AS");
        return false;
    }

    // Upload scene data
    if (!UploadSceneData(sceneData)) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to upload scene data");
        return false;
    }

    // Create pipelines if needed
    if (!m_rtPipeline) {
        if (!CreatePipeline()) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to create RT pipeline");
            return false;
        }
    }

    if (!m_finalizePipeline) {
        if (!CreateFinalizePipeline()) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to create finalize pipeline");
            return false;
        }
    }

    return true;
}

bool CLightmap2DGPUBaker::UploadSceneData(const SRayTracingSceneData& sceneData) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();
    m_vertexBuffer.reset();
    m_indexBuffer.reset();

    // Upload materials
    if (!sceneData.materials.empty()) {
        std::vector<SGPUMaterialData2D> gpuMaterials;
        gpuMaterials.reserve(sceneData.materials.size());

        for (const auto& mat : sceneData.materials) {
            SGPUMaterialData2D gpuMat = {};
            gpuMat.albedo = mat.albedo;
            gpuMat.metallic = mat.metallic;
            gpuMat.roughness = mat.roughness;
            gpuMaterials.push_back(gpuMat);
        }

        RHI::BufferDesc matDesc;
        matDesc.size = static_cast<uint32_t>(gpuMaterials.size() * sizeof(SGPUMaterialData2D));
        matDesc.usage = RHI::EBufferUsage::Structured;
        matDesc.cpuAccess = RHI::ECPUAccess::None;
        matDesc.structureByteStride = sizeof(SGPUMaterialData2D);
        matDesc.debugName = "Lightmap2D_Materials";

        m_materialBuffer.reset(ctx->CreateBuffer(matDesc, gpuMaterials.data()));
    }

    // Upload lights
    if (!sceneData.lights.empty()) {
        std::vector<SGPULightData2D> gpuLights;
        gpuLights.reserve(sceneData.lights.size());

        for (const auto& light : sceneData.lights) {
            SGPULightData2D gpuLight = {};
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
        lightDesc.size = static_cast<uint32_t>(gpuLights.size() * sizeof(SGPULightData2D));
        lightDesc.usage = RHI::EBufferUsage::Structured;
        lightDesc.cpuAccess = RHI::ECPUAccess::None;
        lightDesc.structureByteStride = sizeof(SGPULightData2D);
        lightDesc.debugName = "Lightmap2D_Lights";

        m_lightBuffer.reset(ctx->CreateBuffer(lightDesc, gpuLights.data()));
        m_numLights = static_cast<uint32_t>(gpuLights.size());
    } else {
        m_numLights = 0;
    }

    // Upload instances
    if (!sceneData.instances.empty()) {
        std::vector<SGPUInstanceData2D> gpuInstances;
        gpuInstances.reserve(sceneData.instances.size());

        for (const auto& inst : sceneData.instances) {
            SGPUInstanceData2D gpuInst = {};
            gpuInst.materialIndex = inst.materialIndex;
            gpuInst.vertexBufferOffset = inst.vertexBufferOffset;
            gpuInst.indexBufferOffset = inst.indexBufferOffset;
            gpuInstances.push_back(gpuInst);
        }

        RHI::BufferDesc instDesc;
        instDesc.size = static_cast<uint32_t>(gpuInstances.size() * sizeof(SGPUInstanceData2D));
        instDesc.usage = RHI::EBufferUsage::Structured;
        instDesc.cpuAccess = RHI::ECPUAccess::None;
        instDesc.structureByteStride = sizeof(SGPUInstanceData2D);
        instDesc.debugName = "Lightmap2D_Instances";

        m_instanceBuffer.reset(ctx->CreateBuffer(instDesc, gpuInstances.data()));
    }

    // Upload global vertex positions
    if (!sceneData.globalVertexPositions.empty()) {
        RHI::BufferDesc vertexDesc;
        vertexDesc.size = static_cast<uint32_t>(sceneData.globalVertexPositions.size() * sizeof(XMFLOAT4));
        vertexDesc.usage = RHI::EBufferUsage::Structured;
        vertexDesc.cpuAccess = RHI::ECPUAccess::None;
        vertexDesc.structureByteStride = sizeof(XMFLOAT4);
        vertexDesc.debugName = "Lightmap2D_Vertices";

        m_vertexBuffer.reset(ctx->CreateBuffer(vertexDesc, sceneData.globalVertexPositions.data()));
    }

    // Upload global indices
    if (!sceneData.globalIndices.empty()) {
        RHI::BufferDesc indexDesc;
        indexDesc.size = static_cast<uint32_t>(sceneData.globalIndices.size() * sizeof(uint32_t));
        indexDesc.usage = RHI::EBufferUsage::Structured;
        indexDesc.cpuAccess = RHI::ECPUAccess::None;
        indexDesc.structureByteStride = sizeof(uint32_t);
        indexDesc.debugName = "Lightmap2D_Indices";

        m_indexBuffer.reset(ctx->CreateBuffer(indexDesc, sceneData.globalIndices.data()));
    }

    return true;
}

bool CLightmap2DGPUBaker::BuildAccelerationStructures(const SRayTracingSceneData& sceneData) {
    return m_asManager->BuildFromSceneData(sceneData);
}

// ============================================
// Texel Data Management
// ============================================

void CLightmap2DGPUBaker::LinearizeTexels(
    const std::vector<STexelData>& texels,
    uint32_t atlasWidth,
    uint32_t atlasHeight)
{
    m_linearizedTexels.clear();
    m_texelToAtlasX.clear();
    m_texelToAtlasY.clear();

    // Count valid texels and linearize
    for (uint32_t y = 0; y < atlasHeight; y++) {
        for (uint32_t x = 0; x < atlasWidth; x++) {
            uint32_t idx = y * atlasWidth + x;
            if (idx >= texels.size()) continue;

            const STexelData& texel = texels[idx];
            if (!texel.valid) continue;

            SGPUTexelData gpuTexel = {};
            gpuTexel.worldPos = texel.worldPos;
            gpuTexel.validity = 1.0f;
            gpuTexel.normal = texel.normal;
            gpuTexel.atlasX = x;
            gpuTexel.atlasY = y;

            m_linearizedTexels.push_back(gpuTexel);
            m_texelToAtlasX.push_back(x);
            m_texelToAtlasY.push_back(y);
        }
    }

    m_validTexelCount = static_cast<uint32_t>(m_linearizedTexels.size());
    CFFLog::Info("[Lightmap2DGPUBaker] Linearized %u valid texels from %ux%u atlas",
                 m_validTexelCount, atlasWidth, atlasHeight);
}

bool CLightmap2DGPUBaker::CreateTexelBuffer(uint32_t texelCount) {
    if (texelCount == 0) return true;

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::BufferDesc bufDesc;
    bufDesc.size = texelCount * sizeof(SGPUTexelData);
    bufDesc.usage = RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(SGPUTexelData);
    bufDesc.debugName = "Lightmap2D_TexelData";

    m_texelBuffer.reset(ctx->CreateBuffer(bufDesc, m_linearizedTexels.data()));
    return m_texelBuffer != nullptr;
}

void CLightmap2DGPUBaker::UploadTexelData() {
    if (m_linearizedTexels.empty()) return;

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Recreate buffer with new data
    RHI::BufferDesc bufDesc;
    bufDesc.size = static_cast<uint32_t>(m_linearizedTexels.size() * sizeof(SGPUTexelData));
    bufDesc.usage = RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(SGPUTexelData);
    bufDesc.debugName = "Lightmap2D_TexelData";

    m_texelBuffer.reset(ctx->CreateBuffer(bufDesc, m_linearizedTexels.data()));
}

// ============================================
// Baking
// ============================================

bool CLightmap2DGPUBaker::CreateAccumulationBuffer(uint32_t atlasWidth, uint32_t atlasHeight) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Accumulation buffer: float4 per texel (xyz = radiance, w = sample count)
    uint32_t bufferSize = atlasWidth * atlasHeight * sizeof(XMFLOAT4);

    RHI::BufferDesc bufDesc;
    bufDesc.size = bufferSize;
    bufDesc.usage = RHI::EBufferUsage::UnorderedAccess | RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(XMFLOAT4);
    bufDesc.debugName = "Lightmap2D_Accumulation";

    // Initialize to zero
    std::vector<XMFLOAT4> zeroData(atlasWidth * atlasHeight, XMFLOAT4{0, 0, 0, 0});

    m_accumulationBuffer.reset(ctx->CreateBuffer(bufDesc, zeroData.data()));
    return m_accumulationBuffer != nullptr;
}

bool CLightmap2DGPUBaker::CreateOutputTexture(uint32_t atlasWidth, uint32_t atlasHeight) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    RHI::TextureDesc texDesc;
    texDesc.width = atlasWidth;
    texDesc.height = atlasHeight;
    texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    texDesc.usage = RHI::ETextureUsage::UnorderedAccess | RHI::ETextureUsage::ShaderResource;
    texDesc.debugName = "Lightmap2D_Output";

    m_outputTexture.reset(ctx->CreateTexture(texDesc, nullptr));
    return m_outputTexture != nullptr;
}

void CLightmap2DGPUBaker::DispatchBake(const SLightmap2DGPUBakeConfig& config) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    auto* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Calculate number of batches
    uint32_t numBatches = (m_validTexelCount + BATCH_SIZE - 1) / BATCH_SIZE;

    CFFLog::Info("[Lightmap2DGPUBaker] Dispatching %u batches (%u texels, %u samples/texel)",
                 numBatches, m_validTexelCount, config.samplesPerTexel);

    // Set pipeline
    cmdList->SetRayTracingPipelineState(m_rtPipeline.get());

    // Bind TLAS (t0 is handled by SetAccelerationStructure)
    cmdList->SetAccelerationStructure(0, m_asManager->GetTLAS());

    // Bind SRVs using SetShaderResource/SetShaderResourceBuffer
    // t1=Skybox, t2=Materials, t3=Lights, t4=Instances, t5=Vertices, t6=Indices, t7=TexelData
    if (m_skyboxTexture) {
        cmdList->SetShaderResource(RHI::EShaderStage::Compute, 1, m_skyboxTexture);
    }
    if (m_materialBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 2, m_materialBuffer.get());
    }
    if (m_lightBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 3, m_lightBuffer.get());
    }
    if (m_instanceBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 4, m_instanceBuffer.get());
    }
    if (m_vertexBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 5, m_vertexBuffer.get());
    }
    if (m_indexBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 6, m_indexBuffer.get());
    }
    if (m_texelBuffer) {
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 7, m_texelBuffer.get());
    }

    // Bind UAV: u0=Accumulation
    cmdList->SetUnorderedAccess(0, m_accumulationBuffer.get());

    // Bind sampler
    if (m_skyboxSampler) {
        cmdList->SetSampler(RHI::EShaderStage::Compute, 0, m_skyboxSampler);
    }

    // Process each batch
    for (uint32_t batch = 0; batch < numBatches; batch++) {
        uint32_t batchOffset = batch * BATCH_SIZE;
        uint32_t batchSize = std::min(BATCH_SIZE, m_validTexelCount - batchOffset);

        // Update constant buffer
        CB_Lightmap2DBakeParams cbData = {};
        cbData.totalTexels = m_validTexelCount;
        cbData.samplesPerTexel = config.samplesPerTexel;
        cbData.maxBounces = config.maxBounces;
        cbData.skyIntensity = config.skyIntensity;
        cbData.atlasWidth = m_atlasWidth;
        cbData.atlasHeight = m_atlasHeight;
        cbData.batchOffset = batchOffset;
        cbData.batchSize = batchSize;
        cbData.frameIndex = batch;  // Use batch index for RNG variation
        cbData.numLights = m_numLights;

        // Use SetConstantBufferData for inline CB update
        cmdList->SetConstantBufferData(RHI::EShaderStage::Compute, 0, &cbData, sizeof(cbData));

        // Dispatch rays
        // Dispatch (batchSize, samplesPerTexel, 1)
        RHI::DispatchRaysDesc dispatchDesc = {};
        dispatchDesc.width = batchSize;
        dispatchDesc.height = config.samplesPerTexel;
        dispatchDesc.depth = 1;
        dispatchDesc.shaderBindingTable = m_sbt.get();

        cmdList->DispatchRays(dispatchDesc);

        // Report progress
        float progress = static_cast<float>(batch + 1) / numBatches * 0.8f;  // 80% for baking
        ReportProgress(progress, "Baking");
    }
}

void CLightmap2DGPUBaker::FinalizeAtlas() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    auto* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    ReportProgress(0.85f, "Finalizing atlas");

    // Set compute pipeline
    cmdList->SetPipelineState(m_finalizePipeline.get());

    // Bind resources
    // SRV: t0=Accumulation buffer
    cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Compute, 0, m_accumulationBuffer.get());

    // UAV: u0=Output texture
    cmdList->SetUnorderedAccessTexture(0, m_outputTexture.get());

    // Update constant buffer with atlas dimensions
    CB_Lightmap2DBakeParams cbData = {};
    cbData.atlasWidth = m_atlasWidth;
    cbData.atlasHeight = m_atlasHeight;

    // Use SetConstantBufferData for inline CB update
    cmdList->SetConstantBufferData(RHI::EShaderStage::Compute, 0, &cbData, sizeof(cbData));

    // Dispatch compute shader
    // Thread groups: (atlasWidth/8, atlasHeight/8, 1)
    uint32_t groupsX = (m_atlasWidth + 7) / 8;
    uint32_t groupsY = (m_atlasHeight + 7) / 8;

    cmdList->Dispatch(groupsX, groupsY, 1);

    ReportProgress(0.95f, "Finalize complete");
}

void CLightmap2DGPUBaker::DilateLightmap(int radius) {
    // TODO: Implement GPU-based dilation pass
    // For now, this is a placeholder
    ReportProgress(0.98f, "Dilation");
}

void CLightmap2DGPUBaker::ReleasePerBakeResources() {
    m_materialBuffer.reset();
    m_lightBuffer.reset();
    m_instanceBuffer.reset();
    m_vertexBuffer.reset();
    m_indexBuffer.reset();
    m_texelBuffer.reset();
    m_accumulationBuffer.reset();
    m_outputTexture.reset();

    m_linearizedTexels.clear();
    m_texelToAtlasX.clear();
    m_texelToAtlasY.clear();

    m_atlasWidth = 0;
    m_atlasHeight = 0;
    m_validTexelCount = 0;
    m_numLights = 0;
    m_skyboxTexture = nullptr;
    m_skyboxSampler = nullptr;

    if (m_asManager) {
        m_asManager->ClearAll();
    }
}

void CLightmap2DGPUBaker::ReportProgress(float progress, const char* stage) {
    if (m_progressCallback) {
        m_progressCallback(progress, stage);
    }
    CFFLog::Info("[Lightmap2DGPUBaker] %.0f%% - %s", progress * 100.0f, stage);
}

// ============================================
// Main Baking Entry Points
// ============================================

RHI::TexturePtr CLightmap2DGPUBaker::BakeLightmap(
    CScene& scene,
    const CLightmapRasterizer& rasterizer,
    const SLightmap2DGPUBakeConfig& config)
{
    // Get skybox texture
    m_skyboxTexture = scene.GetSkybox().GetEnvironmentTexture();
    m_skyboxSampler = scene.GetSkybox().GetEnvironmentTextureSampler();

    // Export scene geometry
    auto sceneData = CSceneGeometryExporter::ExportScene(scene);
    if (!sceneData) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to export scene geometry");
        return nullptr;
    }

    return BakeLightmap(
        *sceneData,
        rasterizer.GetTexels(),
        rasterizer.GetWidth(),
        rasterizer.GetHeight(),
        config);
}

RHI::TexturePtr CLightmap2DGPUBaker::BakeLightmap(
    const SRayTracingSceneData& sceneData,
    const std::vector<STexelData>& texels,
    uint32_t atlasWidth,
    uint32_t atlasHeight,
    const SLightmap2DGPUBakeConfig& config)
{
    m_progressCallback = config.progressCallback;

    auto startTime = std::chrono::high_resolution_clock::now();

    ReportProgress(0.0f, "Starting GPU lightmap bake");

    // Initialize if needed
    if (!m_isReady) {
        if (!Initialize()) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to initialize");
            return nullptr;
        }
    }

    // Store atlas dimensions
    m_atlasWidth = atlasWidth;
    m_atlasHeight = atlasHeight;

    // Phase 1: Prepare resources
    ReportProgress(0.05f, "Preparing resources");
    if (!PrepareBakeResources(sceneData)) {
        return nullptr;
    }

    // Phase 2: Linearize texels
    ReportProgress(0.10f, "Linearizing texels");
    LinearizeTexels(texels, atlasWidth, atlasHeight);

    if (m_validTexelCount == 0) {
        CFFLog::Warning("[Lightmap2DGPUBaker] No valid texels to bake");
        return nullptr;
    }

    // Phase 3: Create texel buffer
    if (!CreateTexelBuffer(m_validTexelCount)) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create texel buffer");
        return nullptr;
    }

    // Phase 4: Create accumulation buffer
    if (!CreateAccumulationBuffer(atlasWidth, atlasHeight)) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create accumulation buffer");
        return nullptr;
    }

    // Phase 5: Create output texture
    if (!CreateOutputTexture(atlasWidth, atlasHeight)) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create output texture");
        return nullptr;
    }

    // Phase 6: Dispatch baking
    ReportProgress(0.15f, "Baking");
    DispatchBake(config);

    // Phase 7: Finalize atlas
    FinalizeAtlas();

    // Phase 8: Dilation (optional)
    DilateLightmap(4);

    ReportProgress(1.0f, "Bake complete");

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    CFFLog::Info("[Lightmap2DGPUBaker] Bake complete: %ux%u atlas, %u texels, %.2f seconds",
                 atlasWidth, atlasHeight, m_validTexelCount, duration.count() / 1000.0f);

    return std::move(m_outputTexture);
}
