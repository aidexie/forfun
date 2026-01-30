# Merge Static-CBV/SRV/UAV Tables into Single Descriptor Table

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce root signature DWORD cost by merging static CBV, SRV, and UAV into a single `CBV_SRV_UAV` descriptor table per set, following NVRHI's `descriptorRangesSRVetc` pattern.

**Architecture:** Currently each set creates up to 4 root parameters: static CBV (2 DWORDs), SRV table (1 DWORD), UAV table (1 DWORD), Sampler table (1 DWORD). Following NVRHI, we merge static CBV + SRV + UAV into one table (1 DWORD total). Volatile CBV stays as root CBV (required for per-frame data). Samplers stay separate (different heap type).

**Tech Stack:** DX12, C++17

---

## DWORD Cost Comparison

| Resource Type | Current | After Refactor |
|---------------|---------|----------------|
| Static CBV | 2 DWORDs (root CBV) | 0 (in table) |
| SRV table | 1 DWORD | 0 (merged) |
| UAV table | 1 DWORD | 0 (merged) |
| Combined CBV_SRV_UAV table | N/A | 1 DWORD |
| **Total per set** | **4 DWORDs** | **1 DWORD** |

Savings: 3 DWORDs per set with all three types, 12 DWORDs max for 4 sets.

---

## Critical Design Decision: Binding Order

**Problem:** Root signature descriptor ranges must match the order of handles in the combined array.

**Solution:** Use **binding declaration order** for both ranges and handles. This matches the existing NVRHI pattern where `PopulateSRVRanges()` iterates bindings in declaration order.

The combined array layout follows binding order, NOT a fixed CBV→SRV→UAV order. We use slot-to-index maps (already exist for SRV/UAV) to translate shader slots to array indices.

---

## Summary of Changes

| File | Change |
|------|--------|
| `RHI/DX12/DX12DescriptorSet.h` | Replace `srvTableRootParam`/`uavTableRootParam`/`constantBufferRootParam` with `cbvSrvUavTableRootParam` |
| `RHI/DX12/DX12DescriptorSet.h` | Add `m_cbvSrvUavHandles` combining static CBV + SRV + UAV handles |
| `RHI/DX12/DX12DescriptorSet.h` | Add `m_cbvSlotToIndex` map for static CBV slot lookup |
| `RHI/DX12/DX12DescriptorSet.cpp` | Add `PopulateCBVSRVUAVRanges()`. Remove separate populate methods. |
| `RHI/DX12/DX12DescriptorSet.cpp` | Update `Bind()` to store static CBV descriptor handle in combined array using slot-to-index map |
| `RHI/DX12/DX12RootSignatureCache.cpp` | Create single combined CBV_SRV_UAV table instead of separate |
| `RHI/DX12/DX12CommandList.cpp` | Bind single combined table instead of separate calls |

---

## Task 1: Update SSetRootParamInfo Structure

**Files:**
- Modify: `RHI/DX12/DX12DescriptorSet.h:27-50`

**Step 1: Replace separate fields with combined**

```cpp
struct SSetRootParamInfo {
    uint32_t pushConstantRootParam = UINT32_MAX;   // Root param index for push constants
    uint32_t cbvSrvUavTableRootParam = UINT32_MAX; // Root param index for combined CBV+SRV+UAV table
    uint32_t samplerTableRootParam = UINT32_MAX;   // Root param index for Sampler table

    // Multiple volatile CBVs support (unchanged - stays as root CBV)
    static constexpr uint32_t MAX_VOLATILE_CBVS = 8;
    uint32_t volatileCBVRootParams[MAX_VOLATILE_CBVS] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    uint32_t volatileCBVSlots[MAX_VOLATILE_CBVS] = {0};
    uint32_t volatileCBVSizes[MAX_VOLATILE_CBVS] = {0};
    uint32_t volatileCBVCount = 0;

    uint32_t cbvSrvUavCount = 0;  // Total static CBV + SRV + UAV descriptor count
    uint32_t samplerCount = 0;
    uint32_t pushConstantDwordCount = 0;

    bool isUsed = false;
};
```

**Removed fields:** `constantBufferRootParam`, `srvTableRootParam`, `uavTableRootParam`, `srvCount`, `uavCount`

**Step 2: Verify compilation fails (expected)**

Run: `cmake --build build --target forfun 2>&1 | head -50`
Expected: Errors on old field names

---

## Task 2: Update CDX12DescriptorSetLayout

**Files:**
- Modify: `RHI/DX12/DX12DescriptorSet.h:58-123`
- Modify: `RHI/DX12/DX12DescriptorSet.cpp:190-344`

**Step 1: Add GetCBVSRVUAVCount() and CBV index lookup**

In header, add after GetSamplerCount():
```cpp
    uint32_t GetCBVSRVUAVCount() const { return (m_hasConstantBuffer ? 1 : 0) + m_srvCount + m_uavCount; }

    // Get combined array index for a slot. Returns false if slot not in layout.
    bool GetCBVIndex(uint32_t slot, uint32_t& outIndex) const;
```

**Step 2: Add m_cbvSlotToIndex member**

In header private section, add:
```cpp
    std::unordered_map<uint32_t, uint32_t> m_cbvSlotToIndex;
```

**Step 3: Replace PopulateSRVRanges/PopulateUAVRanges with PopulateCBVSRVUAVRanges**

In header, replace lines 94-96:
```cpp
    // Populate combined CBV+SRV+UAV descriptor ranges for root signature construction
    // Order follows binding declaration order (matches handle array layout)
    // Returns number of ranges added
    uint32_t PopulateCBVSRVUAVRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const;
```

**Step 4: Update constructor to build combined slot-to-index map**

In cpp, update the constructor's slot mapping section:

```cpp
    // Build slot-to-index mappings for combined CBV+SRV+UAV array
    // Order follows binding declaration order
    uint32_t cbvSrvUavOffset = 0;
    uint32_t samplerOffset = 0;

    for (const auto& binding : m_bindings) {
        switch (binding.type) {
            case EDescriptorType::ConstantBuffer:
                m_cbvSlotToIndex[binding.slot] = cbvSrvUavOffset++;
                break;
            case EDescriptorType::Texture_SRV:
            case EDescriptorType::Buffer_SRV:
            case EDescriptorType::AccelerationStructure:
                for (uint32_t i = 0; i < binding.count; ++i) {
                    m_srvSlotToIndex[binding.slot + i] = cbvSrvUavOffset++;
                }
                break;
            case EDescriptorType::Texture_UAV:
            case EDescriptorType::Buffer_UAV:
                for (uint32_t i = 0; i < binding.count; ++i) {
                    m_uavSlotToIndex[binding.slot + i] = cbvSrvUavOffset++;
                }
                break;
            case EDescriptorType::Sampler:
                for (uint32_t i = 0; i < binding.count; ++i) {
                    m_samplerSlotToIndex[binding.slot + i] = samplerOffset++;
                }
                break;
            default:
                break;
        }
    }
```

**Step 5: Implement GetCBVIndex**

```cpp
bool CDX12DescriptorSetLayout::GetCBVIndex(uint32_t slot, uint32_t& outIndex) const {
    auto it = m_cbvSlotToIndex.find(slot);
    if (it != m_cbvSlotToIndex.end()) {
        outIndex = it->second;
        return true;
    }
    return false;
}
```

**Step 6: Implement PopulateCBVSRVUAVRanges**

```cpp
uint32_t CDX12DescriptorSetLayout::PopulateCBVSRVUAVRanges(D3D12_DESCRIPTOR_RANGE1* ranges, uint32_t registerSpace) const {
    uint32_t rangeCount = 0;

    for (const auto& binding : m_bindings) {
        D3D12_DESCRIPTOR_RANGE_TYPE rangeType;
        D3D12_DESCRIPTOR_RANGE_FLAGS flags;

        switch (binding.type) {
            case EDescriptorType::ConstantBuffer:
                rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
                break;
            case EDescriptorType::Texture_SRV:
            case EDescriptorType::Buffer_SRV:
            case EDescriptorType::AccelerationStructure:
                rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                break;
            case EDescriptorType::Texture_UAV:
            case EDescriptorType::Buffer_UAV:
                rangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                break;
            default:
                continue; // Skip volatile CBV, push constants, samplers
        }

        D3D12_DESCRIPTOR_RANGE1& range = ranges[rangeCount++];
        range.RangeType = rangeType;
        range.NumDescriptors = binding.count;
        range.BaseShaderRegister = binding.slot;
        range.RegisterSpace = registerSpace;
        range.Flags = flags;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    return rangeCount;
}
```

Delete old `PopulateSRVRanges` and `PopulateUAVRanges`.

---

## Task 3: Update CDX12DescriptorSet Handle Storage

**Files:**
- Modify: `RHI/DX12/DX12DescriptorSet.h:132-209`
- Modify: `RHI/DX12/DX12DescriptorSet.cpp:346-636`

**Step 1: Update header member variables**

Replace lines 184-204:
```cpp
    // Combined CBV+SRV+UAV handles (indexed via slot-to-index maps)
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_cbvSrvUavHandles;
    std::vector<bool> m_cbvSrvUavBound;

    // Sampler handles (separate heap)
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_samplerHandles;
    std::vector<bool> m_samplerBound;

    // Volatile CBV data per slot (copied to ring buffer at bind time) - UNCHANGED
    struct VolatileCBVEntry {
        uint32_t slot;
        std::vector<uint8_t> data;
        bool bound = false;
    };
    std::vector<VolatileCBVEntry> m_volatileCBVs;

    // Push constant data
    std::vector<uint8_t> m_pushConstantData;
    bool m_pushConstantBound = false;
```

**Removed:** `m_srvHandles`, `m_uavHandles`, `m_srvBound`, `m_uavBound`, `m_constantBufferGPUAddress`, `m_constantBufferBound`

**Step 2: Update header methods**

Replace lines 143-176:
```cpp
    // DX12-specific accessors for command list binding
    bool HasCBVSRVUAVs() const { return !m_cbvSrvUavHandles.empty(); }
    bool HasSamplers() const { return m_layout->GetSamplerCount() > 0; }
    bool HasVolatileCBV() const { return m_layout->HasVolatileCBV(); }
    bool HasPushConstants() const { return m_layout->HasPushConstants(); }

    // Copy combined CBV+SRV+UAV descriptors to staging ring
    D3D12_GPU_DESCRIPTOR_HANDLE CopyCBVSRVUAVsToStaging(CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device);

    // Copy Samplers to staging ring
    D3D12_GPU_DESCRIPTOR_HANDLE CopySamplersToStaging(CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device);

    // Allocate volatile CBV from ring and return GPU virtual address
    D3D12_GPU_VIRTUAL_ADDRESS AllocateVolatileCBV(CDX12DynamicBufferRing& bufferRing, uint32_t slot);

    // Get number of volatile CBVs in this set
    uint32_t GetVolatileCBVCount() const;

    // Get slot of volatile CBV at index
    uint32_t GetVolatileCBVSlot(uint32_t index) const;

    // Get push constant data
    const void* GetPushConstantData() const { return m_pushConstantData.data(); }
    uint32_t GetPushConstantDwordCount() const { return static_cast<uint32_t>(m_pushConstantData.size()) / 4; }

    bool IsPersistent() const { return m_isPersistent; }
```

**Removed:** `HasSRVs()`, `HasUAVs()`, `HasConstantBuffer()`, `CopySRVsToStaging()`, `CopyUAVsToStaging()`, `GetConstantBufferGPUAddress()`

**Step 3: Update constructor**

```cpp
CDX12DescriptorSet::CDX12DescriptorSet(CDX12DescriptorSetLayout* layout, bool isPersistent)
    : m_layout(layout)
    , m_isPersistent(isPersistent) {

    // Initialize combined CBV+SRV+UAV handle array
    uint32_t cbvSrvUavCount = layout->GetCBVSRVUAVCount();
    if (cbvSrvUavCount > 0) {
        m_cbvSrvUavHandles.resize(cbvSrvUavCount);
        m_cbvSrvUavBound.resize(cbvSrvUavCount, false);
        for (auto& h : m_cbvSrvUavHandles) { h.ptr = 0; }
    }

    // Initialize sampler handle array
    uint32_t samplerCount = layout->GetSamplerCount();
    if (samplerCount > 0) {
        m_samplerHandles.resize(samplerCount);
        m_samplerBound.resize(samplerCount, false);
        for (auto& h : m_samplerHandles) { h.ptr = 0; }
    }

    // Initialize volatile CBV storage for each CBV in layout (unchanged)
    if (layout->HasVolatileCBV()) {
        const auto& cbvInfos = layout->GetVolatileCBVs();
        m_volatileCBVs.resize(cbvInfos.size());
        for (size_t i = 0; i < cbvInfos.size(); ++i) {
            m_volatileCBVs[i].slot = cbvInfos[i].slot;
            m_volatileCBVs[i].data.resize(cbvInfos[i].size, 0);
            m_volatileCBVs[i].bound = false;
        }
    }

    // Initialize push constant storage
    if (layout->HasPushConstants()) {
        m_pushConstantData.resize(layout->GetPushConstantSize(), 0);
    }
}
```

**Step 4: Update Bind() for static CBV**

```cpp
case EDescriptorType::ConstantBuffer: {
    if (item.buffer) {
        auto* dx12Buf = static_cast<CDX12Buffer*>(item.buffer);
        uint32_t index;
        if (m_layout->GetCBVIndex(item.slot, index)) {
            m_cbvSrvUavHandles[index] = dx12Buf->GetCBV();
            m_cbvSrvUavBound[index] = true;
        }
    }
    break;
}
```

**Step 5: Update Bind() for SRV types**

```cpp
case EDescriptorType::Texture_SRV: {
    if (item.texture) {
        auto* dx12Tex = static_cast<CDX12Texture*>(item.texture);
        SDescriptorHandle handle;
        if (item.arraySlice > 0) {
            handle = dx12Tex->GetOrCreateSRVSlice(item.arraySlice, 0);
        } else {
            handle = dx12Tex->GetOrCreateSRV();
        }
        uint32_t index;
        if (m_layout->GetSRVIndex(item.slot, index)) {
            m_cbvSrvUavHandles[index] = handle.cpuHandle;
            m_cbvSrvUavBound[index] = true;
        }
    }
    break;
}
case EDescriptorType::Buffer_SRV: {
    if (item.buffer) {
        auto* dx12Buf = static_cast<CDX12Buffer*>(item.buffer);
        SDescriptorHandle handle = dx12Buf->GetSRV();
        uint32_t index;
        if (m_layout->GetSRVIndex(item.slot, index)) {
            m_cbvSrvUavHandles[index] = handle.cpuHandle;
            m_cbvSrvUavBound[index] = true;
        }
    }
    break;
}
```

**Step 6: Update Bind() for UAV types**

```cpp
case EDescriptorType::Texture_UAV: {
    if (item.texture) {
        auto* dx12Tex = static_cast<CDX12Texture*>(item.texture);
        SDescriptorHandle handle;
        if (item.mipLevel > 0) {
            handle = dx12Tex->GetOrCreateUAVSlice(item.mipLevel);
        } else {
            handle = dx12Tex->GetOrCreateUAV();
        }
        uint32_t index;
        if (m_layout->GetUAVIndex(item.slot, index)) {
            m_cbvSrvUavHandles[index] = handle.cpuHandle;
            m_cbvSrvUavBound[index] = true;
        }
    }
    break;
}
case EDescriptorType::Buffer_UAV: {
    if (item.buffer) {
        auto* dx12Buf = static_cast<CDX12Buffer*>(item.buffer);
        SDescriptorHandle handle = dx12Buf->GetUAV();
        uint32_t index;
        if (m_layout->GetUAVIndex(item.slot, index)) {
            m_cbvSrvUavHandles[index] = handle.cpuHandle;
            m_cbvSrvUavBound[index] = true;
        }
    }
    break;
}
```

**Step 7: Update IsComplete()**

```cpp
bool CDX12DescriptorSet::IsComplete() const {
    // Check all CBV/SRV/UAVs are bound
    for (bool bound : m_cbvSrvUavBound) {
        if (!bound) return false;
    }
    // Check all samplers are bound
    for (bool bound : m_samplerBound) {
        if (!bound) return false;
    }
    // Check volatile CBVs
    if (m_layout->HasVolatileCBV()) {
        for (const auto& cbv : m_volatileCBVs) {
            if (!cbv.bound) return false;
        }
    }
    // Check push constants
    if (m_layout->HasPushConstants() && !m_pushConstantBound) return false;

    return true;
}
```

**Step 8: Implement CopyCBVSRVUAVsToStaging**

```cpp
D3D12_GPU_DESCRIPTOR_HANDLE CDX12DescriptorSet::CopyCBVSRVUAVsToStaging(
    CDX12DescriptorStagingRing& stagingRing, ID3D12Device* device) {

    uint32_t count = static_cast<uint32_t>(m_cbvSrvUavHandles.size());
    if (count == 0) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    SDescriptorHandle stagingHandle = stagingRing.AllocateContiguous(count);
    if (!stagingHandle.IsValid()) {
        assert(false && "CBV/SRV/UAV staging ring overflow");
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    device->CopyDescriptors(
        1, &stagingHandle.cpuHandle, &count,
        count, m_cbvSrvUavHandles.data(), nullptr,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return stagingHandle.gpuHandle;
}
```

Delete old `CopySRVsToStaging` and `CopyUAVsToStaging`.

---

## Task 4: Verify GetCBV() Exists (No Changes Needed)

**Files:**
- Check: `RHI/DX12/DX12Resources.h` (CDX12Buffer class)

**Verification:** `CDX12Buffer::GetCBV()` already exists and returns `D3D12_CPU_DESCRIPTOR_HANDLE`. No changes needed.

---

## Task 5: Update BuildRootSignature

**Files:**
- Modify: `RHI/DX12/DX12RootSignatureCache.cpp:76-208`

**Step 1: Update setBindings initialization**

Replace lines 96-98:
```cpp
        entry.setBindings[setIndex].isUsed = true;
        entry.setBindings[setIndex].samplerCount = dx12Layout->GetSamplerCount();
```

**Step 2: Remove static CBV as root parameter**

Delete the section at lines 135-146 that creates root CBV for static constant buffer.

**Step 3: Replace SRV/UAV table sections with combined CBV+SRV+UAV table**

Replace lines 148-187 with:

```cpp
        // Add combined CBV+SRV+UAV table (NVRHI pattern: descriptorRangesSRVetc)
        uint32_t cbvSrvUavCount = dx12Layout->GetCBVSRVUAVCount();
        if (cbvSrvUavCount > 0) {
            size_t rangeStart = allRanges.size();

            allRanges.resize(rangeStart + dx12Layout->GetBindingCount());
            uint32_t rangeCount = dx12Layout->PopulateCBVSRVUAVRanges(&allRanges[rangeStart], setIndex);
            allRanges.resize(rangeStart + rangeCount);

            if (rangeCount > 0) {
                entry.setBindings[setIndex].cbvSrvUavTableRootParam = static_cast<uint32_t>(rootParams.size());
                entry.setBindings[setIndex].cbvSrvUavCount = cbvSrvUavCount;

                D3D12_ROOT_PARAMETER1 param = {};
                param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                param.DescriptorTable.NumDescriptorRanges = rangeCount;
                param.DescriptorTable.pDescriptorRanges = &allRanges[rangeStart];
                param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                rootParams.push_back(param);
            }
        }
```

---

## Task 6: Update CalculateDWordCost

**Files:**
- Modify: `RHI/DX12/DX12RootSignatureCache.cpp` (find CalculateDWordCost function)

**Step 1: Update cost calculation**

Replace the static CBV and SRV/UAV table cost sections with:

```cpp
        // Combined CBV+SRV+UAV table: 1 DWORD
        if (dx12Layout->GetCBVSRVUAVCount() > 0) cost += 1;

        // Sampler table: 1 DWORD
        if (dx12Layout->GetSamplerCount() > 0) cost += 1;
```

---

## Task 7: Update BindDescriptorSet

**Files:**
- Modify: `RHI/DX12/DX12CommandList.cpp` (find BindDescriptorSet function)

**Step 1: Replace separate binding calls with combined**

Replace SRV, UAV, and static CBV binding sections with:

```cpp
    // Bind combined CBV+SRV+UAV table
    if (bindingInfo.cbvSrvUavTableRootParam != UINT32_MAX && dx12Set->HasCBVSRVUAVs()) {
        auto& stagingRing = heapMgr.GetSRVStagingRing();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = dx12Set->CopyCBVSRVUAVsToStaging(stagingRing, device);
        if (gpuHandle.ptr != 0) {
            if (isCompute) {
                m_commandList->SetComputeRootDescriptorTable(bindingInfo.cbvSrvUavTableRootParam, gpuHandle);
            } else {
                m_commandList->SetGraphicsRootDescriptorTable(bindingInfo.cbvSrvUavTableRootParam, gpuHandle);
            }
        }
    }
```

Remove the old sections:
- Static CBV root binding (`SetGraphicsRootConstantBufferView` for `constantBufferRootParam`)
- Separate SRV table binding (`srvTableRootParam`)
- Separate UAV table binding (`uavTableRootParam`)

---

## Task 8: Build and Test

**Step 1: Build**

Run: `cmake --build build --target forfun`
Expected: Clean build

**Step 2: Run TestDescriptorSet**

Run: `timeout 15 build/Debug/forfun.exe --test TestDescriptorSet`
Expected: Pass

**Step 3: Verify logs**

Read: `E:/forfun/debug/TestDescriptorSet/runtime.log`
Expected: No errors

**Step 4: Visual check**

Read: `E:/forfun/debug/TestDescriptorSet/screenshot_frame20.png`
Expected: Correct rendering

**Step 5: Commit**

```bash
git add RHI/DX12/DX12DescriptorSet.h RHI/DX12/DX12DescriptorSet.cpp \
        RHI/DX12/DX12RootSignatureCache.cpp RHI/DX12/DX12CommandList.cpp
git commit -m "$(cat <<'EOF'
refactor(rhi): merge static-CBV/SRV/UAV into single descriptor table

Following NVRHI's descriptorRangesSRVetc pattern, combine static CBV,
SRV, and UAV into a single descriptor table per set. This reduces
root signature cost from 4 DWORDs (2 root CBV + 1 SRV + 1 UAV) to
1 DWORD per set.

- Replace constantBufferRootParam/srvTableRootParam/uavTableRootParam
  with cbvSrvUavTableRootParam
- Add PopulateCBVSRVUAVRanges() for combined range generation
- Merge m_srvHandles/m_uavHandles into m_cbvSrvUavHandles with CBV
- Static CBV now uses CBV descriptor in table instead of root CBV
- Use binding declaration order for both ranges and handle array

Volatile CBV remains as root CBV (required for per-frame updates).
Samplers remain separate (required by D3D12 heap type separation).

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Verification Checklist

- [ ] `SSetRootParamInfo` uses `cbvSrvUavTableRootParam` (no separate fields)
- [ ] `CDX12DescriptorSetLayout` has `m_cbvSlotToIndex` map
- [ ] `CDX12DescriptorSetLayout::PopulateCBVSRVUAVRanges()` iterates bindings in declaration order
- [ ] `CDX12DescriptorSet` uses single `m_cbvSrvUavHandles` with CBV+SRV+UAV
- [ ] `Bind()` uses slot-to-index maps for all types (CBV, SRV, UAV)
- [ ] Static CBV no longer uses root CBV - goes in table
- [ ] Volatile CBV still uses root CBV (unchanged)
- [ ] `BuildRootSignature()` creates one combined table
- [ ] `BindDescriptorSet()` binds single combined table
- [ ] TestDescriptorSet passes
- [ ] Root signature DWORD cost reduced by 3 per set
