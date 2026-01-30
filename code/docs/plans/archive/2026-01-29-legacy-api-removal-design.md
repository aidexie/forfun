# Legacy Binding API Removal Design

**Date**: 2026-01-29
**Status**: Draft
**Related**: `descriptor-set-abstraction.md`, `2026-01-28-remaining-passes-descriptor-set-migration.md`

---

## Goal

Remove legacy slot-based binding APIs from `ICommandList` and use descriptor sets exclusively.

## Approach

- Add CMake option `FF_LEGACY_BINDING_DISABLED` (default **ON**)
- When ON: legacy APIs removed from interface, compile errors reveal remaining callers
- When OFF: legacy APIs remain, allowing temporary rollback if needed

## Scope

### Phase 1 - Render Passes (Runtime Critical)

| File | Calls | Notes |
|------|-------|-------|
| DeferredLightingPass.cpp | 16 | Core lighting, high priority |
| ClusteredLightingPass.cpp | 18 | Internal compute needs DS |
| SceneRenderer.cpp | 16 | Main render orchestration |
| TransparentForwardPass.cpp | 12 | Forward rendering |
| Skybox.cpp | 7 | Simple, quick migration |
| DebugLinePass.cpp | 3 | Debug visualization |
| GridPass.cpp | 3 | Editor grid |
| AntiAliasingPass.cpp | 18 | FXAA/SMAA |
| MotionBlurPass.cpp | 5 | Post-process |
| DepthOfFieldPass.cpp | 24 | Post-process |
| AutoExposurePass.cpp | 11 | Post-process |

**Already migrated** (have DS path, delete legacy path):
- GBufferPass, ShadowPass, DepthPrePass, HiZPass
- SSAOPass, SSRPass, TAAPass, BloomPass, PostProcessPass

### Phase 2 - Utilities & Baking

| File | Calls | Notes |
|------|-------|-------|
| IBLGenerator.cpp | 9 | Cubemap convolution |
| Lightmap2DGPUBaker.cpp | 19 | Lightmap baking |
| Lightmap2DManager.cpp | 2 | Lightmap management |
| ReflectionProbeManager.cpp | 5 | Probe capture |
| LightProbeManager.cpp | 3 | Light probes |
| VolumetricLightmap.cpp | 21 | Volumetric GI |
| DXRCubemapBaker.cpp | 10 | RT cubemap baking |
| DX12GenerateMipsPass.cpp | 5 | Mip generation |
| TestDXRReadback.cpp | 2 | Test |
| TestGPUReadback.cpp | 1 | Test |

---

## APIs to Remove

```cpp
// ICommandList.h - Remove these when FF_LEGACY_BINDING_DISABLED=ON
virtual void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
virtual void SetShaderResource(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;
virtual void SetUnorderedAccess(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
virtual void SetUnorderedAccess(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;
virtual void SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) = 0;
virtual void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) = 0;
virtual void UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t count) = 0;
```

---

## CMake Integration

**CMakeLists.txt**:
```cmake
# Option definition (default ON - legacy disabled)
option(FF_LEGACY_BINDING_DISABLED "Disable legacy slot-based binding APIs" ON)

# Pass to compiler
if(FF_LEGACY_BINDING_DISABLED)
    add_compile_definitions(FF_LEGACY_BINDING_DISABLED=1)
endif()
```

**Usage**:
```bash
# Normal build (legacy APIs disabled - default)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug

# Rollback build (legacy APIs available)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DFF_LEGACY_BINDING_DISABLED=OFF
```

**ICommandList.h**:
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    // Legacy slot-based binding (deprecated)
    virtual void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
    virtual void SetShaderResource(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;
    virtual void SetUnorderedAccess(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
    virtual void SetUnorderedAccess(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;
    virtual void SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) = 0;
    virtual void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) = 0;
    virtual void UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t count) = 0;
#endif
```

---

## DX11 Stub Implementation

DX11 backend becomes compile-compatible but runtime-unsupported.

**DX11CommandList.h**:
```cpp
class CDX11CommandList : public ICommandList {
public:
    // Descriptor set binding (stub - DX11 not supported)
    void BindDescriptorSet(uint32_t setIndex, IDescriptorSet* set) override {}
};
```

**DX11RenderContext.h**:
```cpp
class CDX11DescriptorSetAllocator : public IDescriptorSetAllocator {
public:
    IDescriptorSetLayout* CreateLayout(const BindingLayoutDesc& desc) override { return nullptr; }
    IDescriptorSet* AllocatePersistentSet(IDescriptorSetLayout* layout) override { return nullptr; }
    IDescriptorSet* AllocateTransientSet(IDescriptorSetLayout* layout) override { return nullptr; }
    void FreePersistentSet(IDescriptorSet* set) override {}
    void BeginFrame(uint32_t frameIndex) override {}
};

// In CDX11RenderContext:
IDescriptorSetAllocator* GetDescriptorSetAllocator() override {
    return &m_stubAllocator;
}
```

---

## Migration Pattern

### For Already Migrated Passes (Dual-Path)

Delete the legacy code block:

```cpp
// BEFORE: Dual path
void CSomePass::Render(ICommandList* cmdList, ...) {
    if (m_useDescriptorSets) {
        // DS path
        cmdList->BindDescriptorSet(1, m_perPassSet);
    } else {
        // Legacy path - DELETE THIS
        cmdList->SetShaderResource(PS, 0, texture);
        cmdList->SetConstantBufferData(PS, 0, &cb, sizeof(cb));
    }
}

// AFTER: DS only
void CSomePass::Render(ICommandList* cmdList, ...) {
    cmdList->BindDescriptorSet(1, m_perPassSet);
}
```

### For Not Yet Migrated Passes

1. Add layout/set members to header
2. Create layout in `Initialize()`
3. Allocate persistent set in `Initialize()`
4. Bind resources to set before `BindDescriptorSet()`
5. Create `_DS.hlsl` shader with `space1` registers (if needed)
6. Delete legacy calls

---

## Verification

### Build Verification

```bash
# Must compile with legacy disabled (default)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun

# Zero legacy API calls in render passes (after Phase 1)
grep -r "SetShaderResource\|SetConstantBufferData\|SetUnorderedAccess\|SetSampler" Engine/Rendering/*.cpp
# Expected: Only Phase 2 files (utilities/baking)
```

### Runtime Verification

```bash
# Run existing tests
./build/Debug/forfun.exe --test TestDeferredStress
./build/Debug/forfun.exe --test TestClusteredLighting

# Visual check in editor
./build/Debug/forfun.exe
# Verify: shadows, lighting, post-process, skybox all render correctly
```

### Final Cleanup (After Phase 2)

- Remove `#ifndef FF_LEGACY_BINDING_DISABLED` guards entirely
- Delete legacy API declarations from `ICommandList.h`
- Delete legacy implementations from `DX12CommandList.cpp`
- Remove CMake option (no longer needed)

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Missed code path | CMake switch reveals all callers at compile time |
| Runtime regression | Existing tests + visual verification |
| DX11 users | DX11 unsupported for DS path (documented) |
| Large PR | Phase 1/2 split keeps changes manageable |

---

**Last Updated**: 2026-01-29
