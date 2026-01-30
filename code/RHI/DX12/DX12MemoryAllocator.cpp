// ============================================
// DX12 Memory Allocator Implementation
// ============================================

// D3D12MA configuration - must be before including the header
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED

#ifdef _DEBUG
#define D3D12MA_DEBUG_MARGIN 16
#define D3D12MA_DEBUG_GLOBAL_MUTEX 1
#endif

#include "DX12MemoryAllocator.h"
#include "Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// Singleton
// ============================================

CDX12MemoryAllocator& CDX12MemoryAllocator::Instance() {
    static CDX12MemoryAllocator instance;
    return instance;
}

CDX12MemoryAllocator::~CDX12MemoryAllocator() {
    // Ensure Shutdown() was called
    if (m_allocator) {
        CFFLog::Warning("[D3D12MA] Destructor called without Shutdown()");
        Shutdown();
    }
}

// ============================================
// Lifecycle
// ============================================

bool CDX12MemoryAllocator::Initialize(ID3D12Device* device, IDXGIAdapter* adapter) {
    if (m_allocator) {
        CFFLog::Warning("[D3D12MA] Already initialized");
        return true;
    }

    D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
    allocatorDesc.pDevice = device;
    allocatorDesc.pAdapter = adapter;
    allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;

    // Enable detailed budget tracking in debug builds
#ifdef _DEBUG
    allocatorDesc.Flags |= D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;
#endif

    HRESULT hr = D3D12MA::CreateAllocator(&allocatorDesc, &m_allocator);
    if (FAILED(hr)) {
        CFFLog::Error("[D3D12MA] Failed to create allocator: 0x%08X", hr);
        return false;
    }

    CFFLog::Info("[D3D12MA] Memory allocator initialized");
    LogStatistics();
    return true;
}

void CDX12MemoryAllocator::Shutdown() {
    if (!m_allocator) {
        return;
    }

    // Process all pending frees (force release)
    ProcessDeferredFrees(UINT64_MAX);

    // Log allocation statistics
    uint64_t totalAllocated = m_totalBufferAllocations + m_totalTextureAllocations;
    CFFLog::Info("[D3D12MA] Allocation stats: Buffers=%llu, Textures=%llu, Total=%llu, Released=%llu",
                 m_totalBufferAllocations, m_totalTextureAllocations, totalAllocated, m_totalReleased);

    // Report leaked allocations
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_liveAllocations.empty()) {
            CFFLog::Warning("[D3D12MA] LEAK DETECTED: %zu allocations not released!", m_liveAllocations.size());

            // Count by type
            int bufferLeaks = 0, textureLeaks = 0;
            for (const auto& [alloc, info] : m_liveAllocations) {
                if (info.type == "Buffer") bufferLeaks++;
                else textureLeaks++;
            }
            CFFLog::Warning("[D3D12MA] Leaked: %d Buffers, %d Textures", bufferLeaks, textureLeaks);

            // Print all leaked allocations
            int count = 0;
            for (const auto& [alloc, info] : m_liveAllocations) {
                CFFLog::Warning("[D3D12MA]   Leak #%d: %s, size=%llu, name=%s",
                               count + 1, info.type.c_str(), info.size, info.name.c_str());
                count++;
            }
        }
    }

    // Log final statistics
    LogStatistics();

    // Release allocator
    m_allocator->Release();
    m_allocator = nullptr;

    CFFLog::Info("[D3D12MA] Memory allocator shutdown");
}

// ============================================
// Buffer Allocation
// ============================================

SMemoryAllocation CDX12MemoryAllocator::CreateBuffer(const D3D12_RESOURCE_DESC& desc,
                                                      D3D12_HEAP_TYPE heapType,
                                                      D3D12_RESOURCE_STATES initialState,
                                                      const char* debugName) {
    SMemoryAllocation result = {};

    if (!m_allocator) {
        CFFLog::Error("[D3D12MA] Allocator not initialized");
        return result;
    }

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = heapType;

    HRESULT hr = m_allocator->CreateResource(
        &allocDesc,
        &desc,
        initialState,
        nullptr,  // No clear value for buffers
        &result.allocation,
        IID_NULL, nullptr  // Don't request resource - avoids extra refcount
    );

    if (FAILED(hr) || !result.allocation) {
        CFFLog::Error("[D3D12MA] Failed to create buffer: 0x%08X (size=%llu)", hr, desc.Width);
        return {};
    }

    // Get resource from allocation (refcount = 1, held by D3D12MA)
    result.resource = result.allocation->GetResource();

    // Get GPU virtual address for buffers
    result.gpuAddress = result.resource->GetGPUVirtualAddress();

    // Track allocation
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_liveAllocations[result.allocation] = { "Buffer", debugName ? debugName : "<unnamed>", desc.Width };
    }
    m_totalBufferAllocations++;
    return result;
}

// ============================================
// Texture Allocation
// ============================================

SMemoryAllocation CDX12MemoryAllocator::CreateTexture(const D3D12_RESOURCE_DESC& desc,
                                                       D3D12_HEAP_TYPE heapType,
                                                       D3D12_RESOURCE_STATES initialState,
                                                       const D3D12_CLEAR_VALUE* clearValue,
                                                       const char* debugName) {
    SMemoryAllocation result = {};

    if (!m_allocator) {
        CFFLog::Error("[D3D12MA] Allocator not initialized");
        return result;
    }

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = heapType;

    HRESULT hr = m_allocator->CreateResource(
        &allocDesc,
        &desc,
        initialState,
        clearValue,
        &result.allocation,
        IID_NULL, nullptr  // Don't request resource - avoids extra refcount
    );

    if (FAILED(hr) || !result.allocation) {
        CFFLog::Error("[D3D12MA] Failed to create texture: 0x%08X (%ux%u)",
                      hr, (uint32_t)desc.Width, desc.Height);
        return {};
    }

    // Get resource from allocation (refcount = 1, held by D3D12MA)
    result.resource = result.allocation->GetResource();

    // Textures don't have GPU virtual address (use SRV/UAV instead)
    result.gpuAddress = 0;

    // Track allocation
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_liveAllocations[result.allocation] = { "Texture", debugName ? debugName : "<unnamed>", desc.Width * desc.Height };
    }
    m_totalTextureAllocations++;
    return result;
}

// ============================================
// Deferred Deallocation
// ============================================

void CDX12MemoryAllocator::FreeAllocation(D3D12MA::Allocation* allocation, uint64_t fenceValue) {
    if (!allocation) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_liveAllocations.erase(allocation);
    m_pendingFrees.push_back({ allocation, fenceValue });
}

void CDX12MemoryAllocator::ProcessDeferredFrees(uint64_t completedFenceValue) {
    std::lock_guard<std::mutex> lock(m_mutex);

    while (!m_pendingFrees.empty()) {
        const auto& pending = m_pendingFrees.front();
        if (pending.fenceValue > completedFenceValue) {
            break;  // Not yet safe to free
        }

        // Safe to release
        pending.allocation->Release();
        m_pendingFrees.pop_front();
        m_totalReleased++;
    }
}

// ============================================
// Statistics
// ============================================

void CDX12MemoryAllocator::GetBudget(D3D12MA::Budget* localBudget, D3D12MA::Budget* nonLocalBudget) {
    if (m_allocator) {
        m_allocator->GetBudget(localBudget, nonLocalBudget);
    }
}

void CDX12MemoryAllocator::LogStatistics() {
    if (!m_allocator) {
        return;
    }

    D3D12MA::Budget localBudget = {};
    D3D12MA::Budget nonLocalBudget = {};
    m_allocator->GetBudget(&localBudget, &nonLocalBudget);

    // Local (VRAM) budget
    CFFLog::Info("[D3D12MA] VRAM: Used=%llu MB / Budget=%llu MB (Blocks=%llu MB, Allocs=%u)",
                 localBudget.Stats.AllocationBytes / (1024 * 1024),
                 localBudget.BudgetBytes / (1024 * 1024),
                 localBudget.Stats.BlockBytes / (1024 * 1024),
                 localBudget.Stats.AllocationCount);

    // Non-local (system RAM) budget - only log if used
    if (nonLocalBudget.Stats.AllocationCount > 0) {
        CFFLog::Info("[D3D12MA] System RAM: Used=%llu MB (Allocs=%u)",
                     nonLocalBudget.Stats.AllocationBytes / (1024 * 1024),
                     nonLocalBudget.Stats.AllocationCount);
    }

    // Pending frees
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pendingFrees.empty()) {
        CFFLog::Info("[D3D12MA] Pending frees: %zu", m_pendingFrees.size());
    }
}

} // namespace DX12
} // namespace RHI
