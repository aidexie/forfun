#pragma once

#include "../RHI/RHIRayTracing.h"
#include "SceneGeometryExport.h"
#include <memory>
#include <vector>
#include <unordered_map>

// ============================================
// DXR Acceleration Structure Manager
// ============================================
// High-level manager for building and maintaining BLAS/TLAS
// from exported scene geometry data.

namespace RHI {
    class IRenderContext;
    class IBuffer;
    class IAccelerationStructure;
}

class CScene;

// ============================================
// BLAS Handle
// ============================================
// Represents a built BLAS with its associated buffers

struct SBLASHandle {
    std::unique_ptr<RHI::IAccelerationStructure> accelerationStructure;
    std::unique_ptr<RHI::IBuffer> resultBuffer;
    std::unique_ptr<RHI::IBuffer> scratchBuffer;

    // Source data reference
    std::string sourcePath;
    uint32_t subMeshIndex = 0;

    // GPU buffers for geometry (kept alive for BLAS reference)
    std::unique_ptr<RHI::IBuffer> vertexBuffer;
    std::unique_ptr<RHI::IBuffer> indexBuffer;

    bool IsValid() const { return accelerationStructure != nullptr; }
};

// ============================================
// TLAS Handle
// ============================================
// Represents a built TLAS with its associated buffers

struct STLASHandle {
    std::unique_ptr<RHI::IAccelerationStructure> accelerationStructure;
    std::unique_ptr<RHI::IBuffer> resultBuffer;
    std::unique_ptr<RHI::IBuffer> scratchBuffer;
    std::unique_ptr<RHI::IBuffer> instanceBuffer;

    uint32_t instanceCount = 0;

    bool IsValid() const { return accelerationStructure != nullptr; }
};

// ============================================
// CDXRAccelerationStructureManager
// ============================================

class CDXRAccelerationStructureManager {
public:
    CDXRAccelerationStructureManager() = default;
    ~CDXRAccelerationStructureManager();

    // Non-copyable
    CDXRAccelerationStructureManager(const CDXRAccelerationStructureManager&) = delete;
    CDXRAccelerationStructureManager& operator=(const CDXRAccelerationStructureManager&) = delete;

    // Initialize manager (checks DXR support)
    bool Initialize();

    // Shutdown and release all resources
    void Shutdown();

    // Check if DXR is available
    bool IsAvailable() const { return m_isAvailable; }

    // ============================================
    // Scene Building
    // ============================================

    // Build all acceleration structures from exported scene data
    // This is the main entry point for scene setup
    bool BuildFromSceneData(const SRayTracingSceneData& sceneData);

    // Build from a live scene (exports geometry internally)
    bool BuildFromScene(CScene& scene);

    // ============================================
    // Individual Building
    // ============================================

    // Build a single BLAS from mesh data
    // Returns index into m_blasList, or -1 on failure
    int BuildBLAS(const SRayTracingMeshData& meshData);

    // Build TLAS from instances
    // Each instance references a BLAS by index
    bool BuildTLAS(const std::vector<SRayTracingInstance>& instances);

    // ============================================
    // Accessors
    // ============================================

    // Get TLAS for shader binding (returns nullptr if not built)
    RHI::IAccelerationStructure* GetTLAS() const;

    // Get BLAS by index
    RHI::IAccelerationStructure* GetBLAS(int index) const;

    // Get BLAS count
    size_t GetBLASCount() const { return m_blasList.size(); }

    // Get instance count in current TLAS
    uint32_t GetInstanceCount() const;

    // ============================================
    // Resource Management
    // ============================================

    // Clear all BLAS (also invalidates TLAS)
    void ClearBLAS();

    // Clear TLAS only
    void ClearTLAS();

    // Clear everything
    void ClearAll();

private:
    // Create GPU buffers for mesh geometry
    bool CreateGeometryBuffers(
        const SRayTracingMeshData& meshData,
        std::unique_ptr<RHI::IBuffer>& outVertexBuffer,
        std::unique_ptr<RHI::IBuffer>& outIndexBuffer);

    // Allocate scratch and result buffers for acceleration structure
    bool AllocateASBuffers(
        uint64_t scratchSize,
        uint64_t resultSize,
        std::unique_ptr<RHI::IBuffer>& outScratchBuffer,
        std::unique_ptr<RHI::IBuffer>& outResultBuffer);

    // Execute BLAS build on command list
    void ExecuteBLASBuild(SBLASHandle& handle);

    // Execute TLAS build on command list
    void ExecuteTLASBuild();

private:
    bool m_isAvailable = false;

    // Built acceleration structures
    std::vector<std::unique_ptr<SBLASHandle>> m_blasList;
    std::unique_ptr<STLASHandle> m_tlas;

    // Map from mesh key to BLAS index (for deduplication)
    // Key format: "path:subMeshIndex"
    std::unordered_map<std::string, int> m_blasIndexMap;
};
