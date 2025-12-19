#include "DX12AccelerationStructure.h"
#include "DX12Resources.h"
#include "DX12Context.h"
#include <cassert>

// ============================================
// DX12 Acceleration Structure Implementation
// ============================================

namespace RHI {
namespace DX12 {

// ============================================
// BLAS Constructor
// ============================================
CDX12AccelerationStructure::CDX12AccelerationStructure(
    ID3D12Device5* device,
    const BLASDesc& desc,
    IBuffer* scratchBuffer,
    IBuffer* resultBuffer)
    : m_type(EAccelerationStructureType::BottomLevel)
    , m_scratchBuffer(scratchBuffer)
    , m_resultBuffer(resultBuffer)
{
    assert(device != nullptr);
    assert(scratchBuffer != nullptr);
    assert(resultBuffer != nullptr);

    SetupBLASGeometry(desc);

    // Get prebuild info for size validation
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&m_buildInputs, &prebuildInfo);

    m_resultSize = prebuildInfo.ResultDataMaxSizeInBytes;
    m_scratchSize = prebuildInfo.ScratchDataSizeInBytes;
}

// ============================================
// TLAS Constructor
// ============================================
CDX12AccelerationStructure::CDX12AccelerationStructure(
    ID3D12Device5* device,
    const TLASDesc& desc,
    IBuffer* scratchBuffer,
    IBuffer* resultBuffer,
    IBuffer* instanceBuffer)
    : m_type(EAccelerationStructureType::TopLevel)
    , m_scratchBuffer(scratchBuffer)
    , m_resultBuffer(resultBuffer)
    , m_instanceBuffer(instanceBuffer)
{
    assert(device != nullptr);
    assert(scratchBuffer != nullptr);
    assert(resultBuffer != nullptr);
    assert(instanceBuffer != nullptr);

    SetupTLASInstances(desc, instanceBuffer);

    // Get prebuild info for size validation
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&m_buildInputs, &prebuildInfo);

    m_resultSize = prebuildInfo.ResultDataMaxSizeInBytes;
    m_scratchSize = prebuildInfo.ScratchDataSizeInBytes;
}

CDX12AccelerationStructure::~CDX12AccelerationStructure() {
    // Buffers are owned externally, don't release them here
    // The caller is responsible for buffer lifetime management
}

// ============================================
// IAccelerationStructure Interface
// ============================================

uint64_t CDX12AccelerationStructure::GetGPUVirtualAddress() const {
    if (m_resultBuffer) {
        auto* dx12Buffer = static_cast<CDX12Buffer*>(m_resultBuffer);
        return dx12Buffer->GetGPUVirtualAddress();
    }
    return 0;
}

void* CDX12AccelerationStructure::GetNativeHandle() {
    return GetResultBuffer();
}

ID3D12Resource* CDX12AccelerationStructure::GetResultBuffer() const {
    if (m_resultBuffer) {
        return static_cast<CDX12Buffer*>(m_resultBuffer)->GetD3D12Resource();
    }
    return nullptr;
}

ID3D12Resource* CDX12AccelerationStructure::GetScratchBuffer() const {
    if (m_scratchBuffer) {
        return static_cast<CDX12Buffer*>(m_scratchBuffer)->GetD3D12Resource();
    }
    return nullptr;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC CDX12AccelerationStructure::GetBuildDesc() const {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = m_buildInputs;
    buildDesc.DestAccelerationStructureData = GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData =
        static_cast<CDX12Buffer*>(m_scratchBuffer)->GetGPUVirtualAddress();
    return buildDesc;
}

// ============================================
// BLAS Setup
// ============================================

void CDX12AccelerationStructure::SetupBLASGeometry(const BLASDesc& desc) {
    m_geometryDescs.reserve(desc.geometries.size());

    for (const auto& geom : desc.geometries) {
        D3D12_RAYTRACING_GEOMETRY_DESC d3dGeom = {};
        d3dGeom.Flags = ConvertGeometryFlags(geom.flags);

        if (geom.type == EGeometryType::Triangles) {
            d3dGeom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;

            const auto& tri = geom.triangles;
            d3dGeom.Triangles.VertexBuffer.StartAddress =
                static_cast<CDX12Buffer*>(tri.vertexBuffer)->GetGPUVirtualAddress() + tri.vertexBufferOffset;
            d3dGeom.Triangles.VertexBuffer.StrideInBytes = tri.vertexStride;
            d3dGeom.Triangles.VertexCount = tri.vertexCount;
            d3dGeom.Triangles.VertexFormat = ToDXGIFormat(tri.vertexFormat);

            if (tri.indexBuffer) {
                d3dGeom.Triangles.IndexBuffer =
                    static_cast<CDX12Buffer*>(tri.indexBuffer)->GetGPUVirtualAddress() + tri.indexBufferOffset;
                d3dGeom.Triangles.IndexCount = tri.indexCount;
                d3dGeom.Triangles.IndexFormat =
                    (tri.indexFormat == EIndexFormat::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            }

            if (tri.transformBuffer) {
                d3dGeom.Triangles.Transform3x4 =
                    static_cast<CDX12Buffer*>(tri.transformBuffer)->GetGPUVirtualAddress() + tri.transformBufferOffset;
            }
        }
        else {
            // Procedural geometry (AABBs)
            d3dGeom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;

            const auto& proc = geom.procedural;
            d3dGeom.AABBs.AABBs.StartAddress =
                static_cast<CDX12Buffer*>(proc.aabbBuffer)->GetGPUVirtualAddress() + proc.aabbBufferOffset;
            d3dGeom.AABBs.AABBs.StrideInBytes = proc.aabbStride;
            d3dGeom.AABBs.AABBCount = proc.aabbCount;
        }

        m_geometryDescs.push_back(d3dGeom);
    }

    // Setup build inputs
    m_buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    m_buildInputs.Flags = ConvertBuildFlags(desc.buildFlags);
    m_buildInputs.NumDescs = static_cast<UINT>(m_geometryDescs.size());
    m_buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    m_buildInputs.pGeometryDescs = m_geometryDescs.data();
}

// ============================================
// TLAS Setup
// ============================================

void CDX12AccelerationStructure::SetupTLASInstances(const TLASDesc& desc, IBuffer* instanceBuffer) {
    // Setup build inputs
    m_buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    m_buildInputs.Flags = ConvertBuildFlags(desc.buildFlags);
    m_buildInputs.NumDescs = static_cast<UINT>(desc.instances.size());
    m_buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    m_buildInputs.InstanceDescs =
        static_cast<CDX12Buffer*>(instanceBuffer)->GetGPUVirtualAddress();
}

// ============================================
// Flag Conversion
// ============================================

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS
CDX12AccelerationStructure::ConvertBuildFlags(EAccelerationStructureBuildFlags flags) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS result = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

    if ((flags & EAccelerationStructureBuildFlags::AllowUpdate) != EAccelerationStructureBuildFlags::None) {
        result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    if ((flags & EAccelerationStructureBuildFlags::AllowCompaction) != EAccelerationStructureBuildFlags::None) {
        result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    }
    if ((flags & EAccelerationStructureBuildFlags::PreferFastTrace) != EAccelerationStructureBuildFlags::None) {
        result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    }
    if ((flags & EAccelerationStructureBuildFlags::PreferFastBuild) != EAccelerationStructureBuildFlags::None) {
        result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    }
    if ((flags & EAccelerationStructureBuildFlags::MinimizeMemory) != EAccelerationStructureBuildFlags::None) {
        result |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    }

    return result;
}

D3D12_RAYTRACING_GEOMETRY_FLAGS
CDX12AccelerationStructure::ConvertGeometryFlags(EGeometryFlags flags) {
    D3D12_RAYTRACING_GEOMETRY_FLAGS result = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

    if ((flags & EGeometryFlags::Opaque) != EGeometryFlags::None) {
        result |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    }
    if ((flags & EGeometryFlags::NoDuplicateAnyHit) != EGeometryFlags::None) {
        result |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
    }

    return result;
}

// ============================================
// Helper Functions
// ============================================

AccelerationStructurePrebuildInfo GetBLASPrebuildInfo(
    ID3D12Device5* device,
    const BLASDesc& desc)
{
    // Build temporary geometry descs
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    geometryDescs.reserve(desc.geometries.size());

    for (const auto& geom : desc.geometries) {
        D3D12_RAYTRACING_GEOMETRY_DESC d3dGeom = {};
        d3dGeom.Flags = CDX12AccelerationStructure::ConvertGeometryFlags(geom.flags);

        if (geom.type == EGeometryType::Triangles) {
            d3dGeom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            const auto& tri = geom.triangles;

            // For prebuild, we only need the counts and formats, not actual addresses
            d3dGeom.Triangles.VertexBuffer.StrideInBytes = tri.vertexStride;
            d3dGeom.Triangles.VertexCount = tri.vertexCount;
            d3dGeom.Triangles.VertexFormat = ToDXGIFormat(tri.vertexFormat);

            if (tri.indexBuffer) {
                d3dGeom.Triangles.IndexCount = tri.indexCount;
                d3dGeom.Triangles.IndexFormat =
                    (tri.indexFormat == EIndexFormat::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            }
        }
        else {
            d3dGeom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            const auto& proc = geom.procedural;
            d3dGeom.AABBs.AABBs.StrideInBytes = proc.aabbStride;
            d3dGeom.AABBs.AABBCount = proc.aabbCount;
        }

        geometryDescs.push_back(d3dGeom);
    }

    // Setup build inputs
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs = {};
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    buildInputs.Flags = CDX12AccelerationStructure::ConvertBuildFlags(desc.buildFlags);
    buildInputs.NumDescs = static_cast<UINT>(geometryDescs.size());
    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.pGeometryDescs = geometryDescs.data();

    // Query prebuild info
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO d3dPrebuildInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &d3dPrebuildInfo);

    AccelerationStructurePrebuildInfo result;
    result.resultDataMaxSizeInBytes = d3dPrebuildInfo.ResultDataMaxSizeInBytes;
    result.scratchDataSizeInBytes = d3dPrebuildInfo.ScratchDataSizeInBytes;
    result.updateScratchDataSizeInBytes = d3dPrebuildInfo.UpdateScratchDataSizeInBytes;

    return result;
}

AccelerationStructurePrebuildInfo GetTLASPrebuildInfo(
    ID3D12Device5* device,
    const TLASDesc& desc)
{
    // Setup build inputs
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs = {};
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildInputs.Flags = CDX12AccelerationStructure::ConvertBuildFlags(desc.buildFlags);
    buildInputs.NumDescs = static_cast<UINT>(desc.instances.size());
    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    // Query prebuild info
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO d3dPrebuildInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &d3dPrebuildInfo);

    AccelerationStructurePrebuildInfo result;
    result.resultDataMaxSizeInBytes = d3dPrebuildInfo.ResultDataMaxSizeInBytes;
    result.scratchDataSizeInBytes = d3dPrebuildInfo.ScratchDataSizeInBytes;
    result.updateScratchDataSizeInBytes = d3dPrebuildInfo.UpdateScratchDataSizeInBytes;

    return result;
}

size_t GetInstanceBufferSize(const TLASDesc& desc) {
    return desc.instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
}

void WriteInstanceData(void* destBuffer, const TLASDesc& desc) {
    auto* instanceDescs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(destBuffer);

    for (size_t i = 0; i < desc.instances.size(); ++i) {
        const auto& src = desc.instances[i];
        auto& dst = instanceDescs[i];

        // Copy 3x4 transform (row-major)
        memcpy(dst.Transform, src.transform, sizeof(float) * 12);

        dst.InstanceID = src.instanceID & 0xFFFFFF;  // 24-bit
        dst.InstanceMask = src.instanceMask;
        dst.InstanceContributionToHitGroupIndex = src.instanceContributionToHitGroupIndex & 0xFFFFFF;  // 24-bit
        dst.Flags = src.flags;

        // Get BLAS GPU address
        if (src.blas) {
            dst.AccelerationStructure = src.blas->GetGPUVirtualAddress();
        } else {
            dst.AccelerationStructure = 0;
        }
    }
}

} // namespace DX12
} // namespace RHI
