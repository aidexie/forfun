# Legacy API Deletion Plan

**Date**: 2026-01-29
**Status**: READY - Migration Complete
**Prerequisite**: `2026-01-29-legacy-api-removal.md` (COMPLETE)

---

## Migration Completed (2026-01-29)

All 8 remaining passes have been migrated to descriptor sets:

| File | Status | Notes |
|------|--------|-------|
| `SceneRenderer.cpp` | ✅ Done | Added PerFrame/PerPass/PerMaterial/PerDraw sets |
| `Skybox.cpp` | ✅ Done | Created SM 5.1 shaders for conversion pass |
| `PostProcessPass.cpp` | ✅ Done | Removed legacy createShaders/createPipelineState |
| `TAAPass.cpp` | ✅ Done | Restored missing PSO members, removed legacy path |
| `DebugLinePass.cpp` | ✅ Done | Created SM 5.1 shaders, added descriptor set support |
| `SSRPass.cpp` | ✅ Done | Removed legacy path, uses descriptor sets only |
| `Lightmap2DGPUBaker.cpp` | ✅ Done | Migrated 13 DXR bindings to descriptor sets |
| `HiZPass.cpp` | ✅ Done | Removed legacy dispatch methods |

### New Shader Files Created
- `Shader/DebugLine_DS.vs.hlsl`, `DebugLine_DS.gs.hlsl`, `DebugLine_DS.ps.hlsl`
- `Shader/EquirectToCubemap_DS.vs.hlsl`, `EquirectToCubemap_DS.ps.hlsl`

### Build & Test Verification
- Build: ✅ Passed
- TestDeferredStress: ✅ Passed (105 lights, all post-processing effects)

---

## Goal

Permanently delete all legacy slot-based binding APIs and their `#ifndef FF_LEGACY_BINDING_DISABLED` guards. The migration is complete; this is cleanup.

## Scope Summary

| Category | Files | Occurrences |
|----------|-------|-------------|
| RHI Layer | 5 | 16 |
| Render Passes (.cpp) | 28 | 119 |
| Render Passes (.h) | 13 | 40 |
| Build System | 1 | 2 |
| **Total** | **47** | **177** |

---

## Task 1: Remove CMake Option

**File**: `CMakeLists.txt`

**Delete lines 13-18**:
```cmake
# Descriptor set migration - disable legacy binding APIs
option(FF_LEGACY_BINDING_DISABLED "Disable legacy slot-based binding APIs (SetShaderResource, etc.)" ON)

if(FF_LEGACY_BINDING_DISABLED)
    add_compile_definitions(FF_LEGACY_BINDING_DISABLED=1)
endif()
```

---

## Task 2: Remove Legacy APIs from ICommandList.h

**File**: `RHI/ICommandList.h`

**Delete lines 80-102** (legacy binding APIs):
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    // Legacy slot-based binding (deprecated - use BindDescriptorSet instead)
    virtual bool SetConstantBufferData(...) = 0;
    virtual void SetShaderResource(...) = 0;
    virtual void SetShaderResourceBuffer(...) = 0;
    virtual void SetSampler(...) = 0;
    virtual void SetUnorderedAccess(...) = 0;
    virtual void SetUnorderedAccessTexture(...) = 0;
    virtual void SetUnorderedAccessTextureMip(...) = 0;
#endif
```

**Delete lines 189-192** (UnbindShaderResources):
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    virtual void UnbindShaderResources(...) = 0;
#endif
```

---

## Task 3: Remove Legacy APIs from DX12CommandList

**Files**:
- `RHI/DX12/DX12CommandList.h`
- `RHI/DX12/DX12CommandList.cpp`

**Header - Delete lines 74-82**:
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    bool SetConstantBufferData(...) override;
    void SetShaderResource(...) override;
    void SetShaderResourceBuffer(...) override;
    void SetSampler(...) override;
    void SetUnorderedAccess(...) override;
    void SetUnorderedAccessTexture(...) override;
    void SetUnorderedAccessTextureMip(...) override;
#endif
```

**Header - Delete lines 112-114**:
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    void UnbindShaderResources(...) override;
#endif
```

**Implementation - Delete guarded blocks**:
- Lines ~326-450: `SetConstantBufferData`, `SetShaderResource`, `SetShaderResourceBuffer`, `SetSampler`, `SetUnorderedAccess`, `SetUnorderedAccessTexture`, `SetUnorderedAccessTextureMip`
- Lines ~822-827: `UnbindShaderResources`

---

## Task 4: Remove Legacy APIs from DX11CommandList

**Files**:
- `RHI/DX11/DX11CommandList.h`
- `RHI/DX11/DX11CommandList.cpp`

**Header - Delete lines 41-49**:
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    bool SetConstantBufferData(...) override;
    void SetShaderResource(...) override;
    void SetShaderResourceBuffer(...) override;
    void SetSampler(...) override;
    void SetUnorderedAccess(...) override;
    void SetUnorderedAccessTexture(...) override;
    void SetUnorderedAccessTextureMip(...) override;
#endif
```

**Header - Delete lines 80-82**:
```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    void UnbindShaderResources(...) override;
#endif
```

**Implementation - Delete guarded blocks**:
- Lines ~242-420: All legacy binding implementations
- Lines ~521-547: `UnbindShaderResources`

---

## Task 5: Remove Guards from Render Pass Headers

**Files** (13 total):
| File | Occurrences | Content to Delete |
|------|-------------|-------------------|
| BloomPass.h | 4 | Legacy PSO/shader members |
| DepthOfFieldPass.h | 6 | Legacy PSO/shader members |
| PostProcessPass.h | 4 | Legacy PSO/shader members |
| SSRPass.h | 3 | Legacy members |
| HiZPass.h | 3 | Legacy members |
| SSAOPass.h | 2 | Legacy members |
| GBufferPass.h | 2 | Legacy Render() declaration |
| DeferredLightingPass.h | 2 | Legacy members |
| TAAPass.h | 1 | Legacy members |
| IBLGenerator.h | 1 | Legacy members |

**Pattern**: Delete entire `#ifndef FF_LEGACY_BINDING_DISABLED ... #endif` blocks.

---

## Task 6: Remove Guards from Render Pass Implementations

**Files** (28 total, sorted by occurrence count):

| File | Occurrences | Content Type |
|------|-------------|--------------|
| Lightmap2DGPUBaker.cpp | 13 | Legacy baking code |
| AutoExposurePass.cpp | 10 | Legacy compute dispatch |
| IBLGenerator.cpp | 10 | Legacy IBL generation |
| DepthOfFieldPass.cpp | 9 | Legacy DoF passes |
| BloomPass.cpp | 8 | Legacy bloom chain |
| HiZPass.cpp | 6 | Legacy Hi-Z generation |
| SSRPass.cpp | 5 | Legacy SSR |
| TAAPass.cpp | 5 | Legacy TAA |
| PostProcessPass.cpp | 5 | Legacy tonemapping |
| Skybox.cpp | 4 | Legacy skybox render |
| VolumetricLightmap.cpp | 4 | Legacy volumetric |
| Lightmap2DManager.cpp | 4 | Legacy lightmap |
| ClusteredLightingPass.cpp | 3 | Legacy clustering |
| DeferredRenderPipeline.cpp | 3 | Legacy pipeline |
| SSAOPass.cpp | 3 | Legacy SSAO |
| SceneRenderer.cpp | 3 | Legacy scene render |
| AntiAliasingPass.cpp | 2 | Legacy AA |
| DebugLinePass.cpp | 2 | Legacy debug lines |
| DeferredLightingPass.cpp | 2 | Legacy deferred |
| GBufferPass.cpp | 2 | Legacy G-Buffer |
| LightProbeManager.cpp | 2 | Legacy probes |
| TransparentForwardPass.cpp | 2 | Legacy forward |
| GridPass.cpp | 1 | Legacy grid |
| MotionBlurPass.cpp | 1 | Legacy motion blur |
| ReflectionProbeManager.cpp | 1 | Legacy reflection |
| DXRCubemapBaker.cpp | 1 | Legacy DXR |

**Pattern**: For each file, delete entire `#ifndef FF_LEGACY_BINDING_DISABLED ... #endif` blocks including all code inside.

---

## Task 7: Build Verification

```bash
# Clean rebuild
rm -rf build
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target forfun
```

**Expected**: Build succeeds with zero errors (no more `FF_LEGACY_BINDING_DISABLED` references).

---

## Task 8: Runtime Verification

```bash
# Run visual test
./build/Debug/forfun.exe --test TestDeferredStress

# Check screenshot
# Read: E:/forfun/debug/TestDeferredStress/screenshot_frame20.png
```

**Expected**: Rendering unchanged from before deletion.

---

## Task 9: Commit

```bash
git add -A
git commit -m "$(cat <<'EOF'
rhi: permanently remove legacy binding APIs

Delete all legacy slot-based binding APIs after successful migration
to descriptor sets:
- SetConstantBufferData
- SetShaderResource / SetShaderResourceBuffer
- SetSampler
- SetUnorderedAccess / SetUnorderedAccessTexture / SetUnorderedAccessTextureMip
- UnbindShaderResources

Also removes:
- FF_LEGACY_BINDING_DISABLED CMake option
- All #ifndef FF_LEGACY_BINDING_DISABLED guards (47 files, 177 occurrences)
- Legacy render paths in all passes

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Archive Design Documents

Move completed planning documents:
- `docs/plans/2026-01-29-legacy-api-removal.md` -> `docs/plans/archive/`
- `docs/plans/2026-01-29-legacy-api-removal-design.md` -> `docs/plans/archive/`

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| Build failure | Low | All passes already compile with guards ON |
| Runtime regression | Low | No functional change - just removing dead code |
| Missed guard | Low | Grep verification after deletion |

---

## Rollback

If issues discovered post-deletion:
```bash
git revert HEAD
```

---

**Last Updated**: 2026-01-29
