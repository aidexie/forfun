#include "DXRAccelerationStructureManager.h"
#include "SceneGeometryExport.h"
#include "../../Scene.h"
#include "../../../RHI/RHIManager.h"
#include "../../../RHI/RHIResources.h"
#include "../../../RHI/RHIDescriptors.h"
#include "../../../Core/FFLog.h"

// ============================================
// CDXRAccelerationStructureManager Implementation
// ============================================

CDXRAccelerationStructureManager::~CDXRAccelerationStructureManager() {
    Shutdown();
}

bool CDXRAccelerationStructureManager::Initialize() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        CFFLog::Error("[DXRASManager] No render context available");
        return false;
    }

    // Check DXR support
    if (!ctx->SupportsRaytracing()) {
        CFFLog::Warning("[DXRASManager] Ray tracing not supported on this device");
        m_isAvailable = false;
        return false;
    }

    CFFLog::Info("[DXRASManager] Ray tracing supported");

    m_isAvailable = true;
    return true;
}

void CDXRAccelerationStructureManager::Shutdown() {
    ClearAll();
    m_isAvailable = false;
}

// ============================================
// Scene Building
// ============================================

bool CDXRAccelerationStructureManager::BuildFromSceneData(const SRayTracingSceneData& sceneData) {
    if (!m_isAvailable) {
        CFFLog::Error("[DXRASManager] DXR not available");
        return false;
    }

    // Clear existing structures
    ClearAll();

    CFFLog::Info("[DXRASManager] Building acceleration structures from scene data...");
    CFFLog::Info("[DXRASManager] Meshes: %zu, Instances: %zu",
                 sceneData.meshes.size(), sceneData.instances.size());

    // Build BLAS for each unique mesh
    for (size_t i = 0; i < sceneData.meshes.size(); ++i) {
        const auto& meshData = sceneData.meshes[i];

        int blasIndex = BuildBLAS(meshData);
        if (blasIndex < 0) {
            CFFLog::Error("[DXRASManager] Failed to build BLAS for mesh %zu (%s)",
                         i, meshData.sourcePath.c_str());
            // Continue with other meshes
        }
    }

    CFFLog::Info("[DXRASManager] Built %zu BLAS structures", m_blasList.size());

    // Build TLAS from instances
    if (!BuildTLAS(sceneData.instances)) {
        CFFLog::Error("[DXRASManager] Failed to build TLAS");
        return false;
    }

    CFFLog::Info("[DXRASManager] Acceleration structure build complete");
    return true;
}

bool CDXRAccelerationStructureManager::BuildFromScene(CScene& scene) {
    // Export scene geometry
    auto sceneData = CSceneGeometryExporter::ExportScene(scene);
    if (!sceneData) {
        CFFLog::Error("[DXRASManager] Failed to export scene geometry");
        return false;
    }

    return BuildFromSceneData(*sceneData);
}

// ============================================
// BLAS Building
// ============================================

int CDXRAccelerationStructureManager::BuildBLAS(const SRayTracingMeshData& meshData) {
    if (!m_isAvailable) {
        return -1;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        return -1;
    }

    // Validate mesh data
    if (meshData.positions.empty() || meshData.indices.empty()) {
        CFFLog::Warning("[DXRASManager] Empty mesh data for BLAS: %s",
                       meshData.sourcePath.c_str());
        return -1;
    }

    // Check for existing BLAS with same key
    std::string key = meshData.sourcePath + ":" + std::to_string(meshData.vertexCount);
    auto it = m_blasIndexMap.find(key);
    if (it != m_blasIndexMap.end()) {
        return it->second;  // Return existing BLAS index
    }

    // Create BLAS handle
    auto handle = std::make_unique<SBLASHandle>();
    handle->sourcePath = meshData.sourcePath;

    // Create geometry buffers
    if (!CreateGeometryBuffers(meshData, handle->vertexBuffer, handle->indexBuffer)) {
        CFFLog::Error("[DXRASManager] Failed to create geometry buffers for: %s",
                     meshData.sourcePath.c_str());
        return -1;
    }

    // Build BLAS descriptor
    RHI::BLASDesc blasDesc;
    blasDesc.buildFlags = RHI::EAccelerationStructureBuildFlags::PreferFastTrace;

    RHI::GeometryDesc geomDesc;
    geomDesc.type = RHI::EGeometryType::Triangles;
    geomDesc.flags = RHI::EGeometryFlags::Opaque;
    geomDesc.triangles.vertexBuffer = handle->vertexBuffer.get();
    geomDesc.triangles.vertexBufferOffset = 0;
    geomDesc.triangles.vertexCount = meshData.vertexCount;
    geomDesc.triangles.vertexStride = sizeof(DirectX::XMFLOAT3);
    geomDesc.triangles.vertexFormat = RHI::ETextureFormat::R32G32B32_FLOAT;
    geomDesc.triangles.indexBuffer = handle->indexBuffer.get();
    geomDesc.triangles.indexBufferOffset = 0;
    geomDesc.triangles.indexCount = meshData.indexCount;
    geomDesc.triangles.indexFormat = RHI::EIndexFormat::UInt32;

    blasDesc.geometries.push_back(geomDesc);

    // Get prebuild info for buffer sizing
    RHI::AccelerationStructurePrebuildInfo prebuildInfo = ctx->GetAccelerationStructurePrebuildInfo(blasDesc);

    if (prebuildInfo.resultDataMaxSizeInBytes == 0 || prebuildInfo.scratchDataSizeInBytes == 0) {
        CFFLog::Error("[DXRASManager] Invalid prebuild info for BLAS");
        return -1;
    }

    // Allocate buffers
    if (!AllocateASBuffers(prebuildInfo.scratchDataSizeInBytes,
                           prebuildInfo.resultDataMaxSizeInBytes,
                           handle->scratchBuffer, handle->resultBuffer)) {
        CFFLog::Error("[DXRASManager] Failed to allocate BLAS buffers");
        return -1;
    }

    // Create BLAS
    handle->accelerationStructure.reset(
        ctx->CreateBLAS(blasDesc, handle->scratchBuffer.get(), handle->resultBuffer.get()));

    if (!handle->accelerationStructure) {
        CFFLog::Error("[DXRASManager] Failed to create BLAS for: %s",
                     meshData.sourcePath.c_str());
        return -1;
    }

    // Execute build
    ExecuteBLASBuild(*handle);

    // Store handle
    int index = static_cast<int>(m_blasList.size());
    m_blasList.push_back(std::move(handle));
    m_blasIndexMap[key] = index;

    return index;
}

// ============================================
// TLAS Building
// ============================================

bool CDXRAccelerationStructureManager::BuildTLAS(const std::vector<SRayTracingInstance>& instances) {
    if (!m_isAvailable) {
        return false;
    }

    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        return false;
    }

    if (instances.empty()) {
        CFFLog::Warning("[DXRASManager] No instances for TLAS");
        return false;
    }

    // Clear existing TLAS
    ClearTLAS();

    // Create TLAS handle
    m_tlas = std::make_unique<STLASHandle>();
    m_tlas->instanceCount = static_cast<uint32_t>(instances.size());

    // Build TLAS descriptor
    RHI::TLASDesc tlasDesc;
    tlasDesc.buildFlags = RHI::EAccelerationStructureBuildFlags::PreferFastTrace;

    for (const auto& inst : instances) {
        // Validate BLAS reference
        if (inst.meshIndex >= m_blasList.size() || !m_blasList[inst.meshIndex]->IsValid()) {
            CFFLog::Warning("[DXRASManager] Invalid mesh index %u in instance, skipping",
                           inst.meshIndex);
            continue;
        }

        RHI::AccelerationStructureInstance asInst;

        // Copy transform (convert from XMFLOAT4X4 to DXR 3x4 format)
        // DirectX XMFLOAT4X4 is row-major with translation in row 3 (_41, _42, _43)
        // DXR expects 3x4 transposed format: rows become columns, translation in column 3
        // DXR layout: [Rx Ux Fx Tx]  where R=Right, U=Up, F=Forward, T=Translation
        //             [Ry Uy Fy Ty]
        //             [Rz Uz Fz Tz]
        asInst.transform[0][0] = inst.worldTransform._11;  // Rx
        asInst.transform[0][1] = inst.worldTransform._21;  // Ux (transposed)
        asInst.transform[0][2] = inst.worldTransform._31;  // Fx (transposed)
        asInst.transform[0][3] = inst.worldTransform._41;  // Tx (from row 3)
        asInst.transform[1][0] = inst.worldTransform._12;  // Ry (transposed)
        asInst.transform[1][1] = inst.worldTransform._22;  // Uy
        asInst.transform[1][2] = inst.worldTransform._32;  // Fy (transposed)
        asInst.transform[1][3] = inst.worldTransform._42;  // Ty (from row 3)
        asInst.transform[2][0] = inst.worldTransform._13;  // Rz (transposed)
        asInst.transform[2][1] = inst.worldTransform._23;  // Uz (transposed)
        asInst.transform[2][2] = inst.worldTransform._33;  // Fz
        asInst.transform[2][3] = inst.worldTransform._43;  // Tz (from row 3)

        asInst.instanceID = inst.instanceID;  // Used in shader via InstanceID() to index into g_Instances buffer
        asInst.instanceMask = inst.instanceMask;
        asInst.instanceContributionToHitGroupIndex = 0;  // All instances use hit group 0 (primary shader)
        asInst.flags = 0;
        asInst.blas = m_blasList[inst.meshIndex]->accelerationStructure.get();

        tlasDesc.instances.push_back(asInst);
    }

    if (tlasDesc.instances.empty()) {
        CFFLog::Error("[DXRASManager] No valid instances for TLAS");
        m_tlas.reset();
        return false;
    }

    // Get prebuild info
    RHI::AccelerationStructurePrebuildInfo prebuildInfo = ctx->GetAccelerationStructurePrebuildInfo(tlasDesc);

    if (prebuildInfo.resultDataMaxSizeInBytes == 0) {
        CFFLog::Error("[DXRASManager] Invalid prebuild info for TLAS");
        m_tlas.reset();
        return false;
    }

    // Allocate result and scratch buffers
    if (!AllocateASBuffers(prebuildInfo.scratchDataSizeInBytes,
                           prebuildInfo.resultDataMaxSizeInBytes,
                           m_tlas->scratchBuffer, m_tlas->resultBuffer)) {
        CFFLog::Error("[DXRASManager] Failed to allocate TLAS buffers");
        m_tlas.reset();
        return false;
    }

    // Allocate instance buffer
    size_t instanceBufferSize = tlasDesc.instances.size() * sizeof(RHI::AccelerationStructureInstance);
    RHI::BufferDesc instanceBufferDesc;
    instanceBufferDesc.size = static_cast<uint32_t>(instanceBufferSize);
    instanceBufferDesc.usage = RHI::EBufferUsage::Structured;  // Instance buffer for TLAS
    instanceBufferDesc.cpuAccess = RHI::ECPUAccess::Write;
    instanceBufferDesc.structureByteStride = sizeof(RHI::AccelerationStructureInstance);

    m_tlas->instanceBuffer.reset(ctx->CreateBuffer(instanceBufferDesc, nullptr));
    if (!m_tlas->instanceBuffer) {
        CFFLog::Error("[DXRASManager] Failed to allocate TLAS instance buffer");
        m_tlas.reset();
        return false;
    }

    // Create TLAS
    m_tlas->accelerationStructure.reset(
        ctx->CreateTLAS(tlasDesc, m_tlas->scratchBuffer.get(),
                        m_tlas->resultBuffer.get(), m_tlas->instanceBuffer.get()));

    if (!m_tlas->accelerationStructure) {
        CFFLog::Error("[DXRASManager] Failed to create TLAS");
        m_tlas.reset();
        return false;
    }

    // Execute build
    ExecuteTLASBuild();

    CFFLog::Info("[DXRASManager] TLAS built with %zu instances", tlasDesc.instances.size());
    return true;
}

// ============================================
// Buffer Creation
// ============================================

bool CDXRAccelerationStructureManager::CreateGeometryBuffers(
    const SRayTracingMeshData& meshData,
    std::unique_ptr<RHI::IBuffer>& outVertexBuffer,
    std::unique_ptr<RHI::IBuffer>& outIndexBuffer)
{
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        return false;
    }

    // Create vertex buffer (positions only for RT)
    RHI::BufferDesc vbDesc;
    vbDesc.size = static_cast<uint32_t>(meshData.positions.size() * sizeof(DirectX::XMFLOAT3));
    vbDesc.usage = RHI::EBufferUsage::Structured;  // For BLAS building (SRV access)
    vbDesc.cpuAccess = RHI::ECPUAccess::None;
    vbDesc.structureByteStride = sizeof(DirectX::XMFLOAT3);

    outVertexBuffer.reset(ctx->CreateBuffer(vbDesc, meshData.positions.data()));
    if (!outVertexBuffer) {
        return false;
    }

    // Create index buffer
    // NOTE: For DXR BLAS building, index buffer needs NON_PIXEL_SHADER_RESOURCE state,
    // so we use Structured usage instead of Index usage
    RHI::BufferDesc ibDesc;
    ibDesc.size = static_cast<uint32_t>(meshData.indices.size() * sizeof(uint32_t));
    ibDesc.usage = RHI::EBufferUsage::Structured;  // For BLAS building (SRV access, not Index)
    ibDesc.cpuAccess = RHI::ECPUAccess::None;
    ibDesc.structureByteStride = sizeof(uint32_t);

    outIndexBuffer.reset(ctx->CreateBuffer(ibDesc, meshData.indices.data()));
    if (!outIndexBuffer) {
        return false;
    }

    return true;
}

bool CDXRAccelerationStructureManager::AllocateASBuffers(
    uint64_t scratchSize,
    uint64_t resultSize,
    std::unique_ptr<RHI::IBuffer>& outScratchBuffer,
    std::unique_ptr<RHI::IBuffer>& outResultBuffer)
{
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    if (!ctx) {
        return false;
    }

    // Scratch buffer (UAV for building, starts in COMMON state)
    RHI::BufferDesc scratchDesc;
    scratchDesc.size = static_cast<uint32_t>(scratchSize);
    scratchDesc.usage = RHI::EBufferUsage::UnorderedAccess;
    scratchDesc.cpuAccess = RHI::ECPUAccess::None;

    outScratchBuffer.reset(ctx->CreateBuffer(scratchDesc, nullptr));
    if (!outScratchBuffer) {
        return false;
    }

    // Result buffer (acceleration structure storage)
    // Must be created with AccelerationStructure flag to get correct initial state
    RHI::BufferDesc resultDesc;
    resultDesc.size = static_cast<uint32_t>(resultSize);
    resultDesc.usage = RHI::EBufferUsage::AccelerationStructure;
    resultDesc.cpuAccess = RHI::ECPUAccess::None;

    outResultBuffer.reset(ctx->CreateBuffer(resultDesc, nullptr));
    if (!outResultBuffer) {
        return false;
    }

    return true;
}

// ============================================
// Build Execution
// ============================================

void CDXRAccelerationStructureManager::ExecuteBLASBuild(SBLASHandle& handle) {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = ctx ? ctx->GetCommandList() : nullptr;
    if (!cmdList || !handle.accelerationStructure) {
        return;
    }

    cmdList->BuildAccelerationStructure(handle.accelerationStructure.get());
}

void CDXRAccelerationStructureManager::ExecuteTLASBuild() {
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    auto* cmdList = ctx ? ctx->GetCommandList() : nullptr;
    if (!cmdList || !m_tlas || !m_tlas->accelerationStructure) {
        return;
    }

    cmdList->BuildAccelerationStructure(m_tlas->accelerationStructure.get());
}

// ============================================
// Accessors
// ============================================

RHI::IAccelerationStructure* CDXRAccelerationStructureManager::GetTLAS() const {
    return m_tlas ? m_tlas->accelerationStructure.get() : nullptr;
}

RHI::IAccelerationStructure* CDXRAccelerationStructureManager::GetBLAS(int index) const {
    if (index < 0 || index >= static_cast<int>(m_blasList.size())) {
        return nullptr;
    }
    return m_blasList[index]->accelerationStructure.get();
}

uint32_t CDXRAccelerationStructureManager::GetInstanceCount() const {
    return m_tlas ? m_tlas->instanceCount : 0;
}

// ============================================
// Resource Management
// ============================================

void CDXRAccelerationStructureManager::ClearBLAS() {
    m_blasList.clear();
    m_blasIndexMap.clear();

    // TLAS references are now invalid
    ClearTLAS();
}

void CDXRAccelerationStructureManager::ClearTLAS() {
    m_tlas.reset();
}

void CDXRAccelerationStructureManager::ClearAll() {
    ClearTLAS();
    ClearBLAS();
}
