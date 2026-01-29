# Phase 3.7: Descriptor Set Abstraction - Design Document

## Goal

Create a unified descriptor binding abstraction that:
1. Works across **DX12 and Vulkan** backends (DX11 not supported)
2. Reduces per-draw overhead through descriptor set reuse
3. **Deeply integrates with RDG** for automatic resource management

## Scope

- **In scope**: DX12, Vulkan, RDG integration
- **Out of scope**: DX11 (deprecated for this feature)

## Design Decisions

| Decision | Choice |
|----------|--------|
| Interface Style | **Vulkan-style** (explicit layout definition) |
| Binding Model | **Separate Tables Per Set** (breaking change, requires shader updates) |
| Frequency Model | **4-level** (PerFrame/PerPass/PerMaterial/PerDraw) |

---

## Architecture Overview

### Design Principle: Per-Pipeline Root Signature / Pipeline Layout

**Key Insight**: Root Signature is derived from the descriptor set layouts used by each pipeline, not a global monolithic layout.

```
Pipeline A (MainPass) ────┐
                          ├──► Root Signature X (uses Set0 + Set2 layouts)
Pipeline B (ShadowPass) ──┘    (shared - same layout combination)

Pipeline C (BloomPass) ──────► Root Signature Y (uses Set1 layout only)

Pipeline D (SSAO) ───────────► Root Signature Y (shared - same layout)
```

**Benefits:**
- Optimal binding (only needed parameters in root signature)
- Direct Vulkan mapping (`VkPipelineLayout` = Root Signature)
- Automatic caching and sharing for identical layouts
- Flexible per-pipeline optimization

### Pipeline Creation with Set Layouts

```cpp
struct SPipelineStateDesc {
    // ... existing fields (shaders, rasterizer, blend, etc.) ...

    // NEW: Descriptor set layouts this pipeline uses
    // Index = set index (0-3), nullptr = set not used by this pipeline
    IDescriptorSetLayout* setLayouts[4] = {nullptr, nullptr, nullptr, nullptr};
};

// Example: MainPass pipeline uses Set0 (PerFrame) and Set2 (PerMaterial)
SPipelineStateDesc mainPassDesc;
mainPassDesc.setLayouts[0] = perFrameLayout;    // Set 0: PerFrame
mainPassDesc.setLayouts[1] = nullptr;           // Set 1: unused
mainPassDesc.setLayouts[2] = materialLayout;    // Set 2: PerMaterial
mainPassDesc.setLayouts[3] = nullptr;           // Set 3: unused
auto* mainPassPSO = renderContext->CreatePipelineState(mainPassDesc);
```

### Root Signature Caching (DX12)

```cpp
class CDX12RootSignatureCache {
private:
    // Key: hash of set layout pointers
    struct CacheKey {
        IDescriptorSetLayout* layouts[4];
        bool operator==(const CacheKey& other) const;
    };
    std::unordered_map<CacheKey, ComPtr<ID3D12RootSignature>, CacheKeyHash> m_cache;

public:
    // Returns cached or newly created root signature
    ID3D12RootSignature* GetOrCreate(IDescriptorSetLayout* const* layouts, uint32_t count);
};

class CDX12PipelineState : public IPipelineState {
private:
    ComPtr<ID3D12PipelineState> m_pso;
    ID3D12RootSignature* m_rootSignature;  // Shared via cache

    // Per-set binding info (computed from layouts)
    struct SetBindingInfo {
        uint32_t srvTableRootParam = UINT32_MAX;     // Root param index for SRV table
        uint32_t samplerTableRootParam = UINT32_MAX; // Root param index for Sampler table
        uint32_t cbvRootParam = UINT32_MAX;          // Root param index for CBV
        uint32_t pushConstantRootParam = UINT32_MAX; // Root param index for push constants
        bool isUsed = false;
    };
    SetBindingInfo m_setBindings[4];

public:
    // Called by CDX12CommandList::BindDescriptorSet()
    const SetBindingInfo& GetSetBindingInfo(uint32_t setIndex) const;
};
```

### Command List Binding with Pipeline Context

```cpp
void CDX12CommandList::BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) {
    // Must have PSO set first to know root signature layout
    assert(m_currentPSO != nullptr);

    const auto& bindingInfo = m_currentPSO->GetSetBindingInfo(setIndex);
    if (!bindingInfo.isUsed) {
        // This set is not used by current pipeline - skip
        return;
    }

    // STRICT layout matching - pointer equality required
    assert(set->GetLayout() == m_currentPSO->GetExpectedLayout(setIndex)
           && "Layout pointer mismatch - must use same layout instance");

    auto* dx12Set = static_cast<CDX12DescriptorSet*>(set);

    // Bind SRV table if present
    if (bindingInfo.srvTableRootParam != UINT32_MAX && dx12Set->HasSRVs()) {
        auto gpuHandle = dx12Set->CopySRVsToStaging(m_stagingRing);
        m_commandList->SetGraphicsRootDescriptorTable(
            bindingInfo.srvTableRootParam, gpuHandle);
    }

    // Bind Sampler table if present
    if (bindingInfo.samplerTableRootParam != UINT32_MAX && dx12Set->HasSamplers()) {
        auto gpuHandle = dx12Set->CopySamplersToStaging(m_samplerStagingRing);
        m_commandList->SetGraphicsRootDescriptorTable(
            bindingInfo.samplerTableRootParam, gpuHandle);
    }

    // Bind CBV if present
    if (bindingInfo.cbvRootParam != UINT32_MAX && dx12Set->HasCBV()) {
        m_commandList->SetGraphicsRootConstantBufferView(
            bindingInfo.cbvRootParam, dx12Set->GetCBVGpuAddress());
    }

    // Bind Push Constants if present
    if (bindingInfo.pushConstantRootParam != UINT32_MAX && dx12Set->HasPushConstants()) {
        m_commandList->SetGraphicsRoot32BitConstants(
            bindingInfo.pushConstantRootParam,
            dx12Set->GetPushConstantDwordCount(),
            dx12Set->GetPushConstantData(), 0);
    }
}
```

### Strict Layout Matching Policy

**Rule**: `set->GetLayout() == pipeline->GetExpectedLayout(setIndex)` (pointer equality)

This means:
1. Each `CreateLayout()` call returns a NEW layout instance
2. User must store and share layout pointers explicitly
3. Same layout instance must be used for both pipeline creation and set allocation

```cpp
// ✅ CORRECT - Same layout instance shared
class CRenderer {
    IDescriptorSetLayout* m_materialLayout;  // Shared instance

    void Initialize() {
        m_materialLayout = allocator->CreateLayout(...);
    }

    void CreatePipeline() {
        psoDesc.setLayouts[2] = m_materialLayout;  // Use shared
    }

    void CreateMaterial() {
        auto* set = allocator->AllocatePersistentSet(m_materialLayout);  // Use shared
    }
};

// ❌ WRONG - Different layout instances (even if identical structure)
auto* layoutA = allocator->CreateLayout(BindingLayoutDesc("A").AddItem(...));
auto* layoutB = allocator->CreateLayout(BindingLayoutDesc("B").AddItem(...));  // Different pointer!

psoDesc.setLayouts[2] = layoutA;
auto* set = allocator->AllocatePersistentSet(layoutB);  // WILL FAIL: layoutA != layoutB
```

### Shader Register Mapping (DX12 Spaces → Vulkan Sets)

```hlsl
// DX12 HLSL - Use register spaces
// PerFrame (space0 → Vulkan set=0)
Texture2DArray gShadowMaps     : register(t0, space0);
TextureCubeArray gIrradiance   : register(t1, space0);
TextureCubeArray gPrefiltered  : register(t2, space0);
Texture2D gBrdfLUT             : register(t3, space0);

// PerPass (space1 → Vulkan set=1)
Texture2D gGBufferAlbedo       : register(t0, space1);
Texture2D gGBufferNormal       : register(t1, space1);
Texture2D gDepthBuffer         : register(t2, space1);

// PerMaterial (space2 → Vulkan set=2)
Texture2D gAlbedoTex           : register(t0, space2);
Texture2D gNormalTex           : register(t1, space2);
Texture2D gMetallicRoughness   : register(t2, space2);
Texture2D gEmissiveTex         : register(t3, space2);

// Constant Buffers (root CBVs)
cbuffer CB_PerFrame : register(b0) { ... };
cbuffer CB_PerDraw  : register(b1) { ... };
```

```glsl
// Vulkan GLSL equivalent
layout(set = 0, binding = 0) uniform texture2DArray gShadowMaps;
layout(set = 0, binding = 1) uniform textureCubeArray gIrradiance;

layout(set = 1, binding = 0) uniform texture2D gGBufferAlbedo;
layout(set = 1, binding = 1) uniform texture2D gGBufferNormal;

layout(set = 2, binding = 0) uniform texture2D gAlbedoTex;
layout(set = 2, binding = 1) uniform texture2D gNormalTex;

// Push constants for per-draw data
layout(push_constant) uniform CB_PerDraw { ... };
```

---

## Core Interfaces

**API Style: NVRHI-inspired** with static factory methods and fluent builder pattern.

```cpp
// RHI/IDescriptorSet.h

namespace RHI {

//--------------------------------------------------
// Descriptor Types
//--------------------------------------------------
enum class EDescriptorType : uint8_t {
    Texture_SRV,              // Texture2D, TextureCube, Texture2DArray, etc.
    Buffer_SRV,               // StructuredBuffer, ByteAddressBuffer
    Texture_UAV,              // RWTexture2D, RWTexture3D
    Buffer_UAV,               // RWStructuredBuffer, RWByteAddressBuffer
    ConstantBuffer,           // Static constant buffer (pre-allocated IBuffer)
    VolatileCBV,              // Dynamic constant buffer (per-draw, ring allocated)
    PushConstants,            // Small inline data (DX12: root constants, Vulkan: push constants)
    Sampler,                  // SamplerState
    AccelerationStructure     // RaytracingAccelerationStructure (TLAS)
};

//--------------------------------------------------
// BindingLayoutItem - Schema for one binding slot
// Use static factory methods for clean, readable API
//--------------------------------------------------
struct BindingLayoutItem {
    EDescriptorType type;
    uint32_t slot;
    uint32_t count = 1;       // Array size
    uint32_t size = 0;        // For VolatileCBV/PushConstants: data size in bytes
    EShaderStage stages = EShaderStage::All;

    // Static factory methods - NVRHI-style clean API
    static BindingLayoutItem Texture_SRV(uint32_t slot);
    static BindingLayoutItem Texture_SRVArray(uint32_t slot, uint32_t count);
    static BindingLayoutItem Buffer_SRV(uint32_t slot);
    static BindingLayoutItem Texture_UAV(uint32_t slot);
    static BindingLayoutItem Buffer_UAV(uint32_t slot);
    static BindingLayoutItem ConstantBuffer(uint32_t slot);
    static BindingLayoutItem VolatileCBV(uint32_t slot, uint32_t size);
    static BindingLayoutItem PushConstants(uint32_t slot, uint32_t size);
    static BindingLayoutItem Sampler(uint32_t slot);
    static BindingLayoutItem AccelerationStructure(uint32_t slot);
};

//--------------------------------------------------
// BindingLayoutDesc - Fluent builder for layout
//--------------------------------------------------
class BindingLayoutDesc {
public:
    explicit BindingLayoutDesc(const char* debugName = nullptr);

    // Fluent API - chain AddItem() calls
    BindingLayoutDesc& AddItem(const BindingLayoutItem& item);
    BindingLayoutDesc& SetVisibility(EShaderStage stages);  // Default for subsequent items

    // Getters
    const std::vector<BindingLayoutItem>& GetItems() const;
    const char* GetDebugName() const;

private:
    std::vector<BindingLayoutItem> m_items;
    const char* m_debugName = nullptr;
    EShaderStage m_defaultVisibility = EShaderStage::All;
};

//--------------------------------------------------
// IDescriptorSetLayout (Immutable, cached)
//--------------------------------------------------
class IDescriptorSetLayout {
public:
    virtual ~IDescriptorSetLayout() = default;
    virtual uint32_t GetBindingCount() const = 0;
    virtual const BindingLayoutItem& GetBinding(uint32_t index) const = 0;
    virtual const char* GetDebugName() const = 0;
};

//--------------------------------------------------
// BindingSetItem - Actual resource binding
// Use static factory methods mirroring BindingLayoutItem
//--------------------------------------------------
struct BindingSetItem {
    uint32_t slot;
    EDescriptorType type;

    // Resource union (only one valid based on type)
    ITexture* texture = nullptr;
    IBuffer* buffer = nullptr;
    ISampler* sampler = nullptr;
    IAccelerationStructure* accelStruct = nullptr;
    const void* volatileData = nullptr;
    uint32_t volatileDataSize = 0;
    uint32_t mipLevel = 0;  // For UAV mip binding

    // Static factory methods - mirror BindingLayoutItem naming
    static BindingSetItem Texture_SRV(uint32_t slot, ITexture* tex);
    static BindingSetItem Texture_SRVSlice(uint32_t slot, ITexture* tex, uint32_t arraySlice);
    static BindingSetItem Buffer_SRV(uint32_t slot, IBuffer* buf);
    static BindingSetItem Texture_UAV(uint32_t slot, ITexture* tex, uint32_t mip = 0);
    static BindingSetItem Buffer_UAV(uint32_t slot, IBuffer* buf);
    static BindingSetItem ConstantBuffer(uint32_t slot, IBuffer* buf);
    static BindingSetItem VolatileCBV(uint32_t slot, const void* data, uint32_t size);
    static BindingSetItem PushConstants(uint32_t slot, const void* data, uint32_t size);
    static BindingSetItem Sampler(uint32_t slot, ISampler* samp);
    static BindingSetItem AccelerationStructure(uint32_t slot, IAccelerationStructure* as);
};

//--------------------------------------------------
// IDescriptorSet - Mutable resource bindings
//--------------------------------------------------
class IDescriptorSet {
public:
    virtual ~IDescriptorSet() = default;

    // Bind single resource
    virtual void Bind(const BindingSetItem& item) = 0;

    // Bind multiple resources at once
    virtual void Bind(const BindingSetItem* items, uint32_t count) = 0;

    // Convenience: bind initializer list
    void Bind(std::initializer_list<BindingSetItem> items) {
        Bind(items.begin(), static_cast<uint32_t>(items.size()));
    }

    virtual IDescriptorSetLayout* GetLayout() const = 0;
    virtual bool IsComplete() const = 0;
};

//--------------------------------------------------
// IDescriptorSetAllocator
//--------------------------------------------------
class IDescriptorSetAllocator {
public:
    virtual ~IDescriptorSetAllocator() = default;

    // Create layout - each call creates NEW instance (no caching)
    // User must manage layout lifetime and share instances explicitly
    // STRICT: Set's layout pointer must equal pipeline's expected layout pointer
    virtual IDescriptorSetLayout* CreateLayout(const BindingLayoutDesc& desc) = 0;

    // Allocate sets
    virtual IDescriptorSet* AllocatePersistentSet(IDescriptorSetLayout* layout) = 0;
    virtual IDescriptorSet* AllocateTransientSet(IDescriptorSetLayout* layout) = 0;

    // Free persistent set (transient sets auto-freed at frame end)
    virtual void FreePersistentSet(IDescriptorSet* set) = 0;

    // Frame boundary
    virtual void BeginFrame(uint32_t frameIndex) = 0;
};

} // namespace RHI
```

---

## Binding Type Comparison

| Type | Size Limit | Allocation | DX12 Mapping | Vulkan Mapping |
|------|------------|------------|--------------|----------------|
| ConstantBuffer | 64KB | Pre-allocated IBuffer | Descriptor table CBV | UNIFORM_BUFFER |
| VolatileCBV | 64KB | Ring buffer per-frame | Root CBV (GPU VA) | UNIFORM_BUFFER_DYNAMIC |
| PushConstants | 128-256 bytes | None (inline) | Root Constants | Push Constants |
| Texture_SRV | N/A | Descriptor only | SRV descriptor | SAMPLED_IMAGE |
| Sampler | N/A | Descriptor only | Sampler descriptor | SAMPLER |

---

## ICommandList Additions

```cpp
class ICommandList {
public:
    // ... existing methods ...

    //--------------------------------------------------
    // NEW: Descriptor Set Binding
    //--------------------------------------------------

    // Bind descriptor set to set index (0=PerFrame, 1=PerPass, 2=PerMaterial, 3=PerDraw)
    // All bindings (SRV, UAV, CBV, Sampler, PushConstants) are applied from the set
    // Sets remain bound until replaced
    virtual void BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) = 0;

    //--------------------------------------------------
    // DEPRECATED (retained for migration period)
    //--------------------------------------------------
    virtual void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
    virtual void SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) = 0;
};
```

**Note**: `SetInlineConstants()` is removed. Use `IDescriptorSet::SetPushConstants()` instead.

---

## Allocation Strategy

| Frequency | Allocation Type | Lifetime | GPU Heap | Use Case |
|-----------|-----------------|----------|----------|----------|
| PerFrame | **Persistent Pool** | Reused across frames | Copied to staging once | Shadow maps, IBL, BRDF LUT |
| PerPass | **Persistent Pool** | Reused across frames | Copied to staging per-pass | G-Buffer, post-process inputs |
| PerMaterial | **Persistent Pool** | Cached per material | Copied when material changes | Material textures |
| PerDraw | **Transient Ring** | Auto-freed at frame end | Direct staging allocation | Inline via root CBV |

### DX12 Implementation

```cpp
class CDX12DescriptorSetAllocator : public IDescriptorSetAllocator {
private:
    // Persistent sets: Allocated from CPU-only heap, copied to staging when bound
    struct PersistentPool {
        CDX12DescriptorHeap cpuHeap;      // CPU-only, persistent storage
        std::vector<bool> allocated;
        std::stack<uint32_t> freeList;
    };
    std::unordered_map<IDescriptorSetLayout*, PersistentPool> m_persistentPools;

    // Transient sets: Allocated directly from staging ring
    CDX12DescriptorStagingRing& m_stagingRing;  // Existing infrastructure

public:
    IDescriptorSet* AllocatePersistentSet(IDescriptorSetLayout* layout) override {
        // Allocate from CPU heap, return handle
    }

    IDescriptorSet* AllocateTransientSet(IDescriptorSetLayout* layout) override {
        // Allocate from staging ring (per-frame)
    }
};
```

### Vulkan Implementation

```cpp
class CVulkanDescriptorSetAllocator : public IDescriptorSetAllocator {
private:
    // Pool per layout type
    struct PoolInfo {
        VkDescriptorPool pool;
        uint32_t allocatedCount;
        uint32_t maxSets;
    };
    std::unordered_map<IDescriptorSetLayout*, std::vector<PoolInfo>> m_pools;

    // Per-frame transient pools (reset each frame)
    VkDescriptorPool m_transientPools[MAX_FRAMES_IN_FLIGHT];

public:
    IDescriptorSet* AllocatePersistentSet(IDescriptorSetLayout* layout) override {
        // vkAllocateDescriptorSets from persistent pool
    }

    IDescriptorSet* AllocateTransientSet(IDescriptorSetLayout* layout) override {
        // vkAllocateDescriptorSets from per-frame pool
    }

    void BeginFrame(uint32_t frameIndex) override {
        // vkResetDescriptorPool for transient pool
    }
};
```

---

## File Structure

```
RHI/
├── IDescriptorSet.h           # Core interfaces (Layout, Set, Allocator)
├── IPipelineLayout.h          # NEW: Pipeline layout interface
├── ICommandList.h             # Add BindDescriptorSet()
├── IRenderContext.h           # Add GetDescriptorSetAllocator(), CreatePipelineLayout()
├── RHIDescriptors.h           # MODIFY: Add setLayouts to SPipelineStateDesc
├── DX12/
│   ├── DX12DescriptorSet.h    # CDX12DescriptorSetLayout, CDX12DescriptorSet
│   ├── DX12DescriptorSet.cpp
│   ├── DX12RootSignatureCache.h   # NEW: Root signature caching
│   ├── DX12RootSignatureCache.cpp
│   ├── DX12PipelineState.h    # MODIFY: Store per-set binding info
│   ├── DX12DescriptorHeap.h   # Pool allocation support
│   └── DX12CommandList.cpp    # BindDescriptorSet() with pipeline context
└── Vulkan/                    # Phase 3.8
    ├── VulkanDescriptorSet.h
    ├── VulkanDescriptorSet.cpp
    └── VulkanPipelineLayoutCache.h

Core/RDG/
├── RDGContext.h               # Descriptor set integration
└── RDGDescriptorCache.h       # Per-pass caching
```

---

## Usage Examples

### Example 1: PerFrame Set (Global Resources)

```cpp
// Layout definition using NVRHI-style static factories
auto perFrameLayout = allocator->CreateLayout(
    BindingLayoutDesc("PerFrame")
        .AddItem(BindingLayoutItem::Texture_SRV(0))    // t0: Shadow maps
        .AddItem(BindingLayoutItem::Texture_SRV(1))    // t1: Irradiance
        .AddItem(BindingLayoutItem::Texture_SRV(2))    // t2: Prefiltered
        .AddItem(BindingLayoutItem::Texture_SRV(3))    // t3: BRDF LUT
        .AddItem(BindingLayoutItem::Sampler(0))        // s0: Linear clamp
        .AddItem(BindingLayoutItem::Sampler(1))        // s1: Shadow comparison
        .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_PerFrame)))  // b0
);

// Create persistent set (reused across frames)
auto* perFrameSet = allocator->AllocatePersistentSet(perFrameLayout);

// Bind resources using BindingSetItem factories
perFrameSet->Bind({
    BindingSetItem::Texture_SRV(0, shadowMapArray),
    BindingSetItem::Texture_SRV(1, irradianceArray),
    BindingSetItem::Texture_SRV(2, prefilteredArray),
    BindingSetItem::Texture_SRV(3, brdfLUT),
    BindingSetItem::Sampler(0, linearClampSampler),
    BindingSetItem::Sampler(1, shadowComparisonSampler),
});

// Update volatile CBV each frame
perFrameSet->Bind(BindingSetItem::VolatileCBV(0, &cbPerFrame, sizeof(CB_PerFrame)));

// Bind once per frame
cmdList->BindDescriptorSet(0, perFrameSet);
```

**Shader (HLSL):**
```hlsl
// PerFrame resources in space0
Texture2DArray gShadowMaps         : register(t0, space0);
TextureCubeArray gIrradianceArray  : register(t1, space0);
TextureCubeArray gPrefilteredArray : register(t2, space0);
Texture2D gBrdfLUT                 : register(t3, space0);
SamplerState gLinearClamp          : register(s0, space0);
SamplerComparisonState gShadowSamp : register(s1, space0);
cbuffer CB_PerFrame                : register(b0, space0) {
    float4x4 gViewProj;
    float3 gCameraPos;
    float gTime;
};
```

---

### Example 2: PerPass Set (G-Buffer / Post-Process)

```cpp
// Layout for deferred lighting pass
auto deferredPassLayout = allocator->CreateLayout(
    BindingLayoutDesc("DeferredPass")
        .AddItem(BindingLayoutItem::Texture_SRV(0))    // t0: G-Buffer Albedo
        .AddItem(BindingLayoutItem::Texture_SRV(1))    // t1: G-Buffer Normal
        .AddItem(BindingLayoutItem::Texture_SRV(2))    // t2: G-Buffer Depth
        .AddItem(BindingLayoutItem::Texture_SRV(3))    // t3: G-Buffer Emissive
        .AddItem(BindingLayoutItem::Sampler(0))        // s0: Point clamp
);

// Create and populate set
auto* deferredPassSet = allocator->AllocatePersistentSet(deferredPassLayout);
deferredPassSet->Bind({
    BindingSetItem::Texture_SRV(0, gBufferAlbedo),
    BindingSetItem::Texture_SRV(1, gBufferNormal),
    BindingSetItem::Texture_SRV(2, gBufferDepth),
    BindingSetItem::Texture_SRV(3, gBufferEmissive),
    BindingSetItem::Sampler(0, pointClampSampler),
});

// Bind for deferred lighting pass
cmdList->BindDescriptorSet(1, deferredPassSet);
```

---

### Example 3: PerMaterial Set (PBR Material)

```cpp
// Layout for PBR material
auto materialLayout = allocator->CreateLayout(
    BindingLayoutDesc("PBRMaterial")
        .AddItem(BindingLayoutItem::Texture_SRV(0))    // t0: Albedo
        .AddItem(BindingLayoutItem::Texture_SRV(1))    // t1: Normal
        .AddItem(BindingLayoutItem::Texture_SRV(2))    // t2: MetallicRoughness
        .AddItem(BindingLayoutItem::Texture_SRV(3))    // t3: Emissive
        .AddItem(BindingLayoutItem::Texture_SRV(4))    // t4: AO
        .AddItem(BindingLayoutItem::Sampler(0))        // s0: Material sampler
        .AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_Material)))  // b0
);

// Material class with descriptor set
class CMaterial {
    IDescriptorSet* m_descriptorSet;

public:
    void Initialize(IDescriptorSetAllocator* allocator, IDescriptorSetLayout* layout) {
        m_descriptorSet = allocator->AllocatePersistentSet(layout);
        m_descriptorSet->Bind({
            BindingSetItem::Texture_SRV(0, m_albedoTex),
            BindingSetItem::Texture_SRV(1, m_normalTex),
            BindingSetItem::Texture_SRV(2, m_metallicRoughnessTex),
            BindingSetItem::Texture_SRV(3, m_emissiveTex),
            BindingSetItem::Texture_SRV(4, m_aoTex),
            BindingSetItem::Sampler(0, m_sampler),
        });
    }

    void Bind(ICommandList* cmdList, const CB_Material& props) {
        // Update volatile CBV with current material properties
        m_descriptorSet->Bind(BindingSetItem::VolatileCBV(0, &props, sizeof(props)));
        cmdList->BindDescriptorSet(2, m_descriptorSet);
    }
};
```

---

### Example 4: PerDraw Set (Object Transforms)

```cpp
// Layout for per-object data using push constants (small, fast)
auto perDrawLayout = allocator->CreateLayout(
    BindingLayoutDesc("PerDraw")
        .AddItem(BindingLayoutItem::PushConstants(0, sizeof(CB_PerDraw)))
);

// Transient set - allocated each draw, auto-freed at frame end
void RenderObject(ICommandList* cmdList, IDescriptorSetAllocator* allocator,
                  const CB_PerDraw& objectData) {
    auto* perDrawSet = allocator->AllocateTransientSet(perDrawLayout);
    perDrawSet->Bind(BindingSetItem::PushConstants(0, &objectData, sizeof(objectData)));
    cmdList->BindDescriptorSet(3, perDrawSet);
    cmdList->DrawIndexed(mesh->indexCount);
}
```

---

### Example 5: Complete Rendering Loop

```cpp
void SceneRenderer::Render(ICommandList* cmdList) {
    // 1. Set pipeline and bind PerFrame set (once)
    cmdList->SetPipelineState(m_mainPassPSO);
    m_perFrameSet->Bind(BindingSetItem::VolatileCBV(0, &m_cbPerFrame, sizeof(CB_PerFrame)));
    cmdList->BindDescriptorSet(0, m_perFrameSet);

    // 2. For each render pass
    for (auto& pass : m_passes) {
        if (pass.passSet) {
            cmdList->BindDescriptorSet(1, pass.passSet);
        }

        // 3. For each material in pass
        for (auto& batch : pass.batches) {
            batch.material->Bind(cmdList);  // BindDescriptorSet(2, ...)

            // 4. For each object with this material
            for (auto& object : batch.objects) {
                CB_PerDraw perDraw = {object.worldMatrix, object.objectID};
                auto* drawSet = m_allocator->AllocateTransientSet(m_perDrawLayout);
                drawSet->Bind(BindingSetItem::PushConstants(0, &perDraw, sizeof(perDraw)));
                cmdList->BindDescriptorSet(3, drawSet);
                cmdList->DrawIndexed(object.mesh->indexCount);
            }
        }
    }
}
```

---

### Phase A: Parallel Infrastructure
1. Add new interfaces alongside existing code
2. Create new Root Signature (keep old one working)
3. Both systems coexist

### Phase B: Shader Migration
1. Update shaders to use `space0/1/2` registers
2. Keep old register mapping via `#ifdef` for transition
3. Validate with single test pass

### Phase C: Pass-by-Pass Adoption
1. Migrate one render pass at a time
2. Start with isolated passes (e.g., BloomPass)
3. Gradually migrate main rendering loop

### Phase D: Cleanup
1. Remove old slot-based binding code
2. Remove old Root Signature
3. Remove deprecated `SetShaderResource()` methods

---

## Critical Files to Modify

| File | Changes |
|------|---------|
| `RHI\IDescriptorSet.h` | NEW - Core interfaces (Layout, Set, Allocator) |
| `RHI\IPipelineLayout.h` | NEW - Pipeline layout interface |
| `RHI\ICommandList.h` | Add `BindDescriptorSet()` |
| `RHI\IRenderContext.h` | Add `GetDescriptorSetAllocator()` |
| `RHI\RHIDescriptors.h` | Add `setLayouts[4]` to `SPipelineStateDesc` |
| `RHI\DX12\DX12DescriptorSet.h/cpp` | NEW - DX12 descriptor set implementation |
| `RHI\DX12\DX12RootSignatureCache.h/cpp` | NEW - Root signature caching |
| `RHI\DX12\DX12PipelineState.h/cpp` | Store per-set binding info, use cached root signature |
| `RHI\DX12\DX12DescriptorHeap.h` | Pool allocation support |
| `RHI\DX12\DX12CommandList.cpp` | `BindDescriptorSet()` with pipeline context |
| `Shader\*.hlsl` | Update register assignments to use spaces |
| `Core\RDG\RDGContext.h` | Descriptor set integration |

---

## Vulkan Mapping Reference

| EDescriptorType | DX12 Implementation | Vulkan Implementation |
|-----------------|---------------------|----------------------|
| SRV_Texture | SRV descriptor in table | `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` |
| SRV_Buffer | SRV descriptor in table | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` (read-only) |
| UAV_Texture | UAV descriptor in table | `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` |
| UAV_Buffer | UAV descriptor in table | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` |
| CBV | Root CBV (GPU VA) | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` |
| VolatileCBV | Root CBV + ring buffer | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` |
| PushConstants | Root Constants (128 bytes max) | Push Constants (128-256 bytes) |
| Sampler | Sampler descriptor in table | `VK_DESCRIPTOR_TYPE_SAMPLER` |
| AccelerationStructure | SRV (TLAS) | `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` |

### DX12 vs Vulkan Concepts

| DX12 Concept | Vulkan Equivalent |
|--------------|-------------------|
| Root Signature | VkPipelineLayout |
| Descriptor Table | VkDescriptorSet |
| Root Parameter | Descriptor Set Binding |
| register(t0, space0) | layout(set=0, binding=0) |
| register(s0, space1) | layout(set=1, binding=0) sampler |
| Root CBV (GPU VA) | Dynamic Uniform Buffer |
| Root Constants | Push Constants |
| Descriptor Heap | VkDescriptorPool |

---

## Known Issues & Solutions

### Issue: D3D12 GPU-Based Validation Errors for Resource States

**Problem**: When using descriptor sets with GPU-based validation enabled, D3D12 reports errors about resources being in incorrect states when bound to the descriptor set.

**Error Examples**:
```
D3D12 ERROR: Resource GBuffer_AlbedoAO is in RENDER_TARGET state, expected SHADER_RESOURCE
D3D12 ERROR: Resource ShadowMapArray is in DEPTH_WRITE state, expected SHADER_RESOURCE
D3D12 ERROR: Resource CompactLightListBuffer is in UNORDERED_ACCESS state, expected SHADER_RESOURCE
```

**Root Cause**: Descriptor sets bind resources at the time `BindDescriptorSet()` is called. If resources haven't been transitioned to the correct state by their producer passes, GPU-based validation fails.

**Solution: Producer Pass Barrier Pattern**

Each pass that produces resources (writes to render targets, UAVs, depth buffers) must transition its outputs to `ShaderResource` state at the **end** of its `Render()` method, before returning control to the caller.

**Implementation**:

1. **GBufferPass** (end of `Render()`):
```cpp
// Unbind render targets before transitioning
cmdList->SetRenderTargets(0, nullptr, nullptr);

// Transition G-Buffer textures from RenderTarget to ShaderResource for consumers
cmdList->Barrier(gbuffer.GetAlbedoAO(), EResourceState::RenderTarget, EResourceState::ShaderResource);
cmdList->Barrier(gbuffer.GetNormalRoughness(), EResourceState::RenderTarget, EResourceState::ShaderResource);
cmdList->Barrier(gbuffer.GetWorldPosMetallic(), EResourceState::RenderTarget, EResourceState::ShaderResource);
cmdList->Barrier(gbuffer.GetEmissiveMaterialID(), EResourceState::RenderTarget, EResourceState::ShaderResource);
cmdList->Barrier(gbuffer.GetVelocity(), EResourceState::RenderTarget, EResourceState::ShaderResource);
cmdList->Barrier(gbuffer.GetDepthBuffer(), EResourceState::DepthWrite, EResourceState::ShaderResource);
```

2. **ShadowPass** (end of `Render()`):
```cpp
// Unbind DSV to allow reading as SRV in MainPass
cmdList->SetRenderTargets(0, nullptr, nullptr);

// Transition shadow map to SRV state for consumers
cmdList->Barrier(m_shadowMapArray.get(), EResourceState::DepthWrite, EResourceState::ShaderResource);
```

3. **ClusteredLightingPass** (end of `CullLights()`):
```cpp
// Unbind UAVs
cmdList->SetUnorderedAccess(0, nullptr);
cmdList->SetUnorderedAccess(1, nullptr);
cmdList->SetUnorderedAccess(2, nullptr);
cmdList->UnbindShaderResources(EShaderStage::Compute, 0, 2);

// Transition buffers from UAV to SRV for consumers (deferred lighting pass)
cmdList->Barrier(m_clusterDataBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
cmdList->Barrier(m_compactLightListBuffer.get(), EResourceState::UnorderedAccess, EResourceState::ShaderResource);
```

**Benefits of Producer Pattern**:
- Clear ownership: each pass manages its own resource states
- No scattered barrier logic in consumer passes
- Consumer passes (e.g., DeferredLightingPass) can assume resources are already in SRV state
- Works correctly with descriptor set binding

**Alternative Approaches Considered**:
- **Consumer barriers**: Adding barriers before `BindDescriptorSet()` - rejected because it scatters responsibility
- **Descriptor set tracks resources**: Complex, requires RDG-like tracking
- **RDG integration**: Future solution, but too complex for initial implementation

---

### Issue: Dimension Mismatch for VolumetricLightmap

**Problem**: When VolumetricLightmap is disabled, the fallback texture was 2D (`GetDefaultBlack()`), but shaders expected Texture3D at slots t8-t11.

**Error**: `D3D12 ERROR: Dimension mismatch - expected Texture3D, got Texture2D`

**Solution**: Added `GetDefaultBlack3D()` to TextureManager that returns a 1x1x1 3D black texture. VolumetricLightmap::PopulatePerFrameSet() now uses this as fallback.

```cpp
// TextureManager.h
RHI::TexturePtr GetDefaultBlack3D() const { return m_defaultBlack3D; }

// VolumetricLightmap.cpp - PopulatePerFrameSet()
if (!IsEnabled() || !m_shCoeffsR) {
    auto& texMgr = CTextureManager::Instance();
    perFrameSet->Bind({
        BindingSetItem::Texture_SRV(Tex::Volumetric_SH_R, texMgr.GetDefaultBlack3D().get()),
        BindingSetItem::Texture_SRV(Tex::Volumetric_SH_G, texMgr.GetDefaultBlack3D().get()),
        BindingSetItem::Texture_SRV(Tex::Volumetric_SH_B, texMgr.GetDefaultBlack3D().get()),
        BindingSetItem::Texture_SRV(Tex::Volumetric_Octree, texMgr.GetDefaultBlack3D().get()),
    });
    return;
}
```

---

## Verification

1. **TestDescriptorSet**: Create layout, allocate set, bind resources, render triangle
2. **TestDescriptorSetPerformance**: Compare per-draw overhead vs old system
3. **Regression**: All existing render passes work after migration
4. **Proof-of-concept**: BloomPass fully converted to descriptor sets
