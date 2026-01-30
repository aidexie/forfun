#pragma once

// ============================================
// DX12 Memory Allocator
// ============================================
// Wraps AMD D3D12 Memory Allocator (D3D12MA) for efficient GPU memory management.
// Provides suballocation from shared heaps instead of per-resource committed resources.

#include "DX12Common.h"
#include "D3D12MemAlloc.h"
#include <deque>
#include <mutex>

namespace RHI {
namespace DX12 {

// ============================================
// Memory Allocation Result
// ============================================
// Returned by CreateBuffer/CreateTexture. Holds both the D3D12MA allocation
// and convenience pointers to the underlying resource.

struct SMemoryAllocation {
    D3D12MA::Allocation* allocation = nullptr;  // Owns the memory (call Release() to free)
    ID3D12Resource* resource = nullptr;         // Non-owning pointer to the resource
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;   // For buffers only

    bool IsValid() const { return allocation != nullptr; }
};

// ============================================
// CDX12MemoryAllocator (Singleton)
// ============================================
// Manages GPU memory allocation via D3D12MA.
// Thread-safe for allocation/deallocation.

class CDX12MemoryAllocator {
public:
    // Singleton access
    static CDX12MemoryAllocator& Instance();

    // Lifecycle
    bool Initialize(ID3D12Device* device, IDXGIAdapter* adapter);
    void Shutdown();

    // Buffer allocation
    // heapType: D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, or D3D12_HEAP_TYPE_READBACK
    SMemoryAllocation CreateBuffer(const D3D12_RESOURCE_DESC& desc,
                                   D3D12_HEAP_TYPE heapType,
                                   D3D12_RESOURCE_STATES initialState);

    // Texture allocation
    // clearValue: Optional optimized clear value for render targets/depth stencils
    SMemoryAllocation CreateTexture(const D3D12_RESOURCE_DESC& desc,
                                    D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES initialState,
                                    const D3D12_CLEAR_VALUE* clearValue = nullptr);

    // Deferred deallocation
    // Resources are queued for deletion until GPU finishes using them
    void FreeAllocation(D3D12MA::Allocation* allocation, uint64_t fenceValue);

    // Process completed deallocations - call at frame start
    void ProcessDeferredFrees(uint64_t completedFenceValue);

    // Memory statistics
    void GetBudget(D3D12MA::Budget* localBudget, D3D12MA::Budget* nonLocalBudget);
    void LogStatistics();

    // Check if initialized
    bool IsInitialized() const { return m_allocator != nullptr; }

private:
    CDX12MemoryAllocator() = default;
    ~CDX12MemoryAllocator();

    // Non-copyable
    CDX12MemoryAllocator(const CDX12MemoryAllocator&) = delete;
    CDX12MemoryAllocator& operator=(const CDX12MemoryAllocator&) = delete;

private:
    D3D12MA::Allocator* m_allocator = nullptr;

    // Deferred deletion queue
    struct SPendingFree {
        D3D12MA::Allocation* allocation;
        uint64_t fenceValue;
    };
    std::deque<SPendingFree> m_pendingFrees;
    std::mutex m_mutex;  // Thread safety for deferred frees
};

} // namespace DX12
} // namespace RHI
