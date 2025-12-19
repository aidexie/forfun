#pragma once

#include "DX12Common.h"
#include "../RHIRayTracing.h"

// ============================================
// DX12 Acceleration Structure Implementation
// ============================================
// Implements IAccelerationStructure for DXR ray tracing.
// Supports both BLAS (geometry) and TLAS (instances).

namespace RHI {
namespace DX12 {

// Forward declarations
class CDX12Buffer;

class CDX12AccelerationStructure : public IAccelerationStructure {
public:
    // Create BLAS
    CDX12AccelerationStructure(
        ID3D12Device5* device,
        const BLASDesc& desc,
        IBuffer* scratchBuffer,
        IBuffer* resultBuffer);

    // Create TLAS
    CDX12AccelerationStructure(
        ID3D12Device5* device,
        const TLASDesc& desc,
        IBuffer* scratchBuffer,
        IBuffer* resultBuffer,
        IBuffer* instanceBuffer);

    ~CDX12AccelerationStructure() override;

    // IAccelerationStructure interface
    EAccelerationStructureType GetType() const override { return m_type; }
    uint64_t GetGPUVirtualAddress() const override;
    void* GetNativeHandle() override;
    uint64_t GetResultSize() const override { return m_resultSize; }
    uint64_t GetScratchSize() const override { return m_scratchSize; }

    // DX12-specific accessors
    ID3D12Resource* GetResultBuffer() const;
    ID3D12Resource* GetScratchBuffer() const;

    // Get the build inputs for command list execution
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& GetBuildInputs() const {
        return m_buildInputs;
    }

    // Get the build desc for command list execution
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC GetBuildDesc() const;

    // Check if build is pending (needs BuildAccelerationStructure call)
    bool IsBuildPending() const { return m_buildPending; }
    void MarkBuilt() { m_buildPending = false; }

    // Flag conversion helpers (public for use by helper functions)
    static D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS
        ConvertBuildFlags(EAccelerationStructureBuildFlags flags);

    static D3D12_RAYTRACING_GEOMETRY_FLAGS
        ConvertGeometryFlags(EGeometryFlags flags);

private:
    // Setup geometry descs for BLAS
    void SetupBLASGeometry(const BLASDesc& desc);

    // Setup instance descs for TLAS
    void SetupTLASInstances(const TLASDesc& desc, IBuffer* instanceBuffer);

private:
    EAccelerationStructureType m_type;

    // D3D12 build inputs (cached for rebuild/refit)
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS m_buildInputs = {};

    // Geometry descriptors for BLAS (stored for lifetime)
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> m_geometryDescs;

    // Buffer references (owned externally, but we need to track them)
    IBuffer* m_scratchBuffer = nullptr;
    IBuffer* m_resultBuffer = nullptr;
    IBuffer* m_instanceBuffer = nullptr;  // TLAS only

    // Size information
    uint64_t m_resultSize = 0;
    uint64_t m_scratchSize = 0;

    // Build state
    bool m_buildPending = true;
};

// ============================================
// Helper Functions
// ============================================

// Get prebuild info for acceleration structure sizing
AccelerationStructurePrebuildInfo GetBLASPrebuildInfo(
    ID3D12Device5* device,
    const BLASDesc& desc);

AccelerationStructurePrebuildInfo GetTLASPrebuildInfo(
    ID3D12Device5* device,
    const TLASDesc& desc);

// Build D3D12 instance desc buffer from RHI instances
// Returns the size needed for the instance buffer
size_t GetInstanceBufferSize(const TLASDesc& desc);

// Write instance data to a mapped buffer
void WriteInstanceData(
    void* destBuffer,
    const TLASDesc& desc);

} // namespace DX12
} // namespace RHI
