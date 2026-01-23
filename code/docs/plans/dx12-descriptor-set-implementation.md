# DX12 Descriptor Set Implementation Plan

## Overview

Implement descriptor set abstraction for DX12 as defined in `docs/plans/descriptor-set-abstraction.md`. This plan focuses ONLY on DX12 implementation (Vulkan is out of scope).

**Key Principle**: Add new feature while keeping old binding system. Migrate incrementally.

---

## Design Decisions

| Decision | Choice | Notes |
|----------|--------|-------|
| DX11 Support | Out of scope | Descriptor sets are DX12/Vulkan only |
| Compute Shaders | Same system | Use `SetComputeRoot*` instead of `SetGraphicsRoot*` |
| Staging Overflow | Assert/crash | Indicates undersized ring - fix by increasing size |
| Shader Validation | Debug-only | Use shader reflection in debug builds, strip in release |
| Slot Binding | Contiguous only | Slots t0,t1,t2... must have no gaps |
| PerFrame Layout | Per-pass defined | Each pass defines own Set0 layout (not global shared) |
| Root Param Limit | Validate and warn | Check root signature < 64 DWORDs, warn if approaching |
| API Style | Methods on IRenderContext | Hide IDescriptorSetAllocator as implementation detail |

---

## Migration Roadmap

| Step | Description | Risk |
|------|-------------|------|
| 1 | Add new descriptor set infrastructure (keep old system working) | Low |
| 2 | Write TestDescriptorSet to validate new system | Low |
| 3 | Migrate PostProcess passes (BloomPass, TonemapPass, etc.) | Medium |
| 4 | Migrate DeferredLightingPass | Medium |
| 5 | Migrate all remaining passes, remove legacy code | High |

---

## Design Summary

**Goal**: Replace per-slot binding (`SetShaderResource`, `SetConstantBufferData`) with set-based binding (`BindDescriptorSet`) using register spaces.

**Frequency Model (4 sets)**:
- Set 0 (space0): PerFrame - shadow maps, IBL, BRDF LUT
- Set 1 (space1): PerPass - G-Buffer, post-process inputs
- Set 2 (space2): PerMaterial - material textures
- Set 3 (space3): PerDraw - object transforms (push constants)

**DX12 Mapping**:
| Type | DX12 Implementation |
|------|---------------------|
| Texture_SRV | SRV descriptor in table |
| Buffer_SRV | SRV descriptor in table |
| Texture_UAV | UAV descriptor in table |
| Buffer_UAV | UAV descriptor in table |
| ConstantBuffer | Root CBV (pre-allocated IBuffer) |
| VolatileCBV | Root CBV + ring buffer allocation |
| PushConstants | Root Constants (32-bit values) |
| Sampler | Sampler descriptor in table |

---

## Implementation Phases

### Phase 1: Core Interfaces (RHI Layer)

**Files to create/modify**:

1. **NEW: `RHI/IDescriptorSet.h`** - Core interfaces
   ```cpp
   namespace RHI {
       enum class EDescriptorType : uint8_t { ... };
       struct BindingLayoutItem { ... };  // Static factory methods
       class BindingLayoutDesc { ... };   // Fluent builder
       struct BindingSetItem { ... };     // Static factory methods
       class IDescriptorSetLayout { ... };
       class IDescriptorSet { ... };
       class IDescriptorSetAllocator { ... };
   }
   ```

2. **MODIFY: `RHI/RHIDescriptors.h`** - Add to PipelineStateDesc
   ```cpp
   struct PipelineStateDesc {
       // ... existing fields ...

       // NEW: Descriptor set layouts (nullptr = not used)
       class IDescriptorSetLayout* setLayouts[4] = {nullptr, nullptr, nullptr, nullptr};
   };
   ```

3. **MODIFY: `RHI/ICommandList.h`** - Add virtual method
   ```cpp
   virtual void BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) = 0;
   ```

4. **MODIFY: `RHI/IRenderContext.h`** - Add descriptor set methods
   ```cpp
   // Descriptor Set API (DX12/Vulkan only, returns nullptr on DX11)
   virtual IDescriptorSetLayout* CreateDescriptorSetLayout(const BindingLayoutDesc& desc) = 0;
   virtual void DestroyDescriptorSetLayout(IDescriptorSetLayout* layout) = 0;
   virtual IDescriptorSet* AllocateDescriptorSet(IDescriptorSetLayout* layout) = 0;
   virtual void FreeDescriptorSet(IDescriptorSet* set) = 0;
   ```

   **Usage (clean API)**:
   ```cpp
   // Before refactoring (confusing - exposed allocator)
   auto* allocator = renderCtx->GetDescriptorSetAllocator();
   auto* layout = allocator->CreateLayout(...);
   auto* set = allocator->AllocateSet(layout);

   // After refactoring (clean - methods on context)
   auto* layout = renderCtx->CreateDescriptorSetLayout(...);
   auto* set = renderCtx->AllocateDescriptorSet(layout);
   ```

### Phase 2: DX12 Layout & Set Implementation

**Files to create**:

1. **NEW: `RHI/DX12/DX12DescriptorSet.h`**
   ```cpp
   namespace RHI::DX12 {
       // Per-set root parameter mapping
       struct SSetRootParamInfo {
           uint32_t pushConstantRootParam = UINT32_MAX;
           uint32_t volatileCBVRootParam = UINT32_MAX;
           uint32_t srvTableRootParam = UINT32_MAX;
           uint32_t uavTableRootParam = UINT32_MAX;
           uint32_t samplerTableRootParam = UINT32_MAX;
           uint32_t srvCount = 0;
           uint32_t uavCount = 0;
           uint32_t samplerCount = 0;
           uint32_t pushConstantDwordCount = 0;
           bool isUsed = false;
       };

       class CDX12DescriptorSetLayout : public IDescriptorSetLayout {
           // Stores binding schema, computes descriptor counts
           // Provides PopulateDescriptorRanges() for root sig construction
       };

       class CDX12DescriptorSet : public IDescriptorSet {
           // Stores CPU handles for SRVs/UAVs/Samplers
           // Stores GPU VA for CBVs, data for push constants
           // CopySRVsToStaging(), CopySamplersToStaging() methods
       };
   }
   ```

2. **NEW: `RHI/DX12/DX12DescriptorSet.cpp`** - Implementation

### Phase 3: Root Signature Cache

**Files to create**:

1. **NEW: `RHI/DX12/DX12RootSignatureCache.h`**
   ```cpp
   class CDX12RootSignatureCache {
   public:
       static CDX12RootSignatureCache& Instance();

       struct RootSignatureResult {
           ID3D12RootSignature* rootSignature;
           SSetRootParamInfo setBindings[4];
       };

       RootSignatureResult GetOrCreate(IDescriptorSetLayout* const layouts[4]);

   private:
       // Cache key: layout pointer combination
       // Cache value: root signature + binding info
       std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> m_cache;
   };
   ```

2. **NEW: `RHI/DX12/DX12RootSignatureCache.cpp`**

   **Root Signature Construction Algorithm**:
   - For each set with layouts[i] != nullptr:
     - Add Push Constants param if `layout->HasPushConstants()`
     - Add Root CBV param if `layout->HasVolatileCBV()`
     - Add SRV Table param if `layout->GetSRVCount() > 0`
     - Add UAV Table param if `layout->GetUAVCount() > 0`
     - Add Sampler Table param if `layout->GetSamplerCount() > 0`
   - All ranges use `RegisterSpace = setIndex`

### Phase 4: Allocator Implementation

**Files to create**:

1. **NEW: `RHI/DX12/DX12DescriptorSetAllocator.h`**
   ```cpp
   class CDX12DescriptorSetAllocator : public IDescriptorSetAllocator {
       // Persistent pools: per-layout, free-list based
       // Transient sets: per-frame, auto-freed
       // Uses existing CDX12DynamicBufferRing for VolatileCBV
   };
   ```

2. **NEW: `RHI/DX12/DX12DescriptorSetAllocator.cpp`**

### Phase 5: Integration

**Files to modify**:

1. **MODIFY: `RHI/DX12/DX12Resources.h`** - Extend CDX12PipelineState
   ```cpp
   class CDX12PipelineState : public IPipelineState {
       // ... existing members ...

       // NEW: Descriptor set support
       bool m_usesDescriptorSets = false;
       SSetRootParamInfo m_setBindings[4];
       IDescriptorSetLayout* m_expectedLayouts[4] = {nullptr};

   public:
       // NEW methods
       const SSetRootParamInfo& GetSetBindingInfo(uint32_t setIndex) const;
       IDescriptorSetLayout* GetExpectedLayout(uint32_t setIndex) const;
       bool UsesDescriptorSets() const { return m_usesDescriptorSets; }
   };
   ```

2. **MODIFY: `RHI/DX12/DX12RenderContext.cpp`** - Pipeline creation
   ```cpp
   IPipelineState* CDX12RenderContext::CreatePipelineState(const PipelineStateDesc& desc) {
       bool usesDescriptorSets = (desc.setLayouts[0] || desc.setLayouts[1] ||
                                   desc.setLayouts[2] || desc.setLayouts[3]);

       if (usesDescriptorSets) {
           auto result = CDX12RootSignatureCache::Instance().GetOrCreate(desc.setLayouts);
           // Create PSO with cached root signature
           return new CDX12PipelineState(pso, result.rootSignature, result.setBindings,
                                         desc.setLayouts, false);
       } else {
           // Legacy path: use global root signature
           return new CDX12PipelineState(pso, m_graphicsRootSignature.Get(), false);
       }
   }
   ```

3. **MODIFY: `RHI/DX12/DX12CommandList.h`** - Add member
   ```cpp
   IDescriptorSet* m_boundSets[4] = {nullptr};
   ```

4. **MODIFY: `RHI/DX12/DX12CommandList.cpp`** - Add BindDescriptorSet
   ```cpp
   void CDX12CommandList::BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) {
       // 1. Verify PSO uses descriptor sets
       // 2. Verify layout pointer equality
       // 3. Allocate VolatileCBV from ring buffer if pending
       // 4. Copy SRVs to staging ring, SetGraphicsRootDescriptorTable()
       // 5. Copy Samplers to staging ring, SetGraphicsRootDescriptorTable()
       // 6. Bind root CBV with SetGraphicsRootConstantBufferView()
       // 7. Bind push constants with SetGraphicsRoot32BitConstants()
   }
   ```

### Phase 6: Shader Model 5.1 Upgrade

**Requirement**: Register spaces (`register(t0, space0)`) require HLSL Shader Model 5.1+.

**Files to modify**:

1. **MODIFY: Shader compilation (CMakeLists.txt or build scripts)**
   - Change shader target from `vs_5_0`/`ps_5_0` to `vs_5_1`/`ps_5_1`
   - Example fxc flags: `/T vs_5_1 /T ps_5_1`

2. **MODIFY: Shaders using descriptor sets**
   ```hlsl
   // Before (SM 5.0 - no spaces)
   Texture2D gAlbedo : register(t0);
   cbuffer CB_PerFrame : register(b0) { ... };

   // After (SM 5.1 - with spaces)
   // Set 0: PerFrame
   Texture2DArray gShadowMaps : register(t0, space0);
   cbuffer CB_PerFrame : register(b0, space0) { ... };

   // Set 2: PerMaterial
   Texture2D gAlbedo : register(t0, space2);
   Texture2D gNormal : register(t1, space2);
   ```

**Migration Strategy**:
- Use `#ifdef USE_DESCRIPTOR_SETS` for conditional compilation
- Migrate shaders incrementally (one pass at a time)
- Keep both SM 5.0 and SM 5.1 paths during transition

**Shader Files to Update (for first test)**:
- Create new test shader: `Shader/TestDescriptorSet.hlsl` with SM 5.1 + spaces

### Phase 7: Testing

1. **NEW: `Tests/TestDescriptorSet.cpp`**
   - Create layout with SRVs + Samplers + VolatileCBV
   - Allocate persistent set
   - Bind resources
   - Create PSO with layout
   - Render triangle
   - Validate output

2. **NEW: `Shader/TestDescriptorSet.hlsl`**
   - SM 5.1 shader with register spaces
   - Simple textured quad rendering

---

## Critical Files Summary

| File | Action | Purpose |
|------|--------|---------|
| `RHI/IDescriptorSet.h` | NEW | Core interfaces |
| `RHI/RHIDescriptors.h` | MODIFY | Add setLayouts[4] to PipelineStateDesc |
| `RHI/ICommandList.h` | MODIFY | Add BindDescriptorSet() |
| `RHI/IRenderContext.h` | MODIFY | Add Create/Destroy/Allocate/Free methods |
| `RHI/DX12/DX12DescriptorSet.h` | NEW | DX12 layout and set classes |
| `RHI/DX12/DX12DescriptorSet.cpp` | NEW | Implementation |
| `RHI/DX12/DX12RootSignatureCache.h` | NEW | Root signature caching |
| `RHI/DX12/DX12RootSignatureCache.cpp` | NEW | Root signature construction |
| `RHI/DX12/DX12DescriptorSetAllocator.h` | NEW | Set allocation |
| `RHI/DX12/DX12DescriptorSetAllocator.cpp` | NEW | Pool/transient allocation |
| `RHI/DX12/DX12Resources.h` | MODIFY | Extend CDX12PipelineState |
| `RHI/DX12/DX12RenderContext.cpp` | MODIFY | Pipeline creation with layouts |
| `RHI/DX12/DX12CommandList.h` | MODIFY | Add m_boundSets[4] |
| `RHI/DX12/DX12CommandList.cpp` | MODIFY | BindDescriptorSet() implementation |
| `Shader/TestDescriptorSet.hlsl` | NEW | SM 5.1 test shader with register spaces |
| `Tests/TestDescriptorSet.cpp` | NEW | Validation test |

---

## Critical Implementation Details

### 1. VolatileCBV Ring Buffer Allocation
- Data stored in `CDX12DescriptorSet` at `Bind()` time
- Actual ring buffer allocation happens in `BindDescriptorSet()`
- GPU VA stored for current frame only

### 2. Layout Pointer Equality
- `set->GetLayout()` must equal `pipeline->GetExpectedLayout(setIndex)`
- Same layout instance for both PSO creation and set allocation
- Assert on mismatch with clear error message

### 3. Coexistence with Legacy Binding
- `CDX12PipelineState::UsesDescriptorSets()` determines path
- Legacy PSOs: use `BindPendingResources()` (unchanged)
- Descriptor set PSOs: use `BindDescriptorSet()` exclusively

### 4. Staging Ring Usage
- `CDX12DescriptorSet::CopySRVsToStaging()` copies CPU handles to contiguous staging block
- Returns GPU handle for `SetGraphicsRootDescriptorTable()`
- Uses existing `CDX12DescriptorStagingRing` infrastructure

### 5. Shader Model 5.1 Requirement
- Register spaces (`space0`, `space1`, etc.) require SM 5.1+
- Compile with `/T vs_5_1`, `/T ps_5_1`, `/T cs_5_1`
- SM 5.0 shaders cannot use descriptor sets (legacy path only)

### 6. Debug-Only Shader Validation
- In debug builds, use D3D12 shader reflection to extract register bindings
- Validate that layout matches shader expectations (slot, type, space)
- Strip validation in release builds for performance
- Store reflection data in `CDX12DescriptorSetLayout` (debug only)

### 7. Compute Shader Support
- Same `IDescriptorSet` interface for compute
- `BindDescriptorSet()` checks `m_isComputePSO` to choose:
  - Graphics: `SetGraphicsRootDescriptorTable()`, `SetGraphicsRootConstantBufferView()`
  - Compute: `SetComputeRootDescriptorTable()`, `SetComputeRootConstantBufferView()`

---

## Migration Steps (Detailed)

### Step 1: Infrastructure (Phases 1-5)
- Implement all descriptor set classes
- Both old and new systems coexist
- No existing code changes behavior

### Step 2: TestDescriptorSet (Phase 6-7)
- Create `Tests/TestDescriptorSet.cpp`
- Create `Shader/TestDescriptorSet.hlsl` (SM 5.1)
- Validate: layout creation, set allocation, binding, rendering
- **Gate**: Must pass before proceeding

### Step 3: Migrate PostProcess Passes
- Target: `BloomPass`, `TonemapPass`, `DOFPass`, `FXAA`
- These are isolated (no shared state with main pass)
- Create SM 5.1 versions of post-process shaders
- Update pass code to use `BindDescriptorSet()`
- Keep old shaders for fallback

### Step 4: Migrate DeferredLightingPass
- More complex: uses multiple texture inputs
- Shares PerFrame resources with other passes
- Define shared `PerFrameLayout` and `PerPassLayout`
- Update `DeferredLightingPass` to use descriptor sets

### Step 5: Migrate All & Cleanup
- Migrate remaining passes (ShadowPass, MainPass, SkyboxPass)
- Remove legacy `SetShaderResource()` calls
- Remove legacy root signature
- Delete old binding code paths

---

## Verification

1. **Build verification**: Compile with descriptor set code paths
2. **Unit test**: `Tests/TestDescriptorSet.cpp` - basic binding and rendering
3. **Visual verification**: Screenshot comparison of test output
4. **Migration test**: Convert BloomPass or simple post-process to descriptor sets
5. **Regression**: Ensure legacy code path still works for unconverted passes

**Test command**:
```bash
cmake --build build --target forfun && timeout 15 build/Debug/forfun.exe --test TestDescriptorSet
```

**Verify**:
- Read `E:/forfun/debug/TestDescriptorSet/runtime.log` for errors
- Read `E:/forfun/debug/TestDescriptorSet/screenshot_frame20.png` for visual output
