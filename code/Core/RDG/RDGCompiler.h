#pragma once

#include "RDGTypes.h"
#include "RDGBuilder.h"
#include <vector>

namespace RDG
{

//=============================================================================
// CRDGCompiler - Analyzes graph and produces execution plan
//=============================================================================

class CRDGCompiler
{
public:
    //-------------------------------------------------------------------------
    // Compiled Graph Output
    //-------------------------------------------------------------------------

    struct CompiledPass
    {
        uint32_t PassIndex;
        std::vector<D3D12_RESOURCE_BARRIER> BarriersBefore;  // Barriers to execute before pass
    };

    struct CompiledGraph
    {
        std::vector<uint32_t> ExecutionOrder;           // Topologically sorted pass indices
        std::vector<CompiledPass> Passes;               // Per-pass compiled data
        std::vector<RDGResourceLifetime> TextureLifetimes;
        std::vector<RDGResourceLifetime> BufferLifetimes;
        std::vector<RDGAliasingGroup> AliasingGroups;

        // Statistics
        uint64_t TotalTransientMemory = 0;
        uint64_t AliasedMemory = 0;                     // Memory saved by aliasing
        uint32_t CulledPassCount = 0;
        uint32_t CulledResourceCount = 0;
    };

    //-------------------------------------------------------------------------
    // Compilation
    //-------------------------------------------------------------------------

    // Compile the graph (main entry point)
    CompiledGraph Compile(CRDGBuilder& builder);

private:
    //-------------------------------------------------------------------------
    // Internal Methods
    //-------------------------------------------------------------------------

    // Step 1: Build adjacency list from pass dependencies
    void BuildDependencyGraph(const CRDGBuilder& builder);

    // Step 2: Topological sort using Kahn's algorithm
    bool TopologicalSort(std::vector<uint32_t>& outOrder);

    // Step 3: Cull unused passes and resources
    void CullUnused(CRDGBuilder& builder, const std::vector<uint32_t>& order);

    // Step 4: Compute resource lifetimes
    void ComputeLifetimes(
        const CRDGBuilder& builder,
        const std::vector<uint32_t>& order,
        std::vector<RDGResourceLifetime>& textureLifetimes,
        std::vector<RDGResourceLifetime>& bufferLifetimes);

    // Step 5: Compute memory aliasing
    void ComputeAliasing(
        ID3D12Device* device,
        const CRDGBuilder& builder,
        const std::vector<RDGResourceLifetime>& textureLifetimes,
        std::vector<RDGAliasingGroup>& outGroups,
        uint64_t& outTotalMemory,
        uint64_t& outAliasedMemory);

    // Step 6: Plan barrier insertions
    void PlanBarriers(
        const CRDGBuilder& builder,
        const std::vector<uint32_t>& order,
        std::vector<CompiledPass>& outPasses);

    //-------------------------------------------------------------------------
    // Internal State
    //-------------------------------------------------------------------------

    // Dependency graph (adjacency list)
    std::vector<std::vector<uint32_t>> m_Adjacency;     // passIndex -> list of dependent pass indices
    std::vector<uint32_t> m_InDegree;                   // In-degree for topological sort

    uint32_t m_PassCount = 0;
};

//=============================================================================
// Memory Aliasing Utilities
//=============================================================================

namespace MemoryAliasing
{
    // Check if two lifetime intervals overlap
    inline bool IntervalsOverlap(
        uint32_t firstA, uint32_t lastA,
        uint32_t firstB, uint32_t lastB)
    {
        return !(lastA < firstB || lastB < firstA);
    }

    // First-Fit Decreasing bin packing
    // Returns heap offsets for each resource
    std::vector<uint64_t> FirstFitDecreasing(
        const std::vector<RDGResourceLifetime>& lifetimes,
        uint64_t alignment);

    // Get required alignment for a resource
    uint64_t GetRequiredAlignment(const D3D12_RESOURCE_DESC& desc);

    // Get allocation size with alignment
    uint64_t AlignUp(uint64_t value, uint64_t alignment);
}

//=============================================================================
// Inline Implementations
//=============================================================================

inline uint64_t MemoryAliasing::AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

inline uint64_t MemoryAliasing::GetRequiredAlignment(const D3D12_RESOURCE_DESC& desc)
{
    // MSAA textures require 4MB alignment
    if (desc.SampleDesc.Count > 1)
    {
        return 4 * 1024 * 1024;  // 4 MB
    }

    // Default alignment
    return 64 * 1024;  // 64 KB
}

} // namespace RDG
