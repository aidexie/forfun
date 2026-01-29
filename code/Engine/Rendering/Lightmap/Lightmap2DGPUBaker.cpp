#include "Lightmap2DGPUBaker.h"
#include "LightmapRasterizer.h"
#include "LightmapDenoiser.h"
#include "../RayTracing/DXRAccelerationStructureManager.h"
#include "../RayTracing/SceneGeometryExport.h"
#include "../ComputePassLayout.h"
#include "../../Scene.h"
#include "../../../RHI/RHIManager.h"
#include "../../../RHI/RHIResources.h"
#include "../../../RHI/RHIRayTracing.h"
#include "../../../RHI/ShaderCompiler.h"
#include "../../../RHI/IDescriptorSet.h"
#include "../../../RHI/RHIHelpers.h"
#include "../../../Core/FFLog.h"
#include "../../../Core/PathManager.h"
#include "../../../Core/Exporter/KTXExporter.h"
#include <chrono>
#include <cmath>

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

    // Initialize descriptor sets (DX12 only)
    InitDescriptorSets();

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

    // Cleanup descriptor set resources
    m_finalizeShader_ds.reset();
    m_finalizePipeline_ds.reset();
    m_dilateShader_ds.reset();
    m_dilatePipeline_ds.reset();

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (ctx) {
        if (m_computePerPassSet) {
            ctx->FreeDescriptorSet(m_computePerPassSet);
            m_computePerPassSet = nullptr;
        }
        if (m_computePerPassLayout) {
            ctx->DestroyDescriptorSetLayout(m_computePerPassLayout);
            m_computePerPassLayout = nullptr;
        }
    }

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

bool CLightmap2DGPUBaker::CreateDilatePipeline() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Compile dilate compute shader
    std::string shaderPath = FFPath::GetSourceDir() + "/Shader/Lightmap2DDilate.cs.hlsl";
    CFFLog::Info("[Lightmap2DGPUBaker] Compiling dilate shader: %s", shaderPath.c_str());

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
        CFFLog::Error("[Lightmap2DGPUBaker] Dilate shader compilation failed: %s", compiled.errorMessage.c_str());
        return false;
    }

    // Create shader
    RHI::ShaderDesc shaderDesc;
    shaderDesc.type = RHI::EShaderType::Compute;
    shaderDesc.bytecode = compiled.bytecode.data();
    shaderDesc.bytecodeSize = compiled.bytecode.size();

    m_dilateShader.reset(ctx->CreateShader(shaderDesc));
    if (!m_dilateShader) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create dilate shader");
        return false;
    }

    // Create compute pipeline
    RHI::ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = m_dilateShader.get();
    pipelineDesc.debugName = "Lightmap2DDilate";

    m_dilatePipeline.reset(ctx->CreateComputePipelineState(pipelineDesc));
    if (!m_dilatePipeline) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create dilate pipeline");
        return false;
    }

    CFFLog::Info("[Lightmap2DGPUBaker] Dilate pipeline created successfully");
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

    if (!m_dilatePipeline) {
        if (!CreateDilatePipeline()) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to create dilate pipeline");
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

    // Accumulation buffer: uint4 per texel (xyz = fixed-point radiance, w = sample count)
    // Using uint4 to enable atomic InterlockedAdd operations
    // Fixed-point scale: 65536 (defined in shader)
    struct UInt4 { uint32_t x, y, z, w; };
    uint32_t bufferSize = atlasWidth * atlasHeight * sizeof(UInt4);

    RHI::BufferDesc bufDesc;
    bufDesc.size = bufferSize;
    bufDesc.usage = RHI::EBufferUsage::UnorderedAccess | RHI::EBufferUsage::Structured;
    bufDesc.cpuAccess = RHI::ECPUAccess::None;
    bufDesc.structureByteStride = sizeof(UInt4);
    bufDesc.debugName = "Lightmap2D_Accumulation";

    // Initialize to zero
    std::vector<UInt4> zeroData(atlasWidth * atlasHeight, UInt4{0, 0, 0, 0});

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

// ============================================
// Baking (Legacy Binding Path)
// ============================================
#ifndef FF_LEGACY_BINDING_DISABLED

void CLightmap2DGPUBaker::DispatchBake(const SLightmap2DGPUBakeConfig& config) {
    CFFLog::Warning("[Lightmap2DGPUBaker] Using legacy binding path for DispatchBake - consider migrating to descriptor sets");

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
    CFFLog::Warning("[Lightmap2DGPUBaker] Using legacy binding path for FinalizeAtlas - consider migrating to descriptor sets");

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
    if (radius <= 0) {
        ReportProgress(0.98f, "Dilation skipped");
        return;
    }

    CFFLog::Warning("[Lightmap2DGPUBaker] Using legacy binding path for DilateLightmap - consider migrating to descriptor sets");

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx || !m_dilatePipeline || !m_outputTexture) {
        ReportProgress(0.98f, "Dilation skipped (no resources)");
        return;
    }

    auto* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Create temp texture for ping-pong if needed
    if (!m_dilateTemp) {
        RHI::TextureDesc texDesc;
        texDesc.width = m_atlasWidth;
        texDesc.height = m_atlasHeight;
        texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
        texDesc.usage = RHI::ETextureUsage::UnorderedAccess | RHI::ETextureUsage::ShaderResource;
        texDesc.debugName = "Lightmap2D_DilateTemp";

        m_dilateTemp.reset(ctx->CreateTexture(texDesc, nullptr));
        if (!m_dilateTemp) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to create dilate temp texture");
            return;
        }
    }

    CFFLog::Info("[Lightmap2DGPUBaker] Running %d dilation passes", radius);

    // Constant buffer for dilation params
    struct CB_DilateParams {
        uint32_t atlasWidth;
        uint32_t atlasHeight;
        uint32_t searchRadius;
        uint32_t padding;
    };

    CB_DilateParams cbData;
    cbData.atlasWidth = m_atlasWidth;
    cbData.atlasHeight = m_atlasHeight;
    cbData.searchRadius = 1;  // Search 1 pixel per pass
    cbData.padding = 0;

    uint32_t groupsX = (m_atlasWidth + 7) / 8;
    uint32_t groupsY = (m_atlasHeight + 7) / 8;

    // Ping-pong dilation passes
    // Pass 0: output -> temp
    // Pass 1: temp -> output
    // ...
    for (int pass = 0; pass < radius; pass++) {
        bool evenPass = (pass % 2) == 0;
        RHI::ITexture* inputTex = evenPass ? m_outputTexture.get() : m_dilateTemp.get();
        RHI::ITexture* outputTex = evenPass ? m_dilateTemp.get() : m_outputTexture.get();

        // Set pipeline
        cmdList->SetPipelineState(m_dilatePipeline.get());

        // Bind resources
        cmdList->SetShaderResource(RHI::EShaderStage::Compute, 0, inputTex);
        cmdList->SetUnorderedAccessTexture(0, outputTex);
        cmdList->SetConstantBufferData(RHI::EShaderStage::Compute, 0, &cbData, sizeof(cbData));

        // Dispatch
        cmdList->Dispatch(groupsX, groupsY, 1);

        // Barrier between passes
        cmdList->UAVBarrier(outputTex);
    }

    // If we ended on an odd pass, the result is in temp - copy back to output
    if ((radius % 2) == 1) {
        cmdList->CopyTexture(m_outputTexture.get(), m_dilateTemp.get());
    }

    ReportProgress(0.98f, "Dilation complete");
}

#endif // FF_LEGACY_BINDING_DISABLED

void CLightmap2DGPUBaker::DenoiseLightmap() {
    if (!m_enableDenoiser) {
        ReportProgress(0.99f, "Denoising skipped (disabled)");
        return;
    }

    if (!m_outputTexture || m_atlasWidth == 0 || m_atlasHeight == 0) {
        ReportProgress(0.99f, "Denoising skipped (no texture)");
        return;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[Lightmap2DGPUBaker] No render context for denoising");
        return;
    }

    ReportProgress(0.90f, "Initializing denoiser");

    // Initialize denoiser if needed
    if (!m_denoiser) {
        m_denoiser = std::make_unique<CLightmapDenoiser>();
        if (!m_denoiser->Initialize()) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to initialize OIDN denoiser");
            m_denoiser.reset();
            return;
        }
    }

    ReportProgress(0.91f, "Reading lightmap from GPU");

    // ============================================
    // Phase 1: GPU Readback
    // ============================================

    // Create staging texture for CPU read
    RHI::TextureDesc stagingDesc;
    stagingDesc.width = m_atlasWidth;
    stagingDesc.height = m_atlasHeight;
    stagingDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    stagingDesc.usage = RHI::ETextureUsage::Staging;
    stagingDesc.cpuAccess = RHI::ECPUAccess::Read;
    stagingDesc.debugName = "Lightmap2D_StagingRead";

    std::unique_ptr<RHI::ITexture> stagingTexture(ctx->CreateTexture(stagingDesc, nullptr));
    if (!stagingTexture) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create staging texture for readback");
        return;
    }

    // Copy output texture to staging (use CopyTextureToSlice for proper 2D texture copy)
    auto* cmdList = ctx->GetCommandList();
    if (cmdList) {
        cmdList->CopyTextureToSlice(stagingTexture.get(), 0, 0, m_outputTexture.get());
    }

    // Execute and wait for GPU to complete the copy
    ctx->ExecuteAndWait();

    // Map staging texture
    RHI::MappedTexture mapped = stagingTexture->Map(0, 0);
    if (!mapped.pData) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to map staging texture");
        return;
    }

    // Convert R16G16B16A16_FLOAT to float3 buffer for OIDN
    // R16G16B16A16_FLOAT = 8 bytes per pixel (4 x 16-bit float)
    uint32_t pixelCount = m_atlasWidth * m_atlasHeight;
    std::vector<float> colorBuffer(pixelCount * 3);  // RGB float

    const uint16_t* srcData = static_cast<const uint16_t*>(mapped.pData);
    uint32_t srcRowPitch = mapped.rowPitch / sizeof(uint16_t);  // Elements per row

    // Helper: Convert half-precision float to single-precision
    auto halfToFloat = [](uint16_t h) -> float {
        uint32_t sign = (h >> 15) & 0x1;
        uint32_t exp = (h >> 10) & 0x1F;
        uint32_t mant = h & 0x3FF;

        if (exp == 0) {
            if (mant == 0) {
                // Zero
                return sign ? -0.0f : 0.0f;
            } else {
                // Denormalized
                float m = mant / 1024.0f;
                return (sign ? -1.0f : 1.0f) * std::ldexp(m, -14);
            }
        } else if (exp == 31) {
            // Inf or NaN
            if (mant == 0) {
                return sign ? -INFINITY : INFINITY;
            } else {
                return NAN;
            }
        } else {
            // Normalized
            float m = 1.0f + mant / 1024.0f;
            return (sign ? -1.0f : 1.0f) * std::ldexp(m, (int)exp - 15);
        }
    };

    for (uint32_t y = 0; y < m_atlasHeight; y++) {
        for (uint32_t x = 0; x < m_atlasWidth; x++) {
            uint32_t srcIdx = y * srcRowPitch + x * 4;  // 4 components (RGBA)
            uint32_t dstIdx = (y * m_atlasWidth + x) * 3;  // 3 components (RGB)

            colorBuffer[dstIdx + 0] = halfToFloat(srcData[srcIdx + 0]);  // R
            colorBuffer[dstIdx + 1] = halfToFloat(srcData[srcIdx + 1]);  // G
            colorBuffer[dstIdx + 2] = halfToFloat(srcData[srcIdx + 2]);  // B
        }
    }

    stagingTexture->Unmap(0, 0);

    CFFLog::Info("[Lightmap2DGPUBaker] Read %ux%u lightmap from GPU", m_atlasWidth, m_atlasHeight);

    // Debug: Export before-denoise image to KTX2 (if enabled)
    if (m_debugExportImages) {
        std::string debugPath = FFPath::GetDebugDir() + "/lightmap_before_denoise.ktx2";
        if (CKTXExporter::Export2DFromFloat3Buffer(colorBuffer.data(), m_atlasWidth, m_atlasHeight, debugPath)) {
            CFFLog::Info("[Lightmap2DGPUBaker] Debug: Saved before-denoise image to %s", debugPath.c_str());
        } else {
            CFFLog::Warning("[Lightmap2DGPUBaker] Debug: Failed to save before-denoise image");
        }
    }

    // ============================================
    // Phase 2: OIDN Denoise
    // ============================================

    ReportProgress(0.93f, "Denoising with OIDN");

    if (!m_denoiser->Denoise(colorBuffer.data(), m_atlasWidth, m_atlasHeight)) {
        CFFLog::Error("[Lightmap2DGPUBaker] OIDN denoising failed: %s", m_denoiser->GetLastError());
        return;
    }

    // Debug: Export after-denoise image to KTX2 (if enabled)
    if (m_debugExportImages) {
        std::string debugPath = FFPath::GetDebugDir() + "/lightmap_after_denoise.ktx2";
        if (CKTXExporter::Export2DFromFloat3Buffer(colorBuffer.data(), m_atlasWidth, m_atlasHeight, debugPath)) {
            CFFLog::Info("[Lightmap2DGPUBaker] Debug: Saved after-denoise image to %s", debugPath.c_str());
        } else {
            CFFLog::Warning("[Lightmap2DGPUBaker] Debug: Failed to save after-denoise image");
        }
    }

    // ============================================
    // Phase 3: CPU to GPU Upload
    // ============================================

    ReportProgress(0.97f, "Uploading denoised lightmap to GPU");

    // Helper: Convert single-precision float to half-precision
    auto floatToHalf = [](float f) -> uint16_t {
        uint32_t fltInt = *reinterpret_cast<uint32_t*>(&f);
        uint32_t sign = (fltInt >> 31) & 0x1;
        int32_t exp = ((fltInt >> 23) & 0xFF) - 127;
        uint32_t mant = fltInt & 0x7FFFFF;

        if (exp > 15) {
            // Overflow to infinity
            return (sign << 15) | (31 << 10);
        } else if (exp < -14) {
            // Underflow to zero or denormal
            if (exp < -24) {
                return sign << 15;  // Zero
            }
            // Denormalized
            mant |= 0x800000;  // Add implicit bit
            int shift = -14 - exp;
            mant >>= shift;
            return (sign << 15) | ((mant >> 13) & 0x3FF);
        } else {
            // Normalized
            return (sign << 15) | ((exp + 15) << 10) | ((mant >> 13) & 0x3FF);
        }
    };

    // Convert float3 back to R16G16B16A16_FLOAT
    std::vector<uint16_t> uploadData(pixelCount * 4);  // RGBA half

    for (uint32_t i = 0; i < pixelCount; i++) {
        uploadData[i * 4 + 0] = floatToHalf(colorBuffer[i * 3 + 0]);  // R
        uploadData[i * 4 + 1] = floatToHalf(colorBuffer[i * 3 + 1]);  // G
        uploadData[i * 4 + 2] = floatToHalf(colorBuffer[i * 3 + 2]);  // B
        uploadData[i * 4 + 3] = floatToHalf(1.0f);  // A = 1.0
    }

    // Create staging texture for upload
    RHI::TextureDesc uploadStagingDesc;
    uploadStagingDesc.width = m_atlasWidth;
    uploadStagingDesc.height = m_atlasHeight;
    uploadStagingDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
    uploadStagingDesc.usage = RHI::ETextureUsage::Staging;
    uploadStagingDesc.cpuAccess = RHI::ECPUAccess::Write;
    uploadStagingDesc.debugName = "Lightmap2D_StagingWrite";

    std::unique_ptr<RHI::ITexture> uploadStaging(ctx->CreateTexture(uploadStagingDesc, nullptr));
    if (!uploadStaging) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create upload staging texture");
        return;
    }

    // Map and write data
    RHI::MappedTexture uploadMapped = uploadStaging->Map(0, 0);
    if (!uploadMapped.pData) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to map upload staging texture");
        return;
    }

    // Copy row by row (handle pitch)
    uint8_t* dstPtr = static_cast<uint8_t*>(uploadMapped.pData);
    const uint8_t* srcPtr = reinterpret_cast<const uint8_t*>(uploadData.data());
    uint32_t srcRowSize = m_atlasWidth * 4 * sizeof(uint16_t);  // 8 bytes per pixel

    for (uint32_t y = 0; y < m_atlasHeight; y++) {
        memcpy(dstPtr + y * uploadMapped.rowPitch, srcPtr + y * srcRowSize, srcRowSize);
    }

    uploadStaging->Unmap(0, 0);

    // Copy staging to output texture
    cmdList = ctx->GetCommandList();
    if (cmdList) {
        cmdList->CopyTextureToSlice(m_outputTexture.get(), 0, 0, uploadStaging.get());
    }

    // Execute and wait
    ctx->ExecuteAndWait();

    CFFLog::Info("[Lightmap2DGPUBaker] Uploaded denoised lightmap to GPU");
    ReportProgress(0.99f, "Denoising complete");
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
    m_dilateTemp.reset();

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
    m_enableDenoiser = config.enableDenoiser;
    m_debugExportImages = config.debugExportImages;

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
    // Note: Ray tracing dispatch still uses legacy binding as DXR descriptor sets
    // require more complex root signature setup. The compute passes (finalize, dilate)
    // use descriptor sets when available.
    ReportProgress(0.15f, "Baking");
    if (IsDescriptorSetModeAvailable()) {
        DispatchBake_DS(config);
    }
#ifndef FF_LEGACY_BINDING_DISABLED
    else {
        DispatchBake(config);
    }
#else
    else {
        CFFLog::Error("[Lightmap2DGPUBaker] Legacy binding disabled and descriptor sets not available for ray tracing");
        return nullptr;
    }
#endif

    // Phase 7: Finalize atlas
    if (IsDescriptorSetModeAvailable()) {
        FinalizeAtlas_DS();
    }
#ifndef FF_LEGACY_BINDING_DISABLED
    else {
        FinalizeAtlas();
    }
#else
    else {
        CFFLog::Error("[Lightmap2DGPUBaker] Legacy binding disabled and descriptor sets not available for finalize");
        return nullptr;
    }
#endif

    // Phase 8: Dilation (optional)
    if (IsDescriptorSetModeAvailable()) {
        DilateLightmap_DS(4);
    }
#ifndef FF_LEGACY_BINDING_DISABLED
    else {
        DilateLightmap(4);
    }
#else
    else {
        CFFLog::Error("[Lightmap2DGPUBaker] Legacy binding disabled and descriptor sets not available for dilation");
        return nullptr;
    }
#endif

    // Phase 9: OIDN Denoising (optional)
    DenoiseLightmap();

    ReportProgress(1.0f, "Bake complete");

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    CFFLog::Info("[Lightmap2DGPUBaker] Bake complete: %ux%u atlas, %u texels, %.2f seconds",
                 atlasWidth, atlasHeight, m_validTexelCount, duration.count() / 1000.0f);

    return std::move(m_outputTexture);
}

// ============================================
// Descriptor Set Support
// ============================================

bool CLightmap2DGPUBaker::IsDescriptorSetModeAvailable() const {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return false;

    // Descriptor sets are only supported on DX12
    return ctx->GetBackend() == RHI::EBackend::DX12 && m_computePerPassSet != nullptr;
}

void CLightmap2DGPUBaker::InitDescriptorSets() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    // Descriptor sets are only supported on DX12
    if (ctx->GetBackend() != RHI::EBackend::DX12) {
        CFFLog::Info("[Lightmap2DGPUBaker] DX11 mode - descriptor sets not supported");
        return;
    }

    // Create unified compute layout
    m_computePerPassLayout = ComputePassLayout::CreateComputePerPassLayout(ctx);
    if (!m_computePerPassLayout) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to create compute PerPass layout");
        return;
    }

    // Allocate descriptor set
    m_computePerPassSet = ctx->AllocateDescriptorSet(m_computePerPassLayout);
    if (!m_computePerPassSet) {
        CFFLog::Error("[Lightmap2DGPUBaker] Failed to allocate PerPass descriptor set");
        return;
    }

    // Compile SM 5.1 shaders for descriptor set path
#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Finalize shader (SM 5.1)
    std::string finalizeShaderPath = FFPath::GetSourceDir() + "/Shader/Lightmap2DFinalize_DS.cs.hlsl";
    RHI::SCompiledShader finalizeCompiled = RHI::CompileShaderFromFile(
        finalizeShaderPath,
        "CSMain",
        "cs_5_1",
        nullptr,
        debugShaders
    );

    if (finalizeCompiled.success) {
        RHI::ShaderDesc shaderDesc;
        shaderDesc.type = RHI::EShaderType::Compute;
        shaderDesc.bytecode = finalizeCompiled.bytecode.data();
        shaderDesc.bytecodeSize = finalizeCompiled.bytecode.size();
        shaderDesc.debugName = "Lightmap2DFinalize_DS";
        m_finalizeShader_ds.reset(ctx->CreateShader(shaderDesc));

        RHI::ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_finalizeShader_ds.get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = "Lightmap2DFinalize_DS_PSO";
        m_finalizePipeline_ds.reset(ctx->CreateComputePipelineState(psoDesc));
    } else {
        CFFLog::Warning("[Lightmap2DGPUBaker] Finalize DS shader not found or failed to compile: %s",
                        finalizeCompiled.errorMessage.c_str());
    }

    // Dilate shader (SM 5.1)
    std::string dilateShaderPath = FFPath::GetSourceDir() + "/Shader/Lightmap2DDilate_DS.cs.hlsl";
    RHI::SCompiledShader dilateCompiled = RHI::CompileShaderFromFile(
        dilateShaderPath,
        "CSMain",
        "cs_5_1",
        nullptr,
        debugShaders
    );

    if (dilateCompiled.success) {
        RHI::ShaderDesc shaderDesc;
        shaderDesc.type = RHI::EShaderType::Compute;
        shaderDesc.bytecode = dilateCompiled.bytecode.data();
        shaderDesc.bytecodeSize = dilateCompiled.bytecode.size();
        shaderDesc.debugName = "Lightmap2DDilate_DS";
        m_dilateShader_ds.reset(ctx->CreateShader(shaderDesc));

        RHI::ComputePipelineDesc psoDesc;
        psoDesc.computeShader = m_dilateShader_ds.get();
        psoDesc.setLayouts[1] = m_computePerPassLayout;  // Set 1: PerPass (space1)
        psoDesc.debugName = "Lightmap2DDilate_DS_PSO";
        m_dilatePipeline_ds.reset(ctx->CreateComputePipelineState(psoDesc));
    } else {
        CFFLog::Warning("[Lightmap2DGPUBaker] Dilate DS shader not found or failed to compile: %s",
                        dilateCompiled.errorMessage.c_str());
    }

    CFFLog::Info("[Lightmap2DGPUBaker] Descriptor set resources initialized");
}

// ============================================
// Baking (Descriptor Set Path)
// ============================================

// Note: Ray tracing dispatch uses legacy binding APIs because DXR requires specific
// root signature setup that differs from compute passes. The ray tracing pipeline
// uses SetAccelerationStructure, SetShaderResource, etc. which work with the DXR
// root signature. This is guarded with FF_LEGACY_BINDING_DISABLED.
//
// Future work: Create a dedicated DXR descriptor set layout for ray tracing passes.

#ifndef FF_LEGACY_BINDING_DISABLED

void CLightmap2DGPUBaker::DispatchBake_DS(const SLightmap2DGPUBakeConfig& config) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) return;

    auto* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Calculate number of batches
    uint32_t numBatches = (m_validTexelCount + BATCH_SIZE - 1) / BATCH_SIZE;

    CFFLog::Info("[Lightmap2DGPUBaker] Dispatching %u batches (%u texels, %u samples/texel) [DS path]",
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

#else // FF_LEGACY_BINDING_DISABLED

void CLightmap2DGPUBaker::DispatchBake_DS(const SLightmap2DGPUBakeConfig& /*config*/) {
    // DXR ray tracing requires legacy binding APIs for resource binding.
    // When FF_LEGACY_BINDING_DISABLED is defined, ray tracing baking is not available.
    // Future work: Implement DXR-specific descriptor set layout for ray tracing passes.
    CFFLog::Error("[Lightmap2DGPUBaker] DispatchBake_DS: DXR ray tracing requires legacy binding APIs. "
                  "Ray tracing baking is not available when FF_LEGACY_BINDING_DISABLED is defined.");
}

#endif // FF_LEGACY_BINDING_DISABLED

void CLightmap2DGPUBaker::FinalizeAtlas_DS() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx || !m_finalizePipeline_ds || !m_computePerPassSet) {
        CFFLog::Warning("[Lightmap2DGPUBaker] FinalizeAtlas_DS: Missing resources, falling back");
#ifndef FF_LEGACY_BINDING_DISABLED
        FinalizeAtlas();
#endif
        return;
    }

    auto* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    ReportProgress(0.85f, "Finalizing atlas (DS)");

    // Update constant buffer with atlas dimensions
    CB_Lightmap2DBakeParams cbData = {};
    cbData.atlasWidth = m_atlasWidth;
    cbData.atlasHeight = m_atlasHeight;

    // Bind resources to descriptor set
    m_computePerPassSet->Bind({
        RHI::BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cbData, sizeof(cbData)),
        RHI::BindingSetItem::Buffer_SRV(ComputePassLayout::Slots::Tex_Input0, m_accumulationBuffer.get()),
        RHI::BindingSetItem::Texture_UAV(ComputePassLayout::Slots::UAV_Output0, m_outputTexture.get())
    });

    cmdList->SetPipelineState(m_finalizePipeline_ds.get());
    cmdList->BindDescriptorSet(1, m_computePerPassSet);

    // Dispatch compute shader
    // Thread groups: (atlasWidth/8, atlasHeight/8, 1)
    uint32_t groupsX = (m_atlasWidth + 7) / 8;
    uint32_t groupsY = (m_atlasHeight + 7) / 8;

    cmdList->Dispatch(groupsX, groupsY, 1);

    ReportProgress(0.95f, "Finalize complete");
}

void CLightmap2DGPUBaker::DilateLightmap_DS(int radius) {
    if (radius <= 0) {
        ReportProgress(0.98f, "Dilation skipped");
        return;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx || !m_dilatePipeline_ds || !m_computePerPassSet || !m_outputTexture) {
        CFFLog::Warning("[Lightmap2DGPUBaker] DilateLightmap_DS: Missing resources, falling back");
#ifndef FF_LEGACY_BINDING_DISABLED
        DilateLightmap(radius);
#endif
        return;
    }

    auto* cmdList = ctx->GetCommandList();
    if (!cmdList) return;

    // Create temp texture for ping-pong if needed
    if (!m_dilateTemp) {
        RHI::TextureDesc texDesc;
        texDesc.width = m_atlasWidth;
        texDesc.height = m_atlasHeight;
        texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
        texDesc.usage = RHI::ETextureUsage::UnorderedAccess | RHI::ETextureUsage::ShaderResource;
        texDesc.debugName = "Lightmap2D_DilateTemp";

        m_dilateTemp.reset(ctx->CreateTexture(texDesc, nullptr));
        if (!m_dilateTemp) {
            CFFLog::Error("[Lightmap2DGPUBaker] Failed to create dilate temp texture");
            return;
        }
    }

    CFFLog::Info("[Lightmap2DGPUBaker] Running %d dilation passes (DS)", radius);

    // Constant buffer for dilation params
    struct CB_DilateParams {
        uint32_t atlasWidth;
        uint32_t atlasHeight;
        uint32_t searchRadius;
        uint32_t padding;
    };

    CB_DilateParams cbData;
    cbData.atlasWidth = m_atlasWidth;
    cbData.atlasHeight = m_atlasHeight;
    cbData.searchRadius = 1;  // Search 1 pixel per pass
    cbData.padding = 0;

    uint32_t groupsX = (m_atlasWidth + 7) / 8;
    uint32_t groupsY = (m_atlasHeight + 7) / 8;

    // Ping-pong dilation passes
    // Pass 0: output -> temp
    // Pass 1: temp -> output
    // ...
    for (int pass = 0; pass < radius; pass++) {
        bool evenPass = (pass % 2) == 0;
        RHI::ITexture* inputTex = evenPass ? m_outputTexture.get() : m_dilateTemp.get();
        RHI::ITexture* outputTex = evenPass ? m_dilateTemp.get() : m_outputTexture.get();

        // Bind resources to descriptor set
        m_computePerPassSet->Bind({
            RHI::BindingSetItem::VolatileCBV(ComputePassLayout::Slots::CB_PerPass, &cbData, sizeof(cbData)),
            RHI::BindingSetItem::Texture_SRV(ComputePassLayout::Slots::Tex_Input0, inputTex),
            RHI::BindingSetItem::Texture_UAV(ComputePassLayout::Slots::UAV_Output0, outputTex)
        });

        cmdList->SetPipelineState(m_dilatePipeline_ds.get());
        cmdList->BindDescriptorSet(1, m_computePerPassSet);

        // Dispatch
        cmdList->Dispatch(groupsX, groupsY, 1);

        // Barrier between passes
        cmdList->UAVBarrier(outputTex);
    }

    // If we ended on an odd pass, the result is in temp - copy back to output
    if ((radius % 2) == 1) {
        cmdList->CopyTexture(m_outputTexture.get(), m_dilateTemp.get());
    }

    ReportProgress(0.98f, "Dilation complete");
}
