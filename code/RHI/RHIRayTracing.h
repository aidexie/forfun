#pragma once

#include "RHICommon.h"
#include "RHIDescriptors.h"
#include "RHIResources.h"
#include <vector>

// ============================================
// RHI Ray Tracing Interfaces
// ============================================
// DXR abstraction layer for ray tracing support.
// These interfaces enable GPU-accelerated ray tracing for
// lightmap baking and other offline rendering tasks.

namespace RHI {

// Forward declarations
class IBuffer;
class IShader;

// ============================================
// Acceleration Structure Types
// ============================================

enum class EAccelerationStructureType {
    BottomLevel,    // BLAS - geometry (triangles/procedural)
    TopLevel        // TLAS - instances
};

enum class EGeometryType {
    Triangles,      // Triangle mesh
    Procedural      // AABBs for custom intersection
};

enum class EGeometryFlags : uint32_t {
    None = 0,
    Opaque = 1 << 0,                    // Skip any-hit shader
    NoDuplicateAnyHit = 1 << 1          // Any-hit called once per primitive
};

enum class EAccelerationStructureBuildFlags : uint32_t {
    None = 0,
    AllowUpdate = 1 << 0,               // Enable refit updates
    AllowCompaction = 1 << 1,           // Enable post-build compaction
    PreferFastTrace = 1 << 2,           // Optimize for trace performance
    PreferFastBuild = 1 << 3,           // Optimize for build performance
    MinimizeMemory = 1 << 4             // Minimize memory footprint
};

// Enable bitwise operators for flags
inline EGeometryFlags operator|(EGeometryFlags a, EGeometryFlags b) {
    return static_cast<EGeometryFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline EGeometryFlags operator&(EGeometryFlags a, EGeometryFlags b) {
    return static_cast<EGeometryFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline EAccelerationStructureBuildFlags operator|(EAccelerationStructureBuildFlags a, EAccelerationStructureBuildFlags b) {
    return static_cast<EAccelerationStructureBuildFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline EAccelerationStructureBuildFlags operator&(EAccelerationStructureBuildFlags a, EAccelerationStructureBuildFlags b) {
    return static_cast<EAccelerationStructureBuildFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

// ============================================
// Geometry Descriptors (for BLAS)
// ============================================

// Triangle geometry input
struct TriangleGeometryDesc {
    // Vertex buffer
    IBuffer* vertexBuffer = nullptr;
    uint64_t vertexBufferOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t vertexStride = 0;          // Stride in bytes (typically sizeof(float) * 3)
    ETextureFormat vertexFormat = ETextureFormat::R32G32B32_FLOAT;

    // Index buffer (optional - nullptr for non-indexed)
    IBuffer* indexBuffer = nullptr;
    uint64_t indexBufferOffset = 0;
    uint32_t indexCount = 0;
    EIndexFormat indexFormat = EIndexFormat::UInt32;

    // Optional transform (3x4 row-major matrix)
    // If provided, baked into BLAS
    IBuffer* transformBuffer = nullptr;
    uint64_t transformBufferOffset = 0;
};

// Procedural (AABB) geometry input
struct ProceduralGeometryDesc {
    IBuffer* aabbBuffer = nullptr;      // Buffer of D3D12_RAYTRACING_AABB structs
    uint64_t aabbBufferOffset = 0;
    uint32_t aabbCount = 0;
    uint32_t aabbStride = 0;            // Typically sizeof(float) * 6
};

// Combined geometry descriptor
struct GeometryDesc {
    EGeometryType type = EGeometryType::Triangles;
    EGeometryFlags flags = EGeometryFlags::Opaque;

    union {
        TriangleGeometryDesc triangles;
        ProceduralGeometryDesc procedural;
    };

    GeometryDesc() : triangles{} {}
};

// ============================================
// Instance Descriptor (for TLAS)
// ============================================

struct AccelerationStructureInstance {
    // 3x4 row-major transform matrix (world transform)
    float transform[3][4] = {
        {1, 0, 0, 0},
        {0, 1, 0, 0},
        {0, 0, 1, 0}
    };

    // User-defined instance ID (accessible via InstanceID() in shader)
    uint32_t instanceID = 0;

    // Visibility mask (ANDed with ray mask in TraceRay)
    uint8_t instanceMask = 0xFF;

    // Shader table offset
    uint32_t instanceContributionToHitGroupIndex = 0;

    // Instance flags (cull mode, force opaque, etc.)
    uint8_t flags = 0;

    // Reference to BLAS
    class IAccelerationStructure* blas = nullptr;
};

// ============================================
// Build Info
// ============================================

struct AccelerationStructurePrebuildInfo {
    uint64_t resultDataMaxSizeInBytes = 0;
    uint64_t scratchDataSizeInBytes = 0;
    uint64_t updateScratchDataSizeInBytes = 0;
};

// ============================================
// BLAS Descriptor
// ============================================

struct BLASDesc {
    std::vector<GeometryDesc> geometries;
    EAccelerationStructureBuildFlags buildFlags = EAccelerationStructureBuildFlags::PreferFastTrace;
};

// ============================================
// TLAS Descriptor
// ============================================

struct TLASDesc {
    std::vector<AccelerationStructureInstance> instances;
    EAccelerationStructureBuildFlags buildFlags = EAccelerationStructureBuildFlags::PreferFastTrace;
};

// ============================================
// Acceleration Structure Interface
// ============================================

class IAccelerationStructure {
public:
    virtual ~IAccelerationStructure() = default;

    // Get the type (BLAS or TLAS)
    virtual EAccelerationStructureType GetType() const = 0;

    // Get GPU virtual address (for shader binding)
    virtual uint64_t GetGPUVirtualAddress() const = 0;

    // Get native handle (ID3D12Resource* for result buffer)
    virtual void* GetNativeHandle() = 0;

    // Get size information
    virtual uint64_t GetResultSize() const = 0;
    virtual uint64_t GetScratchSize() const = 0;
};

// ============================================
// Ray Tracing Pipeline Types
// ============================================

enum class ERayTracingShaderType {
    RayGeneration,
    Miss,
    ClosestHit,
    AnyHit,
    Intersection
};

// Shader entry point description
struct RayTracingShaderDesc {
    ERayTracingShaderType type;
    IShader* shader = nullptr;
    const char* entryPoint = nullptr;   // Export name (e.g., "RayGen", "ClosestHit")
};

// Hit group combines closest-hit, any-hit, and intersection shaders
struct HitGroupDesc {
    const char* hitGroupName = nullptr;         // Export name for hit group
    const char* closestHitEntryPoint = nullptr; // Optional
    const char* anyHitEntryPoint = nullptr;     // Optional
    const char* intersectionEntryPoint = nullptr; // Optional (only for procedural)
};

// Ray tracing pipeline descriptor
struct RayTracingPipelineDesc {
    // Shader library (DXIL library containing all shaders)
    IShader* shaderLibrary = nullptr;

    // Ray generation shaders (at least one required)
    std::vector<const char*> rayGenEntryPoints;

    // Miss shaders
    std::vector<const char*> missEntryPoints;

    // Hit groups
    std::vector<HitGroupDesc> hitGroups;

    // Pipeline configuration
    uint32_t maxPayloadSizeInBytes = 32;        // Size of ray payload struct
    uint32_t maxAttributeSizeInBytes = 8;       // Size of hit attributes (barycentrics = 8)
    uint32_t maxTraceRecursionDepth = 1;        // Max recursive TraceRay calls
};

// ============================================
// Ray Tracing Pipeline State Interface
// ============================================

class IRayTracingPipelineState {
public:
    virtual ~IRayTracingPipelineState() = default;

    // Get shader identifier (32 bytes) for SBT
    // exportName: The name used in shader export (e.g., "RayGen", "Miss", "HitGroup")
    virtual const void* GetShaderIdentifier(const char* exportName) const = 0;

    // Get shader identifier size (always D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES = 32)
    virtual uint32_t GetShaderIdentifierSize() const = 0;

    // Get native handle (ID3D12StateObject*)
    virtual void* GetNativeHandle() = 0;
};

// ============================================
// Shader Binding Table (SBT)
// ============================================

// SBT record: shader identifier + local root arguments
struct ShaderRecord {
    const void* shaderIdentifier = nullptr;     // 32 bytes from pipeline
    const void* localRootArguments = nullptr;   // Optional local data
    uint32_t localRootArgumentsSize = 0;
};

// SBT configuration
struct ShaderBindingTableDesc {
    IRayTracingPipelineState* pipeline = nullptr;

    // Ray generation records (typically 1)
    std::vector<ShaderRecord> rayGenRecords;

    // Miss shader records
    std::vector<ShaderRecord> missRecords;

    // Hit group records
    std::vector<ShaderRecord> hitGroupRecords;
};

// ============================================
// Shader Binding Table Interface
// ============================================

class IShaderBindingTable {
public:
    virtual ~IShaderBindingTable() = default;

    // Get GPU addresses for DispatchRays
    virtual uint64_t GetRayGenShaderRecordAddress() const = 0;
    virtual uint64_t GetRayGenShaderRecordSize() const = 0;

    virtual uint64_t GetMissShaderTableAddress() const = 0;
    virtual uint64_t GetMissShaderTableSize() const = 0;
    virtual uint64_t GetMissShaderTableStride() const = 0;

    virtual uint64_t GetHitGroupTableAddress() const = 0;
    virtual uint64_t GetHitGroupTableSize() const = 0;
    virtual uint64_t GetHitGroupTableStride() const = 0;

    // Get native handle (ID3D12Resource* for SBT buffer)
    virtual void* GetNativeHandle() = 0;
};

// ============================================
// Dispatch Rays Descriptor
// ============================================

struct DispatchRaysDesc {
    IShaderBindingTable* shaderBindingTable = nullptr;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
};

} // namespace RHI
