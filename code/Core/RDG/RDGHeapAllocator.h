#pragma once

#include "RDGTypes.h"
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace RDG
{

using Microsoft::WRL::ComPtr;

//=============================================================================
// CRDGHeapAllocator - Manages ID3D12Heap pools for placed resources
//=============================================================================

class CRDGHeapAllocator
{
public:
    enum class EHeapCategory
    {
        RenderTargetDepthStencil,   // D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
        NonRTDSTexture,             // D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES (UAV textures)
        Buffer                      // D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
    };

    struct Allocation
    {
        ID3D12Heap* Heap = nullptr;
        uint64_t Offset = 0;
        uint64_t Size = 0;
        bool IsValid() const { return Heap != nullptr; }
    };

    CRDGHeapAllocator();
    ~CRDGHeapAllocator();

    // Initialize with device
    void Initialize(ID3D12Device* device);

    // Allocate from appropriate heap pool
    Allocation Allocate(EHeapCategory category, uint64_t size, uint64_t alignment);

    // Reset all pools (call at frame end - recycles allocations, doesn't free heaps)
    void Reset();

    // Get statistics
    struct Stats
    {
        uint64_t TotalHeapSize = 0;
        uint64_t UsedSize = 0;
        uint32_t HeapCount = 0;
        uint32_t AllocationCount = 0;
    };
    Stats GetStats() const;

private:
    static constexpr uint64_t DefaultHeapSize = 256 * 1024 * 1024;  // 256 MB
    static constexpr uint64_t DefaultAlignment = 64 * 1024;          // 64 KB
    static constexpr uint64_t MSAAAlignment = 4 * 1024 * 1024;       // 4 MB

    struct HeapPool
    {
        std::vector<ComPtr<ID3D12Heap>> Heaps;
        D3D12_HEAP_FLAGS HeapFlags = D3D12_HEAP_FLAG_NONE;
        uint32_t CurrentHeapIndex = 0;
        uint64_t CurrentOffset = 0;
        uint64_t TotalAllocated = 0;

        void Reset()
        {
            CurrentHeapIndex = 0;
            CurrentOffset = 0;
            TotalAllocated = 0;
        }
    };

    ID3D12Device* m_Device = nullptr;

    HeapPool m_RTDSPool;      // Render Targets + Depth Stencil
    HeapPool m_TexturePool;   // Non-RT/DS textures (UAV capable)
    HeapPool m_BufferPool;    // Buffers

    HeapPool& GetPool(EHeapCategory category);
    bool EnsureHeapCapacity(HeapPool& pool, uint64_t requiredSize);
};

//=============================================================================
// Inline Implementations
//=============================================================================

inline CRDGHeapAllocator::HeapPool& CRDGHeapAllocator::GetPool(EHeapCategory category)
{
    switch (category)
    {
        case EHeapCategory::RenderTargetDepthStencil: return m_RTDSPool;
        case EHeapCategory::NonRTDSTexture:           return m_TexturePool;
        case EHeapCategory::Buffer:                   return m_BufferPool;
        default:                                      return m_BufferPool;
    }
}

} // namespace RDG
