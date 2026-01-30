# DX12 Memory Management Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Refactor DX12 memory management to use industry-standard patterns: buddy allocator for descriptors, pooled placed resources, proper metrics, and defragmentation support.

**Architecture:** Replace current free-list descriptor allocator with O(log n) buddy allocator. Integrate AMD D3D12MemoryAllocator (D3D12MA) for resource memory management. Add comprehensive metrics/telemetry. Fix critical issues (DSV heap size, silent failures).

**Tech Stack:** D3D12, D3D12MemoryAllocator (D3D12MA), C++17

---

## Phase 1: Critical Stability Fixes

### Task 1.1: Increase DSV Heap Size

**Files:**
- Modify: `RHI/DX12/DX12DescriptorHeap.cpp:Initialize()`

**Step 1: Locate the DSV heap initialization**

Find the constant defining DSV heap size (currently 32).

**Step 2: Increase DSV heap size to 256**

```cpp
// In CDX12DescriptorHeapManager::Initialize()
// Change from:
constexpr uint32_t DSV_HEAP_SIZE = 32;
// To:
constexpr uint32_t DSV_HEAP_SIZE = 256;
```

**Step 3: Run existing tests to verify no regression**

Run: `E:/forfun/source/code/build/Debug/forfun.exe --test TestDeferredLighting`
Expected: PASS, check `E:/forfun/debug/TestDeferredLighting/runtime.log`

**Step 4: Commit**

```bash
git add RHI/DX12/DX12DescriptorHeap.cpp
git commit -m "fix(DX12): Increase DSV heap size from 32 to 256"
```

---

### Task 1.2: Add Allocation Failure Callback System

**Files:**
- Create: `RHI/DX12/DX12MemoryCallbacks.h`
- Modify: `RHI/DX12/DX12DescriptorHeap.h`
- Modify: `RHI/DX12/DX12DescriptorHeap.cpp`

**Step 1: Create callback interface**

```cpp
// RHI/DX12/DX12MemoryCallbacks.h
#pragma once

#include <functional>
#include <string>

namespace RHI {
namespace DX12 {

enum class EAllocationFailureType {
    DescriptorHeapFull,
    UploadBufferFull,
    DynamicBufferFull,
    BindGroupAllocationFailed
};

struct SAllocationFailureInfo {
    EAllocationFailureType type;
    const char* heapName;
    uint32_t requestedCount;
    uint32_t availableCount;
    uint32_t totalCapacity;
};

// Global callback for allocation failures
// Set this to handle failures (e.g., trigger defragmentation, resize, or assert)
using AllocationFailureCallback = std::function<void(const SAllocationFailureInfo&)>;

class CMemoryCallbacks {
public:
    static CMemoryCallbacks& Instance();

    void SetAllocationFailureCallback(AllocationFailureCallback callback);
    void OnAllocationFailure(const SAllocationFailureInfo& info);

private:
    AllocationFailureCallback m_callback;
};

} // namespace DX12
} // namespace RHI
```

**Step 2: Implement callback system**

```cpp
// RHI/DX12/DX12MemoryCallbacks.cpp
#include "DX12MemoryCallbacks.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

CMemoryCallbacks& CMemoryCallbacks::Instance() {
    static CMemoryCallbacks instance;
    return instance;
}

void CMemoryCallbacks::SetAllocationFailureCallback(AllocationFailureCallback callback) {
    m_callback = std::move(callback);
}

void CMemoryCallbacks::OnAllocationFailure(const SAllocationFailureInfo& info) {
    // Always log
    CFFLog::Error("[DX12Memory] Allocation failed: type=%d heap=%s requested=%u available=%u capacity=%u",
        static_cast<int>(info.type), info.heapName, info.requestedCount, info.availableCount, info.totalCapacity);

    // Call user callback if set
    if (m_callback) {
        m_callback(info);
    }
}

} // namespace DX12
} // namespace RHI
```

**Step 3: Integrate into descriptor heap allocation**

```cpp
// In CDX12DescriptorHeap::Allocate() - after allocation fails:
if (m_freeList.empty()) {
    SAllocationFailureInfo info = {};
    info.type = EAllocationFailureType::DescriptorHeapFull;
    info.heapName = m_debugName.c_str();
    info.requestedCount = 1;
    info.availableCount = 0;
    info.totalCapacity = m_capacity;
    CMemoryCallbacks::Instance().OnAllocationFailure(info);
    return SDescriptorHandle{};
}
```

**Step 4: Add to CMakeLists.txt**

```cmake
# Add new source file
set(RHI_DX12_SOURCES
    # ... existing files ...
    RHI/DX12/DX12MemoryCallbacks.cpp
)
```

**Step 5: Build and verify**

Run: `cmake --build build --target forfun`
Expected: Build succeeds

**Step 6: Commit**

```bash
git add RHI/DX12/DX12MemoryCallbacks.h RHI/DX12/DX12MemoryCallbacks.cpp
git add RHI/DX12/DX12DescriptorHeap.cpp CMakeLists.txt
git commit -m "feat(DX12): Add allocation failure callback system"
```

---

### Task 1.3: Add Double-Free Detection in Release Builds

**Files:**
- Modify: `RHI/DX12/DX12DescriptorHeap.cpp`

**Step 1: Add tracking set for allocated descriptors**

```cpp
// In CDX12DescriptorHeap class (header):
#include <unordered_set>

// Add member:
std::unordered_set<uint32_t> m_allocatedIndices;  // Track what's allocated
```

**Step 2: Update Allocate() to track**

```cpp
SDescriptorHandle CDX12DescriptorHeap::Allocate() {
    if (m_freeList.empty()) {
        // ... failure handling ...
        return SDescriptorHandle{};
    }

    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    m_allocatedIndices.insert(index);  // Track allocation
    m_allocatedCount++;

    return GetHandle(index);
}
```

**Step 3: Update Free() to validate**

```cpp
void CDX12DescriptorHeap::Free(const SDescriptorHandle& handle) {
    if (!handle.IsValid()) return;

    // Check for double-free (works in Release builds too)
    if (m_allocatedIndices.find(handle.index) == m_allocatedIndices.end()) {
        CFFLog::Error("[DX12DescriptorHeap] Double free or invalid free detected! index=%u heap=%s",
            handle.index, m_debugName.c_str());
        return;  // Don't corrupt the free list
    }

    m_allocatedIndices.erase(handle.index);
    m_freeList.push_back(handle.index);
    m_allocatedCount--;
}
```

**Step 4: Build and test**

Run: `cmake --build build --target forfun`
Expected: Build succeeds

**Step 5: Commit**

```bash
git add RHI/DX12/DX12DescriptorHeap.h RHI/DX12/DX12DescriptorHeap.cpp
git commit -m "fix(DX12): Add double-free detection in Release builds"
```

---

## Phase 2: Buddy Allocator for Descriptor Heaps

### Task 2.1: Create Buddy Allocator Class

**Files:**
- Create: `RHI/DX12/DX12BuddyAllocator.h`
- Create: `RHI/DX12/DX12BuddyAllocator.cpp`

**Step 1: Write the failing test**

```cpp
// Tests/TestBuddyAllocator.cpp
#include "Core/Testing/TestFramework.h"
#include "RHI/DX12/DX12BuddyAllocator.h"

REGISTER_TEST(TestBuddyAllocator)

void TestBuddyAllocator::Run() {
    using namespace RHI::DX12;

    // Test 1: Basic allocation
    CBuddyAllocator allocator;
    allocator.Initialize(1024);  // 1024 slots

    uint32_t block1 = allocator.Allocate(16);
    FF_ASSERT(block1 != CBuddyAllocator::INVALID_BLOCK, "Allocation should succeed");
    FF_ASSERT(block1 == 0, "First allocation should be at offset 0");

    // Test 2: Sequential allocations
    uint32_t block2 = allocator.Allocate(16);
    FF_ASSERT(block2 == 16, "Second allocation should be at offset 16");

    // Test 3: Free and reuse
    allocator.Free(block1, 16);
    uint32_t block3 = allocator.Allocate(16);
    FF_ASSERT(block3 == 0, "Reused block should be at offset 0");

    // Test 4: Coalescing
    allocator.Free(block2, 16);
    allocator.Free(block3, 16);
    uint32_t block4 = allocator.Allocate(32);  // Should coalesce
    FF_ASSERT(block4 == 0, "Coalesced block should be at offset 0");

    CFFLog::Info("[TestBuddyAllocator] All tests passed");
}
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build && E:/forfun/source/code/build/Debug/forfun.exe --test TestBuddyAllocator`
Expected: FAIL - CBuddyAllocator not defined

**Step 3: Implement buddy allocator header**

```cpp
// RHI/DX12/DX12BuddyAllocator.h
#pragma once

#include <cstdint>
#include <vector>
#include <array>

namespace RHI {
namespace DX12 {

// Power-of-2 buddy allocator for descriptor heaps
// Provides O(log n) allocation and deallocation with automatic coalescing
class CBuddyAllocator {
public:
    static constexpr uint32_t INVALID_BLOCK = UINT32_MAX;
    static constexpr uint32_t MAX_ORDER = 16;  // Up to 2^16 = 65536 slots
    static constexpr uint32_t MIN_BLOCK_SIZE = 1;

    CBuddyAllocator() = default;
    ~CBuddyAllocator() = default;

    // Initialize with total capacity (rounded up to power of 2)
    bool Initialize(uint32_t capacity);

    // Shutdown and release tracking structures
    void Shutdown();

    // Allocate a block of given size (rounded up to power of 2)
    // Returns offset or INVALID_BLOCK on failure
    uint32_t Allocate(uint32_t size);

    // Free a previously allocated block
    void Free(uint32_t offset, uint32_t size);

    // Query
    uint32_t GetCapacity() const { return m_capacity; }
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }
    uint32_t GetFreeCount() const { return m_capacity - m_allocatedCount; }
    uint32_t GetLargestFreeBlock() const;
    float GetFragmentation() const;

private:
    uint32_t SizeToOrder(uint32_t size) const;
    uint32_t OrderToSize(uint32_t order) const { return 1u << order; }
    uint32_t GetBuddyOffset(uint32_t offset, uint32_t order) const;
    bool IsBuddyFree(uint32_t buddyOffset, uint32_t order) const;
    void RemoveFromFreeList(uint32_t offset, uint32_t order);

private:
    uint32_t m_capacity = 0;
    uint32_t m_maxOrder = 0;
    uint32_t m_allocatedCount = 0;

    // Free lists per order (order 0 = size 1, order 1 = size 2, etc.)
    std::array<std::vector<uint32_t>, MAX_ORDER> m_freeLists;

    // Track allocated blocks for validation and coalescing
    // Key: offset, Value: order (size = 2^order)
    std::vector<int8_t> m_blockOrder;  // -1 = free, 0+ = allocated order
};

} // namespace DX12
} // namespace RHI
```

**Step 4: Implement buddy allocator**

```cpp
// RHI/DX12/DX12BuddyAllocator.cpp
#include "DX12BuddyAllocator.h"
#include "../../Core/FFLog.h"
#include <algorithm>
#include <bit>

namespace RHI {
namespace DX12 {

bool CBuddyAllocator::Initialize(uint32_t capacity) {
    // Round up to power of 2
    m_capacity = std::bit_ceil(capacity);
    m_maxOrder = SizeToOrder(m_capacity);
    m_allocatedCount = 0;

    // Clear free lists
    for (auto& list : m_freeLists) {
        list.clear();
    }

    // Initialize block tracking (-1 = free)
    m_blockOrder.resize(m_capacity, -1);

    // Add entire capacity as one free block at max order
    m_freeLists[m_maxOrder].push_back(0);

    return true;
}

void CBuddyAllocator::Shutdown() {
    for (auto& list : m_freeLists) {
        list.clear();
    }
    m_blockOrder.clear();
    m_capacity = 0;
    m_allocatedCount = 0;
}

uint32_t CBuddyAllocator::SizeToOrder(uint32_t size) const {
    if (size == 0) return 0;
    // ceil(log2(size))
    return size <= 1 ? 0 : (32 - std::countl_zero(size - 1));
}

uint32_t CBuddyAllocator::GetBuddyOffset(uint32_t offset, uint32_t order) const {
    return offset ^ (1u << order);  // XOR flips the order-th bit
}

bool CBuddyAllocator::IsBuddyFree(uint32_t buddyOffset, uint32_t order) const {
    if (buddyOffset >= m_capacity) return false;

    auto& freeList = m_freeLists[order];
    return std::find(freeList.begin(), freeList.end(), buddyOffset) != freeList.end();
}

void CBuddyAllocator::RemoveFromFreeList(uint32_t offset, uint32_t order) {
    auto& freeList = m_freeLists[order];
    auto it = std::find(freeList.begin(), freeList.end(), offset);
    if (it != freeList.end()) {
        // Swap with last and pop for O(1) removal
        std::swap(*it, freeList.back());
        freeList.pop_back();
    }
}

uint32_t CBuddyAllocator::Allocate(uint32_t size) {
    if (size == 0) size = 1;

    uint32_t requestedOrder = SizeToOrder(size);

    // Find smallest available block >= requested size
    for (uint32_t order = requestedOrder; order <= m_maxOrder; ++order) {
        if (!m_freeLists[order].empty()) {
            // Found a free block
            uint32_t offset = m_freeLists[order].back();
            m_freeLists[order].pop_back();

            // Split larger blocks until we reach requested size
            while (order > requestedOrder) {
                --order;
                uint32_t buddyOffset = offset + (1u << order);
                m_freeLists[order].push_back(buddyOffset);
            }

            // Mark as allocated
            uint32_t blockSize = 1u << requestedOrder;
            for (uint32_t i = 0; i < blockSize; ++i) {
                m_blockOrder[offset + i] = static_cast<int8_t>(requestedOrder);
            }
            m_allocatedCount += blockSize;

            return offset;
        }
    }

    // No suitable block found
    return INVALID_BLOCK;
}

void CBuddyAllocator::Free(uint32_t offset, uint32_t size) {
    if (offset >= m_capacity || size == 0) return;

    uint32_t order = SizeToOrder(size);
    uint32_t blockSize = 1u << order;

    // Validate it was allocated
    if (m_blockOrder[offset] < 0) {
        CFFLog::Error("[BuddyAllocator] Double free at offset %u", offset);
        return;
    }

    // Mark as free
    for (uint32_t i = 0; i < blockSize; ++i) {
        m_blockOrder[offset + i] = -1;
    }
    m_allocatedCount -= blockSize;

    // Coalesce with buddy if possible
    while (order < m_maxOrder) {
        uint32_t buddyOffset = GetBuddyOffset(offset, order);

        if (!IsBuddyFree(buddyOffset, order)) {
            break;  // Buddy is not free, stop coalescing
        }

        // Remove buddy from free list
        RemoveFromFreeList(buddyOffset, order);

        // Merge: use lower offset as new block
        offset = std::min(offset, buddyOffset);
        ++order;
    }

    // Add coalesced block to free list
    m_freeLists[order].push_back(offset);
}

uint32_t CBuddyAllocator::GetLargestFreeBlock() const {
    for (int order = m_maxOrder; order >= 0; --order) {
        if (!m_freeLists[order].empty()) {
            return 1u << order;
        }
    }
    return 0;
}

float CBuddyAllocator::GetFragmentation() const {
    if (m_allocatedCount == 0) return 0.0f;

    uint32_t largestFree = GetLargestFreeBlock();
    uint32_t totalFree = m_capacity - m_allocatedCount;

    if (totalFree == 0) return 0.0f;

    // Fragmentation = 1 - (largest_free / total_free)
    return 1.0f - (static_cast<float>(largestFree) / static_cast<float>(totalFree));
}

} // namespace DX12
} // namespace RHI
```

**Step 5: Add to CMakeLists.txt and build**

```cmake
set(RHI_DX12_SOURCES
    # ... existing files ...
    RHI/DX12/DX12BuddyAllocator.cpp
)
```

**Step 6: Run test to verify it passes**

Run: `cmake --build build && E:/forfun/source/code/build/Debug/forfun.exe --test TestBuddyAllocator`
Expected: PASS

**Step 7: Commit**

```bash
git add RHI/DX12/DX12BuddyAllocator.h RHI/DX12/DX12BuddyAllocator.cpp
git add Tests/TestBuddyAllocator.cpp CMakeLists.txt
git commit -m "feat(DX12): Add buddy allocator for O(log n) descriptor allocation"
```

---

### Task 2.2: Integrate Buddy Allocator into Descriptor Heap

**Files:**
- Modify: `RHI/DX12/DX12DescriptorHeap.h`
- Modify: `RHI/DX12/DX12DescriptorHeap.cpp`

**Step 1: Replace free list with buddy allocator in header**

```cpp
// In DX12DescriptorHeap.h
#include "DX12BuddyAllocator.h"

class CDX12DescriptorHeap {
    // ... existing interface unchanged ...

private:
    // Replace:
    // std::vector<uint32_t> m_freeList;
    // std::unordered_set<uint32_t> m_allocatedIndices;

    // With:
    CBuddyAllocator m_allocator;
};
```

**Step 2: Update Initialize()**

```cpp
bool CDX12DescriptorHeap::Initialize(...) {
    // ... existing heap creation code ...

    // Replace free list initialization with buddy allocator
    if (!m_allocator.Initialize(numDescriptors)) {
        CFFLog::Error("[DX12DescriptorHeap] Failed to initialize buddy allocator");
        return false;
    }

    return true;
}
```

**Step 3: Update Allocate() to use buddy allocator**

```cpp
SDescriptorHandle CDX12DescriptorHeap::Allocate() {
    uint32_t index = m_allocator.Allocate(1);

    if (index == CBuddyAllocator::INVALID_BLOCK) {
        SAllocationFailureInfo info = {};
        info.type = EAllocationFailureType::DescriptorHeapFull;
        info.heapName = m_debugName.c_str();
        info.requestedCount = 1;
        info.availableCount = m_allocator.GetFreeCount();
        info.totalCapacity = m_capacity;
        CMemoryCallbacks::Instance().OnAllocationFailure(info);
        return SDescriptorHandle{};
    }

    m_allocatedCount++;
    return GetHandle(index);
}
```

**Step 4: Update AllocateRange() - now O(log n)!**

```cpp
SDescriptorHandle CDX12DescriptorHeap::AllocateRange(uint32_t count) {
    uint32_t index = m_allocator.Allocate(count);

    if (index == CBuddyAllocator::INVALID_BLOCK) {
        SAllocationFailureInfo info = {};
        info.type = EAllocationFailureType::DescriptorHeapFull;
        info.heapName = m_debugName.c_str();
        info.requestedCount = count;
        info.availableCount = m_allocator.GetLargestFreeBlock();
        info.totalCapacity = m_capacity;
        CMemoryCallbacks::Instance().OnAllocationFailure(info);
        return SDescriptorHandle{};
    }

    m_allocatedCount += count;
    return GetHandle(index);
}
```

**Step 5: Update Free() and FreeRange()**

```cpp
void CDX12DescriptorHeap::Free(const SDescriptorHandle& handle) {
    if (!handle.IsValid()) return;
    m_allocator.Free(handle.index, 1);
    m_allocatedCount--;
}

void CDX12DescriptorHeap::FreeRange(const SDescriptorHandle& handle, uint32_t count) {
    if (!handle.IsValid()) return;
    m_allocator.Free(handle.index, count);
    m_allocatedCount -= count;
}
```

**Step 6: Run all rendering tests**

Run: `E:/forfun/source/code/build/Debug/forfun.exe --test TestDeferredLighting`
Expected: PASS

Run: `E:/forfun/source/code/build/Debug/forfun.exe --test TestBindGroup`
Expected: PASS

**Step 7: Commit**

```bash
git add RHI/DX12/DX12DescriptorHeap.h RHI/DX12/DX12DescriptorHeap.cpp
git commit -m "refactor(DX12): Replace free list with buddy allocator in descriptor heap"
```

---

## Phase 3: Memory Metrics & Telemetry

### Task 3.1: Create Memory Metrics System

**Files:**
- Create: `RHI/DX12/DX12MemoryMetrics.h`
- Create: `RHI/DX12/DX12MemoryMetrics.cpp`

**Step 1: Design metrics structure**

```cpp
// RHI/DX12/DX12MemoryMetrics.h
#pragma once

#include <cstdint>
#include <string>
#include <mutex>

namespace RHI {
namespace DX12 {

struct SHeapMetrics {
    const char* name = "";
    uint32_t capacity = 0;
    uint32_t allocated = 0;
    uint32_t free = 0;
    uint32_t largestFreeBlock = 0;
    float fragmentation = 0.0f;  // 0.0 = no fragmentation, 1.0 = fully fragmented
};

struct SMemoryMetrics {
    // Descriptor heaps
    SHeapMetrics cbvSrvUavHeap;
    SHeapMetrics samplerHeap;
    SHeapMetrics rtvHeap;
    SHeapMetrics dsvHeap;
    SHeapMetrics bindGroupSrvHeap;
    SHeapMetrics bindGroupSamplerHeap;

    // Upload manager
    uint32_t uploadPagesActive = 0;
    uint32_t uploadPagesPending = 0;
    uint32_t uploadPagesAvailable = 0;
    uint64_t uploadBytesUsedThisFrame = 0;

    // Dynamic buffer
    uint64_t dynamicBufferUsed = 0;
    uint64_t dynamicBufferCapacity = 0;

    // Deferred deletion
    uint32_t pendingDeletions = 0;

    // Per-frame stats
    uint32_t allocationsThisFrame = 0;
    uint32_t deallocationsThisFrame = 0;
    uint32_t failedAllocationsThisFrame = 0;
};

class CDX12MemoryMetrics {
public:
    static CDX12MemoryMetrics& Instance();

    // Collect metrics from all subsystems
    void Update();

    // Get current metrics (thread-safe copy)
    SMemoryMetrics GetMetrics() const;

    // Frame tracking
    void BeginFrame();
    void EndFrame();

    // Increment counters (called by allocators)
    void RecordAllocation() { m_allocationsThisFrame++; }
    void RecordDeallocation() { m_deallocationsThisFrame++; }
    void RecordFailedAllocation() { m_failedAllocationsThisFrame++; }

    // Log summary
    void LogSummary() const;

private:
    CDX12MemoryMetrics() = default;

    mutable std::mutex m_mutex;
    SMemoryMetrics m_metrics;
    uint32_t m_allocationsThisFrame = 0;
    uint32_t m_deallocationsThisFrame = 0;
    uint32_t m_failedAllocationsThisFrame = 0;
};

} // namespace DX12
} // namespace RHI
```

**Step 2: Implement metrics collection**

```cpp
// RHI/DX12/DX12MemoryMetrics.cpp
#include "DX12MemoryMetrics.h"
#include "DX12DescriptorHeap.h"
#include "DX12BindGroupAllocator.h"
#include "DX12UploadManager.h"
#include "DX12Context.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

CDX12MemoryMetrics& CDX12MemoryMetrics::Instance() {
    static CDX12MemoryMetrics instance;
    return instance;
}

void CDX12MemoryMetrics::Update() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Collect descriptor heap metrics
    auto& heapMgr = CDX12DescriptorHeapManager::Instance();

    auto collectHeapMetrics = [](const CDX12DescriptorHeap& heap, SHeapMetrics& out) {
        out.capacity = heap.GetCapacity();
        out.allocated = heap.GetAllocatedCount();
        out.free = heap.GetFreeCount();
        // Note: fragmentation requires buddy allocator integration
    };

    collectHeapMetrics(heapMgr.GetCBVSRVUAVHeap(), m_metrics.cbvSrvUavHeap);
    m_metrics.cbvSrvUavHeap.name = "CBV_SRV_UAV";

    collectHeapMetrics(heapMgr.GetSamplerHeap(), m_metrics.samplerHeap);
    m_metrics.samplerHeap.name = "Sampler";

    collectHeapMetrics(heapMgr.GetRTVHeap(), m_metrics.rtvHeap);
    m_metrics.rtvHeap.name = "RTV";

    collectHeapMetrics(heapMgr.GetDSVHeap(), m_metrics.dsvHeap);
    m_metrics.dsvHeap.name = "DSV";

    // Deferred deletion
    m_metrics.pendingDeletions = static_cast<uint32_t>(CDX12Context::Instance().GetPendingDeletionCount());

    // Per-frame
    m_metrics.allocationsThisFrame = m_allocationsThisFrame;
    m_metrics.deallocationsThisFrame = m_deallocationsThisFrame;
    m_metrics.failedAllocationsThisFrame = m_failedAllocationsThisFrame;
}

SMemoryMetrics CDX12MemoryMetrics::GetMetrics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

void CDX12MemoryMetrics::BeginFrame() {
    m_allocationsThisFrame = 0;
    m_deallocationsThisFrame = 0;
    m_failedAllocationsThisFrame = 0;
}

void CDX12MemoryMetrics::EndFrame() {
    Update();
}

void CDX12MemoryMetrics::LogSummary() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    CFFLog::Info("=== DX12 Memory Metrics ===");
    CFFLog::Info("CBV_SRV_UAV: %u/%u (%.1f%% used)",
        m_metrics.cbvSrvUavHeap.allocated, m_metrics.cbvSrvUavHeap.capacity,
        100.0f * m_metrics.cbvSrvUavHeap.allocated / m_metrics.cbvSrvUavHeap.capacity);
    CFFLog::Info("Sampler: %u/%u", m_metrics.samplerHeap.allocated, m_metrics.samplerHeap.capacity);
    CFFLog::Info("RTV: %u/%u", m_metrics.rtvHeap.allocated, m_metrics.rtvHeap.capacity);
    CFFLog::Info("DSV: %u/%u", m_metrics.dsvHeap.allocated, m_metrics.dsvHeap.capacity);
    CFFLog::Info("Pending deletions: %u", m_metrics.pendingDeletions);
    CFFLog::Info("Frame: alloc=%u dealloc=%u failed=%u",
        m_metrics.allocationsThisFrame, m_metrics.deallocationsThisFrame, m_metrics.failedAllocationsThisFrame);
}

} // namespace DX12
} // namespace RHI
```

**Step 3: Add to CMakeLists.txt**

```cmake
set(RHI_DX12_SOURCES
    # ... existing files ...
    RHI/DX12/DX12MemoryMetrics.cpp
)
```

**Step 4: Integrate into frame loop**

```cpp
// In main.cpp or render loop
CDX12MemoryMetrics::Instance().BeginFrame();
// ... render ...
CDX12MemoryMetrics::Instance().EndFrame();

// Optional: log every N frames
if (frameCount % 300 == 0) {
    CDX12MemoryMetrics::Instance().LogSummary();
}
```

**Step 5: Build and test**

Run: `cmake --build build && E:/forfun/source/code/build/Debug/forfun.exe --test TestDeferredLighting`
Expected: Metrics logged every 300 frames

**Step 6: Commit**

```bash
git add RHI/DX12/DX12MemoryMetrics.h RHI/DX12/DX12MemoryMetrics.cpp
git add CMakeLists.txt main.cpp
git commit -m "feat(DX12): Add memory metrics and telemetry system"
```

---

## Phase 4: Integrate D3D12MemoryAllocator (Future)

> **Note:** This phase requires adding D3D12MA as a dependency. Document here for future reference.

### Task 4.1: Add D3D12MA Dependency

**Files:**
- Download from: `https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator`
- Add: `thirdparty/D3D12MemoryAllocator/`

### Task 4.2: Create D3D12MA Wrapper

**Files:**
- Create: `RHI/DX12/DX12ResourceAllocator.h`
- Create: `RHI/DX12/DX12ResourceAllocator.cpp`

### Task 4.3: Migrate Buffer Creation

### Task 4.4: Migrate Texture Creation

### Task 4.5: Add Defragmentation Support

---

## Summary

| Phase | Tasks | Status |
|-------|-------|--------|
| Phase 1 | Critical Stability Fixes | Ready to implement |
| Phase 2 | Buddy Allocator | Ready to implement |
| Phase 3 | Memory Metrics | Ready to implement |
| Phase 4 | D3D12MA Integration | Future (requires dependency) |

**Estimated Total Tasks:** 7 (Phases 1-3)
**Estimated Lines of Code:** ~800

---

## Testing Strategy

After each task:
1. Run `TestDeferredLighting` - Uses most descriptor types
2. Run `TestBindGroup` - Tests bind group allocator
3. Run `TestClusteredLighting` - Uses many buffers
4. Check `runtime.log` for errors

## Rollback Strategy

Each task has its own commit. If issues arise:
```bash
git revert <commit-hash>
```

---

*Plan created: 2026-01-21*
*Author: Claude Code*
