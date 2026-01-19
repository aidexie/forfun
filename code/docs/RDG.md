# Render Dependency Graph (RDG) Design Document

## Overview

RDG is an automatic resource management system that handles:
- Pass dependency analysis and execution ordering
- Resource state tracking and barrier insertion
- **Memory aliasing via placed resources** (VRAM optimization)

This design is inspired by Frostbite Frame Graph (GDC 2017) and Unreal Engine 5 RDG.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        SETUP PHASE                               │
│  User code calls AddPass<PassData>() to declare render passes    │
│  CreateTexture() / ImportTexture() returns typed handles         │
│  No GPU resources allocated yet - just metadata                  │
└─────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────┐
│                       COMPILE PHASE                              │
│  1. Build DAG from pass read/write declarations                  │
│  2. Topological sort → execution order                           │
│  3. Cull unused passes and resources                             │
│  4. Lifetime analysis: [firstUse, lastUse] per resource          │
│  5. Memory aliasing: bin-pack non-overlapping resources          │
│  6. Calculate barrier placements                                 │
└─────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────┐
│                       EXECUTE PHASE                              │
│  1. Allocate placed resources into heaps                         │
│  2. For each pass in order:                                      │
│     a. Insert barriers (transition + aliasing)                   │
│     b. Resolve handles to GPU resources                          │
│     c. Execute pass lambda                                       │
│  3. Ensure imported resources reach final state                  │
│  4. Release transient allocations back to pool                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Core Design Decisions

### 1. Typed Handle System

**Choice**: Typed wrapper class with frame ID validation

```cpp
template<typename Tag>
class RDGHandle {
    uint32_t m_Index : 20;      // Supports 1M resources per frame
    uint32_t m_FrameId : 12;    // Detects stale handles (4096 frame wrap)
#if _DEBUG
    const char* m_DebugName;    // Debug only - zero cost in release
#endif

public:
    static constexpr uint32_t InvalidIndex = (1 << 20) - 1;

    bool IsValid() const { return m_Index != InvalidIndex; }
    uint32_t GetIndex() const { return m_Index; }
    uint32_t GetFrameId() const { return m_FrameId; }
};

// Type-safe aliases - compiler prevents mixing
struct RDGTextureTag {};
struct RDGBufferTag {};

using RDGTextureHandle = RDGHandle<RDGTextureTag>;
using RDGBufferHandle = RDGHandle<RDGBufferTag>;
```

**Benefits**:
- Compile-time type safety (can't pass buffer handle to texture parameter)
- Runtime stale handle detection (catch use-after-free bugs)
- Debug names for profiling/visualization

### 2. Template Struct PassData (UE5 Style)

**Choice**: Explicit struct for pass data with separate setup/execute lambdas

```cpp
// User defines struct for each pass type
struct FSSAOPassData {
    // Resource handles
    RDGTextureHandle DepthInput;
    RDGTextureHandle NormalInput;
    RDGTextureHandle AOOutput;

    // Pass parameters (copied, not referenced)
    float Radius;
    float Intensity;
    int SampleCount;
};

// Usage
rdg.AddPass<FSSAOPassData>("SSAO",
    // Setup lambda - declares dependencies, populates PassData
    [&](FSSAOPassData& data, RDGPassBuilder& builder) {
        data.DepthInput = builder.ReadTexture(depthHandle);
        data.NormalInput = builder.ReadTexture(normalHandle);
        data.AOOutput = builder.CreateTexture("AOResult", aoDesc);
        builder.WriteUAV(data.AOOutput);

        // Copy parameters (not references!)
        data.Radius = settings.Radius;
        data.Intensity = settings.Intensity;
        data.SampleCount = settings.SampleCount;
    },
    // Execute lambda - receives const PassData, does actual work
    [](const FSSAOPassData& data, RDGContext& ctx) {
        auto* cmdList = ctx.GetCommandList();
        // Bind resources using handles
        cmdList->SetComputeRootDescriptorTable(0, ctx.GetSRV(data.DepthInput));
        cmdList->SetComputeRootDescriptorTable(1, ctx.GetSRV(data.NormalInput));
        cmdList->SetComputeRootDescriptorTable(2, ctx.GetUAV(data.AOOutput));
        // Dispatch...
    }
);
```

**Benefits**:
- RDG can inspect all dependencies (stored in struct fields)
- No dangling reference risk (data is copied, not referenced)
- Clear separation: setup (what) vs execute (how)
- Easy to serialize, visualize, debug

### 3. Imported Resources with Explicit State

**Choice**: Caller specifies initial and final resource states

```cpp
// Swapchain back buffer - starts in PRESENT, must end in PRESENT
auto backBuffer = rdg.ImportTexture(
    "BackBuffer",
    swapchainResource,
    D3D12_RESOURCE_STATE_PRESENT,      // State when RDG begins
    D3D12_RESOURCE_STATE_PRESENT       // State RDG must leave it in
);

// TAA history - persistent across frames
auto taaHistory = rdg.ImportTexture(
    "TAAHistory",
    historyResource,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // Read last frame
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS        // Write this frame
);

// Extract - keep resource alive past RDG execution
ID3D12Resource* extractedResource;
rdg.ExtractTexture(
    ssrResultHandle,
    &extractedResource,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE   // Final state
);
```

**Benefits**:
- No hidden state assumptions
- RDG knows exactly what barriers to insert
- Supports complex multi-frame resources (history buffers, etc.)

---

## Memory Aliasing System

### Lifetime Analysis

```cpp
struct RDGResourceLifetime {
    uint32_t ResourceIndex;
    uint32_t FirstPassIndex;    // First pass using this resource
    uint32_t LastPassIndex;     // Last pass using this resource
    uint64_t SizeInBytes;
    uint64_t Alignment;         // 64KB or 4MB for MSAA
};
```

**Algorithm**:
1. For each transient resource, find first and last pass that references it
2. Build interval graph: resources A and B can share memory if `A.last < B.first` or `B.last < A.first`
3. Sort by size (descending) for better packing
4. First-Fit Decreasing bin packing into heap offsets

### Memory Layout Example

```
Frame passes: GBuffer → SSAO → SSR → Lighting → ToneMap
                 ↓
Resource lifetimes:
  Albedo:     [Pass 0 ─────── Pass 3]
  Normal:     [Pass 0 ─────── Pass 3]
  Depth:      [Pass 0 ─────────────── Pass 4]
  AOResult:   [Pass 1 ─ Pass 3]
  SSRResult:        [Pass 2 ─ Pass 3]
                 ↓
Aliasing opportunities:
  - AOResult ends at Pass 3, but overlaps with Albedo/Normal
  - After Pass 3, Albedo/Normal memory can be reused
                 ↓
Heap layout:
  ┌────────────────────────────────────────┐
  │ Offset 0:   [Albedo]                   │
  │ Offset 16M: [Normal]                   │
  │ Offset 32M: [Depth] ────────────────── │ (lives longest)
  │ Offset 48M: [AOResult] → [SSRResult]   │ (aliased!)
  └────────────────────────────────────────┘
```

### Heap Pool Design

```cpp
class CRDGHeapAllocator {
public:
    enum class EHeapType {
        RenderTarget,   // D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
        UAVTexture,     // D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES
        Buffer          // D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
    };

    struct Allocation {
        ID3D12Heap* Heap;
        uint64_t Offset;
        uint64_t Size;
    };

    // Allocate from appropriate pool
    Allocation Allocate(EHeapType type, uint64_t size, uint64_t alignment);

    // Reset all pools (call at frame end)
    void Reset();

private:
    static constexpr uint64_t DefaultHeapSize = 256 * 1024 * 1024;  // 256MB
    static constexpr uint64_t DefaultAlignment = 64 * 1024;         // 64KB
    static constexpr uint64_t MSAAAlignment = 4 * 1024 * 1024;      // 4MB

    struct HeapPool {
        std::vector<ComPtr<ID3D12Heap>> Heaps;
        uint64_t CurrentHeapIndex = 0;
        uint64_t CurrentOffset = 0;

        void Reset() {
            CurrentHeapIndex = 0;
            CurrentOffset = 0;
        }
    };

    HeapPool m_RTDSPool;
    HeapPool m_TexturePool;
    HeapPool m_BufferPool;
};
```

### Placed Resource Creation

```cpp
// During Execute phase
ID3D12Resource* CRDGExecutor::CreatePlacedResource(
    const RDGTextureDesc& desc,
    const CRDGHeapAllocator::Allocation& alloc)
{
    D3D12_RESOURCE_DESC d3dDesc = ConvertToD3D12Desc(desc);

    ID3D12Resource* resource = nullptr;
    HRESULT hr = m_Device->CreatePlacedResource(
        alloc.Heap,
        alloc.Offset,
        &d3dDesc,
        D3D12_RESOURCE_STATE_COMMON,  // Initial state
        nullptr,                       // Clear value (optional for RT)
        IID_PPV_ARGS(&resource)
    );

    return resource;
}
```

---

## Barrier System

### State Tracking

```cpp
struct ResourceState {
    D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
    bool IsAliased = false;           // Part of aliasing group
    uint32_t LastAliasingBarrier = 0; // Pass index
};

class CRDGStateTracker {
    std::unordered_map<ID3D12Resource*, ResourceState> m_States;

public:
    void SetInitialState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);
    D3D12_RESOURCE_STATES GetCurrentState(ID3D12Resource* resource);
    void RecordTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState);
};
```

### Barrier Batching

```cpp
class CRDGBarrierBatcher {
    std::vector<D3D12_RESOURCE_BARRIER> m_Pending;

public:
    void AddTransition(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        // Skip no-op transitions
        if (before == after) return;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_Pending.push_back(barrier);
    }

    void AddAliasing(ID3D12Resource* before, ID3D12Resource* after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        barrier.Aliasing.pResourceBefore = before;  // nullptr = any
        barrier.Aliasing.pResourceAfter = after;
        m_Pending.push_back(barrier);
    }

    void Flush(ID3D12GraphicsCommandList* cmdList)
    {
        if (!m_Pending.empty()) {
            cmdList->ResourceBarrier(
                static_cast<UINT>(m_Pending.size()),
                m_Pending.data()
            );
            m_Pending.clear();
        }
    }
};
```

### Aliasing Barrier Insertion

When two resources share the same heap memory:

```cpp
// Pass 3 ends (last use of GBuffer.Albedo)
// Pass 4 begins (first use of SSRResult, aliased with Albedo)

barrierBatcher.AddAliasing(
    albedoResource,     // Resource we're done with
    ssrResultResource   // Resource we're about to use
);

// Then transition SSRResult to the state needed by Pass 4
barrierBatcher.AddTransition(
    ssrResultResource,
    D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS
);

barrierBatcher.Flush(cmdList);
```

---

## File Structure

```
Core/RDG/
├── RDGTypes.h              # RDGHandle, RDGTextureDesc, RDGBufferDesc, enums
├── RDGBuilder.h            # CRDGBuilder, RDGPassBuilder (template AddPass)
├── RDGBuilder.cpp          # Non-template implementation
├── RDGCompiler.h           # CRDGCompiler (DAG, topological sort)
├── RDGCompiler.cpp
├── RDGMemoryAliasing.h     # Lifetime analysis, bin packing algorithm
├── RDGMemoryAliasing.cpp
├── RDGHeapAllocator.h      # Heap pools, placed resource allocation
├── RDGHeapAllocator.cpp
├── RDGBarrierBatcher.h     # State tracking, barrier batching
├── RDGBarrierBatcher.cpp
├── RDGContext.h            # RDGContext (handle resolution, execution)
├── RDGContext.cpp
└── RDGDebug.h/cpp          # Graphviz export, memory visualization
```

---

## Usage Example

```cpp
void RenderFrame(CRDGBuilder& rdg)
{
    // Import external resources
    auto backBuffer = rdg.ImportTexture("BackBuffer",
        m_SwapChain->GetCurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT);

    auto taaHistory = rdg.ImportTexture("TAAHistory",
        m_TAAHistoryBuffer,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // GBuffer pass
    RDGTextureHandle albedo, normal, depth;
    rdg.AddPass<FGBufferPassData>("GBuffer",
        [&](FGBufferPassData& data, RDGPassBuilder& builder) {
            data.Albedo = builder.CreateTexture("GBuffer.Albedo",
                {m_Width, m_Height, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB});
            data.Normal = builder.CreateTexture("GBuffer.Normal",
                {m_Width, m_Height, DXGI_FORMAT_R16G16B16A16_FLOAT});
            data.Depth = builder.CreateTexture("GBuffer.Depth",
                {m_Width, m_Height, DXGI_FORMAT_D32_FLOAT});

            builder.WriteRTV(data.Albedo);
            builder.WriteRTV(data.Normal);
            builder.WriteDSV(data.Depth);

            // Export for later passes
            albedo = data.Albedo;
            normal = data.Normal;
            depth = data.Depth;
        },
        [this](const FGBufferPassData& data, RDGContext& ctx) {
            RenderGBuffer(ctx, data);
        }
    );

    // SSAO pass
    RDGTextureHandle aoResult;
    rdg.AddPass<FSSAOPassData>("SSAO",
        [&](FSSAOPassData& data, RDGPassBuilder& builder) {
            data.Depth = builder.ReadTexture(depth);
            data.Normal = builder.ReadTexture(normal);
            data.AOResult = builder.CreateTexture("SSAO.Result",
                {m_Width/2, m_Height/2, DXGI_FORMAT_R8_UNORM});
            builder.WriteUAV(data.AOResult);
            aoResult = data.AOResult;
        },
        [this](const FSSAOPassData& data, RDGContext& ctx) {
            ComputeSSAO(ctx, data);
        }
    );

    // Lighting pass (reads GBuffer + SSAO, writes to HDR target)
    RDGTextureHandle hdrTarget;
    rdg.AddPass<FLightingPassData>("Lighting",
        [&](FLightingPassData& data, RDGPassBuilder& builder) {
            data.Albedo = builder.ReadTexture(albedo);
            data.Normal = builder.ReadTexture(normal);
            data.Depth = builder.ReadTexture(depth);
            data.AO = builder.ReadTexture(aoResult);
            data.HDRTarget = builder.CreateTexture("HDR.Target",
                {m_Width, m_Height, DXGI_FORMAT_R16G16B16A16_FLOAT});
            builder.WriteRTV(data.HDRTarget);
            hdrTarget = data.HDRTarget;
        },
        [this](const FLightingPassData& data, RDGContext& ctx) {
            RenderLighting(ctx, data);
        }
    );

    // ToneMap to back buffer
    rdg.AddPass<FToneMapPassData>("ToneMap",
        [&](FToneMapPassData& data, RDGPassBuilder& builder) {
            data.HDRInput = builder.ReadTexture(hdrTarget);
            data.Output = builder.WriteTexture(backBuffer);
        },
        [this](const FToneMapPassData& data, RDGContext& ctx) {
            ToneMap(ctx, data);
        }
    );

    // Compile and execute
    rdg.Compile();
    rdg.Execute(m_CommandList.Get());
}
```

---

## Implementation Phases

### Phase 1: Core Types & Handle System (Day 1)
**File**: `Core/RDG/RDGTypes.h`

- `RDGHandle<Tag>`: Type-safe handle (index + frameId + debug name)
- `RDGTextureHandle`, `RDGBufferHandle`: Distinct types, compile-time safety
- `RDGTextureDesc`, `RDGBufferDesc`: Resource descriptors
- `ERDGPassFlags`: Raster / Compute / Copy / AsyncCompute

### Phase 2: Pass Graph API & Resource Registry (Days 2-3)
**Files**: `Core/RDG/RDGBuilder.h`, `Core/RDG/RDGBuilder.cpp`

- `CRDGBuilder`: Frame-level pass graph builder
- `RDGPassBuilder`: Per-pass dependency declaration
- `AddPass<PassData>()`: Template method with setup/execute separation
- `CreateTexture()` / `CreateBuffer()`: Transient resources
- `ImportTexture()` / `ImportBuffer()`: External resources (swapchain, history)
- `ExtractTexture()`: Keep resource alive beyond RDG execution

### Phase 3: Dependency Analysis & Compilation (Days 4-5)
**Files**: `Core/RDG/RDGCompiler.h`, `Core/RDG/RDGCompiler.cpp`

- Build DAG from pass declarations (adjacency list)
- Kahn's algorithm for topological sort
- Cycle detection → compile-time error
- Dead pass culling (unused outputs → remove)
- Dead resource culling

### Phase 4: Lifetime Analysis & Memory Aliasing (Days 6-7)
**Files**: `Core/RDG/RDGMemoryAliasing.h`, `Core/RDG/RDGMemoryAliasing.cpp`

- Calculate `[firstUse, lastUse]` interval per transient resource
- Interval graph → non-overlap detection
- First-Fit Decreasing bin packing for heap offset assignment
- Aliasing group generation

### Phase 5: Heap Management & Placed Resources (Days 8-9)
**Files**: `Core/RDG/RDGHeapAllocator.h`, `Core/RDG/RDGHeapAllocator.cpp`

- `CRDGHeapAllocator`: Manage `ID3D12Heap` pools
- Heap categorization: RT/DS heap vs UAV heap vs Buffer heap
- Alignment: 64KB (default) / 4MB (MSAA)
- `CreatePlacedResource()` instead of `CreateCommittedResource()`
- Frame-end recycling, next-frame reuse

### Phase 6: Automatic Barrier Insertion (Days 10-11)
**Files**: `Core/RDG/RDGBarrierBatcher.h`, `Core/RDG/RDGBarrierBatcher.cpp`

- Per-resource state tracking (state machine)
- Barrier batching (reduce `ResourceBarrier()` calls)
- **Transition Barrier**: State transitions
- **Aliasing Barrier**: Same heap location resource switching
- Imported resources: Ensure final state correctness

### Phase 7: RDG Context & Execution (Day 12)
**Files**: `Core/RDG/RDGContext.h`, `Core/RDG/RDGContext.cpp`

- `RDGContext`: Execution context for pass lambdas
- Handle → actual GPU resource resolution
- SRV/UAV/RTV/DSV creation and caching

### Phase 8: Integration & Validation (Days 13-14)
- Replace existing manual barrier code (gradual migration)
- Debug visualization: Pass dependency graph (Graphviz DOT export)
- Memory visualization: Heap layout, aliasing timeline
- VRAM usage statistics comparison (before/after)

---

## Test Cases

### TestRDGBasic
- Create simple 3-pass graph (A → B → C)
- Verify execution order
- Verify barriers are inserted

### TestRDGAliasing
- Create passes with non-overlapping resource lifetimes
- Verify resources share heap memory
- Log VRAM savings (expected 30-50%)

### TestRDGBarrier
- Test transition barriers (COMMON → RTV → SRV)
- Test aliasing barriers
- Run with D3D12 GPU validation layer enabled

### TestRDGImport
- Import swapchain, verify PRESENT → RTV → PRESENT transitions
- Import TAA history, verify cross-frame state preservation
- Test ExtractTexture for persistent resources

---

## References

- [Frostbite Frame Graph - GDC 2017](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)
- [Unreal Engine 5 RDG Documentation](https://docs.unrealengine.com/5.0/en-US/render-dependency-graph-in-unreal-engine/)
- [D3D12 Memory Management Strategies](https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-management-strategies)
- [D3D12 Placed Resources](https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-management#placed-resources)

---

**Last Updated**: 2026-01-19
