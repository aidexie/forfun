#pragma once

#include "DX12Common.h"
#include <vector>
#include <algorithm>
#include <mutex>

// ============================================
// DX12 Descriptor Heap Management
// ============================================
// Manages descriptor heaps for DX12 resources
// Uses a free list allocator for efficient allocation/deallocation

namespace RHI {
namespace DX12 {

// ============================================
// Descriptor Handle
// ============================================
// Represents an allocated descriptor with both CPU and GPU handles

struct SDescriptorHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};  // Only valid for shader-visible heaps
    uint32_t index = UINT32_MAX;                  // Index in the heap

    bool IsValid() const { return index != UINT32_MAX; }
    void Invalidate() {
        cpuHandle = {};
        gpuHandle = {};
        index = UINT32_MAX;
    }
};

// ============================================
// Descriptor Heap
// ============================================
// Single descriptor heap with free list allocation

class CDX12DescriptorHeap {
public:
    CDX12DescriptorHeap() = default;
    ~CDX12DescriptorHeap();

    // Non-copyable
    CDX12DescriptorHeap(const CDX12DescriptorHeap&) = delete;
    CDX12DescriptorHeap& operator=(const CDX12DescriptorHeap&) = delete;

    // Initialize the heap
    bool Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t numDescriptors,
        bool shaderVisible,
        const char* debugName = nullptr
    );

    // Shutdown and release resources
    void Shutdown();

    // Allocate a single descriptor
    SDescriptorHandle Allocate();

    // Allocate multiple contiguous descriptors
    // Returns handle to first descriptor, or invalid handle if not enough space
    SDescriptorHandle AllocateRange(uint32_t count);

    // Free a previously allocated descriptor
    void Free(const SDescriptorHandle& handle);

    // Free a range of descriptors (must have been allocated with AllocateRange)
    void FreeRange(const SDescriptorHandle& handle, uint32_t count);

    // Get handle at specific index (for direct access, no allocation tracking)
    SDescriptorHandle GetHandle(uint32_t index) const;

    // Accessors
    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_type; }
    uint32_t GetDescriptorSize() const { return m_descriptorSize; }
    uint32_t GetCapacity() const { return m_capacity; }
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }
    uint32_t GetFreeCount() const { return m_capacity - m_allocatedCount; }
    bool IsShaderVisible() const { return m_shaderVisible; }

    // Get CPU handle for heap start
    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUStart() const {
        return m_cpuStart;
    }

    // Get GPU handle for heap start (only valid for shader-visible heaps)
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUStart() const {
        return m_gpuStart;
    }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};

    uint32_t m_descriptorSize = 0;
    uint32_t m_capacity = 0;
    uint32_t m_allocatedCount = 0;
    bool m_shaderVisible = false;

    // Free list - stores indices of free descriptors
    std::vector<uint32_t> m_freeList;

    // For thread safety (optional, currently single-threaded)
    // std::mutex m_mutex;
};

// ============================================
// Descriptor Heap Manager
// ============================================
// Manages all descriptor heaps for the application

class CDX12DescriptorHeapManager {
public:
    // Singleton access
    static CDX12DescriptorHeapManager& Instance();

    // Initialize all heaps
    bool Initialize(ID3D12Device* device);

    // Shutdown all heaps
    void Shutdown();

    // Get heaps by type
    CDX12DescriptorHeap& GetCBVSRVUAVHeap() { return m_cbvSrvUavHeap; }
    CDX12DescriptorHeap& GetSamplerHeap() { return m_samplerHeap; }
    CDX12DescriptorHeap& GetRTVHeap() { return m_rtvHeap; }
    CDX12DescriptorHeap& GetDSVHeap() { return m_dsvHeap; }

    // Const versions
    const CDX12DescriptorHeap& GetCBVSRVUAVHeap() const { return m_cbvSrvUavHeap; }
    const CDX12DescriptorHeap& GetSamplerHeap() const { return m_samplerHeap; }
    const CDX12DescriptorHeap& GetRTVHeap() const { return m_rtvHeap; }
    const CDX12DescriptorHeap& GetDSVHeap() const { return m_dsvHeap; }

    // Convenience allocators
    SDescriptorHandle AllocateCBVSRVUAV() { return m_cbvSrvUavHeap.Allocate(); }
    SDescriptorHandle AllocateSampler() { return m_samplerHeap.Allocate(); }
    SDescriptorHandle AllocateRTV() { return m_rtvHeap.Allocate(); }
    SDescriptorHandle AllocateDSV() { return m_dsvHeap.Allocate(); }

    // Convenience freers
    void FreeCBVSRVUAV(const SDescriptorHandle& handle) { m_cbvSrvUavHeap.Free(handle); }
    void FreeSampler(const SDescriptorHandle& handle) { m_samplerHeap.Free(handle); }
    void FreeRTV(const SDescriptorHandle& handle) { m_rtvHeap.Free(handle); }
    void FreeDSV(const SDescriptorHandle& handle) { m_dsvHeap.Free(handle); }

private:
    CDX12DescriptorHeapManager() = default;
    ~CDX12DescriptorHeapManager() = default;

    // Non-copyable
    CDX12DescriptorHeapManager(const CDX12DescriptorHeapManager&) = delete;
    CDX12DescriptorHeapManager& operator=(const CDX12DescriptorHeapManager&) = delete;

private:
    // GPU-visible heaps (for shader binding)
    CDX12DescriptorHeap m_cbvSrvUavHeap;  // CBV, SRV, UAV descriptors
    CDX12DescriptorHeap m_samplerHeap;     // Sampler descriptors

    // CPU-only heaps (for render target and depth stencil)
    CDX12DescriptorHeap m_rtvHeap;         // Render target views
    CDX12DescriptorHeap m_dsvHeap;         // Depth stencil views

    bool m_initialized = false;
};

} // namespace DX12
} // namespace RHI
