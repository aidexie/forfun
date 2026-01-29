# Legacy Binding API Removal Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove legacy slot-based binding APIs (`SetShaderResource`, `SetConstantBufferData`, etc.) from `ICommandList` and use descriptor sets exclusively.

**Architecture:** Add CMake option `FF_LEGACY_BINDING_DISABLED` (default ON) that conditionally compiles legacy APIs. Migrate all render passes to descriptor set path, then remove legacy code.

**Tech Stack:** C++20, DirectX 12, CMake, HLSL SM 5.1

---

## Progress Tracking (Updated 2026-01-29)

### Phase 1: Core Infrastructure - COMPLETE
| Task | Status | Notes |
|------|--------|-------|
| 1. Add CMake option | ✅ DONE | `FF_LEGACY_BINDING_DISABLED=ON` by default |
| 2. Guard ICommandList.h | ✅ DONE | Legacy APIs wrapped with `#ifndef` |
| 3. Guard DX12CommandList | ✅ DONE | Header and implementation guarded |
| 4. Guard DX11CommandList | ✅ DONE | Header and implementation guarded |

### Phase 1: Render Pass Migration - IN PROGRESS
| Pass | Status | Notes |
|------|--------|-------|
| ShadowPass | ✅ DONE | DS-only, legacy path removed |
| DepthPrePass | ✅ DONE | DS-only, legacy path removed |
| Skybox | ✅ DONE | New DS shaders created, legacy guarded |
| DeferredLightingPass | ✅ DONE | DS-only, legacy path guarded |
| SSAOPass | ✅ DONE | DS dispatch methods added |
| SSRPass | ✅ DONE | DS-only |
| BloomPass | ✅ DONE | DS path added, legacy guarded |
| PostProcessPass | ✅ DONE | DS path added |
| HiZPass | ✅ DONE | DS-only |
| GBufferPass | ✅ DONE | Legacy Render guarded |
| TransparentForwardPass | ✅ DONE | Legacy guarded |
| ClusteredLightingPass | ✅ DONE | Legacy guarded |
| ForwardRenderPipeline | ✅ DONE | UnbindShaderResources removed |
| SceneRenderer | ✅ DONE | Legacy guarded |

### Phase 1: Remaining Passes - TODO
| Pass | Status | Notes |
|------|--------|-------|
| TAAPass | ❌ TODO | Needs DS migration |
| AntiAliasingPass | ❌ TODO | Needs DS migration |
| AutoExposurePass | ❌ TODO | Needs DS migration |
| MotionBlurPass | ❌ TODO | Needs DS migration |
| DepthOfFieldPass | ❌ TODO | Needs DS migration |
| GridPass | ❌ TODO | Needs DS migration |
| DebugLinePass | ❌ TODO | Needs DS migration |
| DeferredRenderPipeline | ❌ TODO | Needs legacy calls removed |

### Phase 2: Utilities & Baking - TODO
| File | Status | Notes |
|------|--------|-------|
| IBLGenerator | ❌ TODO | 9 legacy calls |
| Lightmap2DGPUBaker | ❌ TODO | 19 legacy calls |
| Lightmap2DManager | ❌ TODO | 2 legacy calls |
| ReflectionProbeManager | ❌ TODO | 5 legacy calls |
| LightProbeManager | ❌ TODO | 3 legacy calls |
| VolumetricLightmap | ❌ TODO | 21 legacy calls |
| DXRCubemapBaker | ❌ TODO | 10 legacy calls |

### Build Status
- **Current Errors:** 151 (down from 306)
- **Files Remaining:** 15

---

## Task 1: Add CMake Option

**Files:**
- Modify: `CMakeLists.txt:1-20`

**Step 1: Add CMake option definition**

Add after line 11 (after `set(CMAKE_CXX_STANDARD_REQUIRED ON)`):

```cmake
# Descriptor set migration - disable legacy binding APIs
option(FF_LEGACY_BINDING_DISABLED "Disable legacy slot-based binding APIs (SetShaderResource, etc.)" ON)

if(FF_LEGACY_BINDING_DISABLED)
    add_compile_definitions(FF_LEGACY_BINDING_DISABLED=1)
endif()
```

**Step 2: Verify CMake configuration**

Run:
```bash
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
```
Expected: Configuration succeeds, shows `FF_LEGACY_BINDING_DISABLED=ON`

**Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
build: add FF_LEGACY_BINDING_DISABLED CMake option

Default ON - legacy slot-based binding APIs will be conditionally compiled.
This enables incremental migration to descriptor sets.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Guard Legacy APIs in ICommandList

**Files:**
- Modify: `RHI/ICommandList.h:80-100`

**Step 1: Add preprocessor guards around legacy APIs**

Wrap the legacy binding methods (lines ~80-100) with `#ifndef FF_LEGACY_BINDING_DISABLED`:

```cpp
    // ============================================
    // Resource Binding
    // ============================================

    // Set vertex buffer
    virtual void SetVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t stride, uint32_t offset = 0) = 0;

    // Set index buffer
    virtual void SetIndexBuffer(IBuffer* buffer, EIndexFormat format, uint32_t offset = 0) = 0;

#ifndef FF_LEGACY_BINDING_DISABLED
    // Legacy slot-based binding (deprecated - use BindDescriptorSet instead)
    // Set constant buffer data directly (PREFERRED for per-frame/per-draw data)
    virtual bool SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) = 0;

    // Set shader resource (texture or structured buffer)
    virtual void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) = 0;
    virtual void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) = 0;

    // Set sampler
    virtual void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) = 0;

    // Set unordered access view (for compute shaders or pixel shader UAV)
    virtual void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) = 0;
    virtual void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) = 0;
    virtual void SetUnorderedAccessTextureMip(uint32_t slot, ITexture* texture, uint32_t mipLevel) = 0;
#endif // FF_LEGACY_BINDING_DISABLED

    // Clear UAV buffer with uint values (for resetting atomic counters, etc.)
    virtual void ClearUnorderedAccessViewUint(IBuffer* buffer, const uint32_t values[4]) = 0;
```

Also guard `UnbindShaderResources` (around line 187):

```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    // Unbind shader resources for a stage (slots 0-7)
    virtual void UnbindShaderResources(EShaderStage stage, uint32_t startSlot = 0, uint32_t numSlots = 8) = 0;
#endif
```

**Step 2: Build to find compile errors**

Run:
```bash
cmake --build build --target forfun 2>&1 | head -100
```
Expected: Many compile errors showing all files using legacy APIs

**Step 3: Commit**

```bash
git add RHI/ICommandList.h
git commit -m "$(cat <<'EOF'
rhi: guard legacy binding APIs with FF_LEGACY_BINDING_DISABLED

Legacy APIs (SetShaderResource, SetConstantBufferData, SetSampler,
SetUnorderedAccess, UnbindShaderResources) are now conditionally
compiled. Build errors will reveal all remaining callers.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Guard Legacy APIs in DX12CommandList

**Files:**
- Modify: `RHI/DX12/DX12CommandList.h`
- Modify: `RHI/DX12/DX12CommandList.cpp`

**Step 1: Guard declarations in header**

In `DX12CommandList.h`, wrap legacy method declarations:

```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    bool SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) override;
    void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) override;
    void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) override;
    void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) override;
    void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) override;
    void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) override;
    void SetUnorderedAccessTextureMip(uint32_t slot, ITexture* texture, uint32_t mipLevel) override;
    void UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t numSlots) override;
#endif
```

**Step 2: Guard implementations in cpp**

In `DX12CommandList.cpp`, wrap each legacy method implementation with:

```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
// ... implementation ...
#endif
```

**Step 3: Commit**

```bash
git add RHI/DX12/DX12CommandList.h RHI/DX12/DX12CommandList.cpp
git commit -m "$(cat <<'EOF'
rhi(dx12): guard legacy binding implementations

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Guard Legacy APIs in DX11CommandList

**Files:**
- Modify: `RHI/DX11/DX11CommandList.h`
- Modify: `RHI/DX11/DX11CommandList.cpp`

**Step 1: Guard declarations in header**

In `DX11CommandList.h`, wrap legacy method declarations:

```cpp
#ifndef FF_LEGACY_BINDING_DISABLED
    bool SetConstantBufferData(EShaderStage stage, uint32_t slot, const void* data, size_t size) override;
    void SetShaderResource(EShaderStage stage, uint32_t slot, ITexture* texture) override;
    void SetShaderResourceBuffer(EShaderStage stage, uint32_t slot, IBuffer* buffer) override;
    void SetSampler(EShaderStage stage, uint32_t slot, ISampler* sampler) override;
    void SetUnorderedAccess(uint32_t slot, IBuffer* buffer) override;
    void SetUnorderedAccessTexture(uint32_t slot, ITexture* texture) override;
    void SetUnorderedAccessTextureMip(uint32_t slot, ITexture* texture, uint32_t mipLevel) override;
    void UnbindShaderResources(EShaderStage stage, uint32_t startSlot, uint32_t numSlots) override;
#endif
```

**Step 2: Guard implementations in cpp**

In `DX11CommandList.cpp`, wrap each legacy method implementation.

**Step 3: Commit**

```bash
git add RHI/DX11/DX11CommandList.h RHI/DX11/DX11CommandList.cpp
git commit -m "$(cat <<'EOF'
rhi(dx11): guard legacy binding implementations

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Remove Legacy Path from Already-Migrated Passes

These passes already have descriptor set paths. Delete the legacy code blocks.

**Files:**
- Modify: `Engine/Rendering/Deferred/GBufferPass.cpp`
- Modify: `Engine/Rendering/ShadowPass.cpp`
- Modify: `Engine/Rendering/Deferred/DepthPrePass.cpp`
- Modify: `Engine/Rendering/HiZPass.cpp`
- Modify: `Engine/Rendering/SSAOPass.cpp`
- Modify: `Engine/Rendering/SSRPass.cpp`
- Modify: `Engine/Rendering/TAAPass.cpp`
- Modify: `Engine/Rendering/BloomPass.cpp`
- Modify: `Engine/Rendering/PostProcessPass.cpp`

**Step 1: For each file, find and delete legacy code blocks**

Pattern to find:
```cpp
if (m_useDescriptorSets) {
    // DS path - KEEP THIS
} else {
    // Legacy path - DELETE THIS ENTIRE BLOCK
}
```

Or:
```cpp
cmdList->SetShaderResource(...)  // DELETE
cmdList->SetConstantBufferData(...)  // DELETE
cmdList->SetSampler(...)  // DELETE
```

Replace with DS-only path.

**Step 2: Build and verify**

Run:
```bash
cmake --build build --target forfun
```
Expected: Build succeeds (or shows remaining errors in non-migrated passes)

**Step 3: Commit**

```bash
git add Engine/Rendering/Deferred/GBufferPass.cpp Engine/Rendering/ShadowPass.cpp \
        Engine/Rendering/Deferred/DepthPrePass.cpp Engine/Rendering/HiZPass.cpp \
        Engine/Rendering/SSAOPass.cpp Engine/Rendering/SSRPass.cpp \
        Engine/Rendering/TAAPass.cpp Engine/Rendering/BloomPass.cpp \
        Engine/Rendering/PostProcessPass.cpp
git commit -m "$(cat <<'EOF'
rendering: remove legacy binding paths from migrated passes

GBufferPass, ShadowPass, DepthPrePass, HiZPass, SSAOPass, SSRPass,
TAAPass, BloomPass, PostProcessPass now use descriptor sets exclusively.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Migrate Skybox to Descriptor Sets

**Files:**
- Modify: `Engine/Rendering/Skybox.h`
- Modify: `Engine/Rendering/Skybox.cpp`
- Create: `Shader/Skybox_DS.vs.hlsl`
- Create: `Shader/Skybox_DS.ps.hlsl`

**Step 1: Add descriptor set members to header**

In `Skybox.h`, add:

```cpp
private:
    // Descriptor set resources (DX12)
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    std::unique_ptr<RHI::IDescriptorSet, std::function<void(RHI::IDescriptorSet*)>> m_perPassSet;
    std::unique_ptr<RHI::IShader> m_vs_ds;
    std::unique_ptr<RHI::IShader> m_ps_ds;
    std::unique_ptr<RHI::IPipelineState> m_pso_ds;

    void initDescriptorSets();
```

**Step 2: Create SM 5.1 vertex shader**

Create `Shader/Skybox_DS.vs.hlsl`:

```hlsl
// Skybox vertex shader (SM 5.1 - Descriptor Set path)

cbuffer CB_Skybox : register(b0, space1) {
    float4x4 gViewProj;
    uint gUseReversedZ;
    uint3 _pad;
};

struct VSInput {
    float3 position : POSITION;
};

struct VSOutput {
    float4 posH : SV_Position;
    float3 texCoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;

    // Use position as cubemap lookup direction
    output.texCoord = input.position;

    // Transform position
    float4 posH = mul(float4(input.position, 1.0), gViewProj);

    // Set z = w so depth is always at far plane (1.0 or 0.0 for reversed-Z)
    if (gUseReversedZ) {
        output.posH = posH.xyww;
        output.posH.z = 0.0;  // Reversed-Z: far = 0
    } else {
        output.posH = posH.xyww;  // Standard: far = 1
    }

    return output;
}
```

**Step 3: Create SM 5.1 pixel shader**

Create `Shader/Skybox_DS.ps.hlsl`:

```hlsl
// Skybox pixel shader (SM 5.1 - Descriptor Set path)

TextureCube gEnvMap : register(t0, space1);
SamplerState gSampler : register(s0, space1);

struct PSInput {
    float4 posH : SV_Position;
    float3 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float3 color = gEnvMap.Sample(gSampler, input.texCoord).rgb;
    return float4(color, 1.0);
}
```

**Step 4: Implement initDescriptorSets() in Skybox.cpp**

Add to `Skybox.cpp`:

```cpp
void CSkybox::initDescriptorSets() {
    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    if (!ctx || ctx->GetBackend() != EBackend::DX12) return;

    std::string shaderDir = FFPath::GetSourceDir() + "/Shader/";

#if defined(_DEBUG)
    bool debugShaders = true;
#else
    bool debugShaders = false;
#endif

    // Compile SM 5.1 shaders
    std::string vsSource = LoadShaderSource(shaderDir + "Skybox_DS.vs.hlsl");
    std::string psSource = LoadShaderSource(shaderDir + "Skybox_DS.ps.hlsl");
    if (vsSource.empty() || psSource.empty()) return;

    SCompiledShader vsCompiled = CompileShaderFromSource(vsSource.c_str(), "main", "vs_5_1", nullptr, debugShaders);
    SCompiledShader psCompiled = CompileShaderFromSource(psSource.c_str(), "main", "ps_5_1", nullptr, debugShaders);
    if (!vsCompiled.success || !psCompiled.success) return;

    ShaderDesc vsDesc{EShaderType::Vertex, vsCompiled.bytecode.data(), vsCompiled.bytecode.size(), "Skybox_DS_VS"};
    ShaderDesc psDesc{EShaderType::Pixel, psCompiled.bytecode.data(), psCompiled.bytecode.size(), "Skybox_DS_PS"};
    m_vs_ds.reset(ctx->CreateShader(vsDesc));
    m_ps_ds.reset(ctx->CreateShader(psDesc));

    // Create PerPass layout (Set 1)
    BindingLayoutDesc layoutDesc("Skybox_PerPass");
    layoutDesc.AddItem(BindingLayoutItem::VolatileCBV(0, sizeof(CB_SkyboxTransform)));
    layoutDesc.AddItem(BindingLayoutItem::Texture_SRV(0));
    layoutDesc.AddItem(BindingLayoutItem::Sampler(0));

    m_perPassLayout = ctx->CreateDescriptorSetLayout(layoutDesc);
    if (!m_perPassLayout) return;

    auto* set = ctx->AllocateDescriptorSet(m_perPassLayout);
    m_perPassSet = std::unique_ptr<IDescriptorSet, std::function<void(IDescriptorSet*)>>(
        set, [ctx](IDescriptorSet* s) { ctx->FreeDescriptorSet(s); });

    // Create PSO with descriptor set layout
    PipelineStateDesc psoDesc;
    psoDesc.vertexShader = m_vs_ds.get();
    psoDesc.pixelShader = m_ps_ds.get();
    psoDesc.inputLayout = {{ EVertexSemantic::Position, 0, EVertexFormat::Float3, 0, 0 }};
    psoDesc.rasterizer.cullMode = ECullMode::None;
    psoDesc.depthStencil.depthEnable = true;
    psoDesc.depthStencil.depthWriteEnable = false;
    psoDesc.depthStencil.depthFunc = UseReversedZ() ? EComparisonFunc::GreaterEqual : EComparisonFunc::LessEqual;
    psoDesc.blend.blendEnable = false;
    psoDesc.primitiveTopology = EPrimitiveTopology::TriangleList;
    psoDesc.renderTargetFormats = { ETextureFormat::R16G16B16A16_FLOAT };
    psoDesc.depthStencilFormat = ETextureFormat::D32_FLOAT;
    psoDesc.setLayouts[1] = m_perPassLayout;
    psoDesc.debugName = "Skybox_DS_PSO";
    m_pso_ds.reset(ctx->CreatePipelineState(psoDesc));

    CFFLog::Info("[Skybox] Descriptor set path initialized");
}
```

**Step 5: Update Render() to use descriptor sets**

Replace the legacy binding code in `Render()`:

```cpp
void CSkybox::Render(const XMMATRIX& view, const XMMATRIX& proj) {
    if (!m_initialized || !m_envTexture) return;

    IRenderContext* ctx = CRHIManager::Instance().GetRenderContext();
    ICommandList* cmdList = ctx->GetCommandList();

    XMMATRIX viewNoTranslation = view;
    viewNoTranslation.r[3] = XMVectorSet(0, 0, 0, 1);

    CB_SkyboxTransform cb;
    cb.viewProj = XMMatrixTranspose(viewNoTranslation * proj);
    cb.useReversedZ = UseReversedZ() ? 1 : 0;

    // Use descriptor set path
    if (m_pso_ds && m_perPassSet) {
        cmdList->SetPipelineState(m_pso_ds.get());
        cmdList->SetPrimitiveTopology(EPrimitiveTopology::TriangleList);
        cmdList->SetVertexBuffer(0, m_vertexBuffer.get(), sizeof(SkyboxVertex), 0);
        cmdList->SetIndexBuffer(m_indexBuffer.get(), EIndexFormat::UInt32, 0);

        m_perPassSet->Bind({
            BindingSetItem::VolatileCBV(0, &cb, sizeof(cb)),
            BindingSetItem::Texture_SRV(0, m_envTexture.get()),
            BindingSetItem::Sampler(0, m_sampler.get()),
        });
        cmdList->BindDescriptorSet(1, m_perPassSet.get());

        cmdList->DrawIndexed(m_indexCount, 0, 0);
    }
}
```

**Step 6: Call initDescriptorSets() in Initialize()**

Add at end of `Initialize()` and `InitializeFromKTX2()`:

```cpp
    initDescriptorSets();
```

**Step 7: Build and test**

Run:
```bash
cmake --build build --target forfun
./build/Debug/forfun.exe
```
Expected: Skybox renders correctly

**Step 8: Commit**

```bash
git add Engine/Rendering/Skybox.h Engine/Rendering/Skybox.cpp \
        Shader/Skybox_DS.vs.hlsl Shader/Skybox_DS.ps.hlsl
git commit -m "$(cat <<'EOF'
rendering: migrate Skybox to descriptor sets

- Add SM 5.1 shaders (Skybox_DS.vs.hlsl, Skybox_DS.ps.hlsl)
- Create PerPass layout and descriptor set
- Remove legacy SetShaderResource/SetConstantBufferData calls

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Migrate DeferredLightingPass to Descriptor Sets

**Files:**
- Modify: `Engine/Rendering/Deferred/DeferredLightingPass.h`
- Modify: `Engine/Rendering/Deferred/DeferredLightingPass.cpp`
- Create: `Shader/DeferredLighting_DS.ps.hlsl` (if not exists)

This is a complex pass that reads G-Buffer and outputs lit scene. Follow the same pattern as Skybox but with more bindings.

**Step 1: Review existing DS initialization**

Check if `initDescriptorSets()` already exists and what it does.

**Step 2: Complete the DS path implementation**

Ensure the pass has:
- PerPass layout with G-Buffer textures, shadow maps, IBL textures
- SM 5.1 pixel shader using `space0` (PerFrame) and `space1` (PerPass)
- PSO with `setLayouts[0]` and `setLayouts[1]` set

**Step 3: Update Render() to use descriptor sets exclusively**

Remove all `SetShaderResource`, `SetConstantBufferData`, `SetSampler` calls.

**Step 4: Build and test**

**Step 5: Commit**

---

## Task 8-15: Migrate Remaining Render Passes

Follow the same pattern for each:

| Task | Pass | Complexity |
|------|------|------------|
| 8 | ClusteredLightingPass | High - internal compute |
| 9 | SceneRenderer | Medium - orchestration |
| 10 | TransparentForwardPass | Medium |
| 11 | DebugLinePass | Low |
| 12 | GridPass | Low |
| 13 | AntiAliasingPass | Medium |
| 14 | MotionBlurPass | Low |
| 15 | DepthOfFieldPass | Medium |
| 16 | AutoExposurePass | Low |

For each pass:
1. Add DS members to header
2. Create SM 5.1 shaders if needed
3. Implement `initDescriptorSets()`
4. Update `Render()` to use DS path
5. Build and test
6. Commit

---

## Task 17: Final Build Verification

**Step 1: Clean build with legacy disabled**

```bash
rm -rf build
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DFF_LEGACY_BINDING_DISABLED=ON
cmake --build build --target forfun
```
Expected: Build succeeds with zero errors

**Step 2: Run tests**

```bash
./build/Debug/forfun.exe --test TestDeferredStress
./build/Debug/forfun.exe --test TestClusteredLighting
```
Expected: All tests pass

**Step 3: Visual verification**

```bash
./build/Debug/forfun.exe
```
Expected: Editor renders correctly - shadows, lighting, post-process, skybox all work

**Step 4: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
rendering: complete Phase 1 legacy API removal

All render passes now use descriptor sets exclusively.
Legacy binding APIs are disabled by default.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Phase 2 Tasks (Utilities & Baking)

After Phase 1 is complete, continue with:

| Task | File | Calls |
|------|------|-------|
| 18 | IBLGenerator.cpp | 9 |
| 19 | Lightmap2DGPUBaker.cpp | 19 |
| 20 | Lightmap2DManager.cpp | 2 |
| 21 | ReflectionProbeManager.cpp | 5 |
| 22 | LightProbeManager.cpp | 3 |
| 23 | VolumetricLightmap.cpp | 21 |
| 24 | DXRCubemapBaker.cpp | 10 |
| 25 | DX12GenerateMipsPass.cpp | 5 |
| 26 | TestDXRReadback.cpp | 2 |
| 27 | TestGPUReadback.cpp | 1 |

---

## Final Cleanup Task

After all files are migrated:

**Step 1: Remove preprocessor guards**

Delete all `#ifndef FF_LEGACY_BINDING_DISABLED` / `#endif` blocks from:
- `RHI/ICommandList.h`
- `RHI/DX12/DX12CommandList.h`
- `RHI/DX12/DX12CommandList.cpp`
- `RHI/DX11/DX11CommandList.h`
- `RHI/DX11/DX11CommandList.cpp`

**Step 2: Remove CMake option**

Delete from `CMakeLists.txt`:
```cmake
option(FF_LEGACY_BINDING_DISABLED ...)
if(FF_LEGACY_BINDING_DISABLED)
    ...
endif()
```

**Step 3: Final commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
rhi: remove legacy binding APIs permanently

All code now uses descriptor sets. Legacy APIs removed:
- SetShaderResource
- SetShaderResourceBuffer
- SetConstantBufferData
- SetSampler
- SetUnorderedAccess
- SetUnorderedAccessTexture
- SetUnorderedAccessTextureMip
- UnbindShaderResources

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

**Last Updated:** 2026-01-29
