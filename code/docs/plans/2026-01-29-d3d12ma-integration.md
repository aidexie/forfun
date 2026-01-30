# D3D12MA Integration Design

**Date**: 2026-01-29
**Status**: Draft
**Scope**: Phase 1 - Static textures/buffers (DEFAULT heap)

---

## Overview

Integrate AMD D3D12 Memory Allocator (D3D12MA) to replace `CreateCommittedResource` with placed resources in shared heaps.

**Motivations:**
1. Memory efficiency - Reduce VRAM fragmentation via suballocation
2. Aliasing support - Enable memory aliasing for transient render targets
3. Debugging/profiling - Better memory tracking, budgeting, leak detection
4. Future-proofing - Prepare for residency management, tiled resources

---

## Current State

Resources created via `CreateCommittedResource`:
- Each resource gets dedicated heap (64KB alignment overhead)
- No suballocation or memory sharing
- Fragmentation accumulates over time

Existing memory systems:
- `CDX12UploadManager` - Ring buffer for uploads (already efficient)
- `CDX12DynamicBufferRing` - Per-frame constant data (already efficient)

---

## Architecture

### Integration Point

```
┌─────────────────────────────────────────────────────────────┐
│                     Engine/Rendering                         │
└─────────────────────────┬───────────────────────────────────┘
                          │ Uses IBuffer, ITexture
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                    RHI Interface Layer                       │
│              IRHIRenderContext::CreateBuffer()               │
│              IRHIRenderContext::CreateTexture()              │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                   DX12 Implementation                        │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              CDX12MemoryAllocator (NEW)               │  │
│  │         Wraps D3D12MA::Allocator + D3D12MA::Pool      │  │
│  └───────────────────────────────────────────────────────┘  │
│                          │                                   │
│            ┌─────────────┼─────────────┐                    │
│            ▼             ▼             ▼                    │
│     CDX12Buffer    CDX12Texture   CDX12UploadManager        │
│   (holds D3D12MA   (holds D3D12MA    (Phase 2)              │
│    ::Allocation)    ::Allocation)                           │
└─────────────────────────────────────────────────────────────┘
```

### New Class: CDX12MemoryAllocator

**File: `RHI/DX12/DX12MemoryAllocator.h`**

```cpp
#pragma once
#include "D3D12MemAlloc.h"
#include "DX12Common.h"
#include <deque>

namespace RHI {
namespace DX12 {

struct SMemoryAllocation {
    D3D12MA::Allocation* allocation = nullptr;
    ID3D12Resource* resource = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    void* cpuAddress = nullptr;

    bool IsValid() const { return allocation != nullptr; }
};

class CDX12MemoryAllocator {
public:
    static CDX12MemoryAllocator& Instance();

    bool Initialize(ID3D12Device* device, IDXGIAdapter* adapter);
    void Shutdown();

    SMemoryAllocation CreateBuffer(const D3D12_RESOURCE_DESC& desc,
                                   D3D12_HEAP_TYPE heapType,
                                   D3D12_RESOURCE_STATES initialState);

    SMemoryAllocation CreateTexture(const D3D12_RESOURCE_DESC& desc,
                                    D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES initialState,
                                    const D3D12_CLEAR_VALUE* clearValue = nullptr);

    void FreeAllocation(D3D12MA::Allocation* allocation, uint64_t fenceValue);
    void ProcessDeferredFrees(uint64_t completedFenceValue);

    void GetBudget(D3D12MA::Budget* localBudget, D3D12MA::Budget* nonLocalBudget);
    void LogStatistics();

private:
    D3D12MA::Allocator* m_allocator = nullptr;

    struct SPendingFree {
        D3D12MA::Allocation* allocation;
        uint64_t fenceValue;
    };
    std::deque<SPendingFree> m_pendingFrees;
};

} // namespace DX12
} // namespace RHI
```

### Modified Resource Classes

**CDX12Buffer changes:**

```cpp
class CDX12Buffer : public IBuffer {
public:
    // New constructor
    CDX12Buffer(SMemoryAllocation&& allocation, const BufferDesc& desc, ID3D12Device* device);
    ~CDX12Buffer() override;

private:
    // Replace ComPtr<ID3D12Resource> with:
    D3D12MA::Allocation* m_allocation = nullptr;  // Owns the memory
    ID3D12Resource* m_resource = nullptr;         // Non-owning convenience pointer
};
```

**CDX12Texture changes:**

```cpp
class CDX12Texture : public ITexture {
public:
    // New constructor
    CDX12Texture(SMemoryAllocation&& allocation, const TextureDesc& desc, ID3D12Device* device);
    ~CDX12Texture() override;

private:
    // Replace ComPtr<ID3D12Resource> with:
    D3D12MA::Allocation* m_allocation = nullptr;
    ID3D12Resource* m_resource = nullptr;
};
```

**Destructor pattern:**

```cpp
CDX12Buffer::~CDX12Buffer() {
    if (m_allocation) {
        uint64_t fenceValue = CDX12Context::Instance().GetCurrentFenceValue();
        CDX12MemoryAllocator::Instance().FreeAllocation(m_allocation, fenceValue);
        m_allocation = nullptr;
    }
}
```

### RenderContext Integration

**CreateBuffer modification:**

```cpp
IBuffer* CDX12RenderContext::CreateBuffer(const BufferDesc& desc) {
    D3D12_RESOURCE_DESC resourceDesc = {};
    // ... build resourceDesc ...

    D3D12_HEAP_TYPE heapType = GetHeapType(desc.cpuAccess, desc.usage);
    D3D12_RESOURCE_STATES initialState = GetInitialResourceState(heapType, desc.usage);

    SMemoryAllocation allocation = CDX12MemoryAllocator::Instance()
        .CreateBuffer(resourceDesc, heapType, initialState);

    if (!allocation.IsValid()) {
        CFFLog::Error("[RenderContext] Failed to allocate buffer");
        return nullptr;
    }

    return new CDX12Buffer(std::move(allocation), desc, m_device);
}
```

**CreateTexture modification:**

```cpp
ITexture* CDX12RenderContext::CreateTexture(const TextureDesc& desc) {
    D3D12_RESOURCE_DESC resourceDesc = BuildTextureResourceDesc(desc);
    D3D12_HEAP_TYPE heapType = GetHeapType(desc.cpuAccess, desc.usage);
    D3D12_RESOURCE_STATES initialState = GetInitialResourceState(heapType, desc.usage);

    D3D12_CLEAR_VALUE* clearValue = nullptr;
    D3D12_CLEAR_VALUE clearValueStorage;
    if (desc.usage & ETextureUsage::RenderTarget) {
        clearValueStorage = { ToDXGIFormat(desc.format), {0, 0, 0, 0} };
        clearValue = &clearValueStorage;
    } else if (desc.usage & ETextureUsage::DepthStencil) {
        clearValueStorage = { ToDXGIFormat(desc.format), {0.0f, 0} };
        clearValue = &clearValueStorage;
    }

    SMemoryAllocation allocation = CDX12MemoryAllocator::Instance()
        .CreateTexture(resourceDesc, heapType, initialState, clearValue);

    if (!allocation.IsValid()) {
        CFFLog::Error("[RenderContext] Failed to allocate texture");
        return nullptr;
    }

    return new CDX12Texture(std::move(allocation), desc, m_device);
}
```

### Initialization Order

**CDX12Context::Initialize:**

```
Factory → Adapter → Device → MemoryAllocator → DescriptorHeaps → UploadManager → ...
```

**CDX12Context::Shutdown:**

```
... → UploadManager → DescriptorHeaps → ProcessDeferredFrees(UINT64_MAX) → MemoryAllocator → Device
```

---

## Third-Party Integration

**File location:**

```
E:/forfun/thirdparty/
└── D3D12MemoryAllocator/
    ├── D3D12MemAlloc.h
    └── D3D12MemAlloc.cpp
```

**CMakeLists.txt:**

```cmake
set(RHI_DX12_SOURCES
    # ... existing sources ...
    RHI/DX12/DX12MemoryAllocator.cpp
    ${THIRDPARTY_DIR}/D3D12MemoryAllocator/D3D12MemAlloc.cpp
)

target_include_directories(forfun PRIVATE
    ${THIRDPARTY_DIR}/D3D12MemoryAllocator
)
```

**Debug configuration (in DX12MemoryAllocator.cpp):**

```cpp
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED

#ifdef _DEBUG
#define D3D12MA_DEBUG_MARGIN 16
#define D3D12MA_DEBUG_GLOBAL_MUTEX 1
#endif

#include "D3D12MemAlloc.h"
```

---

## File Changes Summary

**New files:**
- `RHI/DX12/DX12MemoryAllocator.h`
- `RHI/DX12/DX12MemoryAllocator.cpp`

**Modified files:**
- `RHI/DX12/DX12Resources.h` - Add allocation member
- `RHI/DX12/DX12Resources.cpp` - New constructor, destructor
- `RHI/DX12/DX12RenderContext.cpp` - Use new allocator
- `RHI/DX12/DX12Context.cpp` - Init/shutdown order
- `CMakeLists.txt` - Add D3D12MA source

**Third-party:**
- `thirdparty/D3D12MemoryAllocator/D3D12MemAlloc.h`
- `thirdparty/D3D12MemoryAllocator/D3D12MemAlloc.cpp`

---

## Implementation Steps

| Step | Task | Risk |
|------|------|------|
| 1 | Download D3D12MA, add to thirdparty | Low |
| 2 | Create `CDX12MemoryAllocator` class | Low |
| 3 | Modify `CDX12Buffer` to hold allocation | Medium |
| 4 | Modify `CDX12Texture` to hold allocation | Medium |
| 5 | Update `CDX12RenderContext::CreateBuffer` | Medium |
| 6 | Update `CDX12RenderContext::CreateTexture` | Medium |
| 7 | Update `CDX12Context` init/shutdown | Low |
| 8 | Update CMakeLists.txt | Low |
| 9 | Test: run editor, verify no crashes | - |
| 10 | Test: check memory stats via `LogStatistics()` | - |

---

## Future Phases

- **Phase 2**: Replace `CDX12UploadManager` with D3D12MA virtual blocks
- **Phase 3**: Replace `CDX12DynamicBufferRing` with D3D12MA
- **Phase 4**: Add memory aliasing for transient render targets (G-buffer reuse)

---

## References

- [D3D12MA GitHub](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)
- [D3D12MA Documentation](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/)
