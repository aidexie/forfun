# Descriptor Set Ownership Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the 4-set descriptor ownership model (PerFrame, PerPass, PerMaterial, PerDraw) with reserved slot ranges and subsystem integration.

**Architecture:** Create slot header files defining reserved ranges, add `PopulatePerFrameSet()` interface to subsystems, create per-pass layout factories, and migrate FXAA as the reference implementation.

**Tech Stack:** C++17, DirectX 12, HLSL SM 5.1, RHI abstraction layer

---

## Prerequisites

- Design document: `docs/plans/2026-01-26-descriptor-set-ownership-design.md`
- Existing descriptor set implementation: `RHI/IDescriptorSet.h`, `RHI/DX12/DX12DescriptorSetAllocator.cpp`
- FXAA already partially migrated: `Engine/Rendering/AntiAliasingPass.h`

---

## Task 1: Create PerFrame Slot Header

**Files:**
- Create: `RHI/PerFrameSlots.h`

**Step 1: Create the slot header file**

```cpp
// RHI/PerFrameSlots.h
#pragma once
#include <cstdint>

namespace PerFrameSlots {

//==============================================
// Constant Buffers (b-registers, space0)
// Range: b0-b7 (8 slots)
//==============================================
namespace CB {
    constexpr uint32_t PerFrame        = 0;  // Camera, Time, global settings
    constexpr uint32_t Clustered       = 1;  // Cluster grid params
    constexpr uint32_t Volumetric      = 2;  // Lightmap params
    constexpr uint32_t ReflectionProbe = 3;  // Probe selection data
    // b4-b7: Reserved for future
}

//==============================================
// Textures (t-registers, space0)
// Range: t0-t31 (32 slots)
//==============================================
namespace Tex {
    // Global resources (t0-t3)
    constexpr uint32_t ShadowMapArray   = 0;
    constexpr uint32_t BrdfLUT          = 1;
    constexpr uint32_t IrradianceArray  = 2;
    constexpr uint32_t PrefilteredArray = 3;

    // ClusteredLighting (t4-t7)
    constexpr uint32_t Clustered_LightIndexList = 4;
    constexpr uint32_t Clustered_LightGrid      = 5;
    constexpr uint32_t Clustered_LightData      = 6;
    constexpr uint32_t Clustered_Reserved       = 7;

    // VolumetricLightmap (t8-t12)
    constexpr uint32_t Volumetric_SH_R     = 8;
    constexpr uint32_t Volumetric_SH_G     = 9;
    constexpr uint32_t Volumetric_SH_B     = 10;
    constexpr uint32_t Volumetric_Octree   = 11;
    constexpr uint32_t Volumetric_Reserved = 12;

    // ReflectionProbes (t13-t15)
    constexpr uint32_t ReflectionProbe_Array    = 13;
    constexpr uint32_t ReflectionProbe_Indices  = 14;
    constexpr uint32_t ReflectionProbe_Reserved = 15;

    // t16-t31: Reserved for future (DDGI, RTGI, etc.)
}

//==============================================
// Samplers (s-registers, space0)
// Range: s0-s7 (8 slots)
//==============================================
namespace Samp {
    constexpr uint32_t LinearClamp = 0;
    constexpr uint32_t LinearWrap  = 1;
    constexpr uint32_t PointClamp  = 2;
    constexpr uint32_t ShadowCmp   = 3;
    constexpr uint32_t Aniso       = 4;
    // s5-s7: Reserved for future
}

} // namespace PerFrameSlots
```

**Step 2: Verify file compiles**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds (header not yet included anywhere)

**Step 3: Commit**

```bash
git add RHI/PerFrameSlots.h
git commit -m "feat(rhi): add PerFrame slot constants header

Define reserved register ranges for Set 0 (space0):
- CB: b0-b3 (PerFrame, Clustered, Volumetric, ReflectionProbe)
- Tex: t0-t15 (global resources, subsystem slots)
- Samp: s0-s4 (common samplers)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 2: Create PerPass Slot Header

**Files:**
- Create: `RHI/PerPassSlots.h`

**Step 1: Create the slot header file**

```cpp
// RHI/PerPassSlots.h
#pragma once
#include <cstdint>

namespace PerPassSlots {

//==============================================
// Constant Buffers (b-registers, space1)
//==============================================
namespace CB {
    constexpr uint32_t PerPass = 0;
}

//==============================================
// Textures (t-registers, space1)
// Range: t0-t15 (16 slots)
//==============================================
namespace Tex {
    // G-Buffer (t0-t5)
    constexpr uint32_t GBuffer_Albedo   = 0;
    constexpr uint32_t GBuffer_Normal   = 1;
    constexpr uint32_t GBuffer_WorldPos = 2;
    constexpr uint32_t GBuffer_Emissive = 3;
    constexpr uint32_t GBuffer_Velocity = 4;
    constexpr uint32_t GBuffer_Depth    = 5;

    // Post-process inputs (t6-t11)
    constexpr uint32_t SceneColor = 6;
    constexpr uint32_t SSAO       = 7;
    constexpr uint32_t SSR        = 8;
    constexpr uint32_t Bloom      = 9;
    constexpr uint32_t PrevFrame  = 10;
    constexpr uint32_t DepthHiZ   = 11;

    // t12-t15: Reserved
}

//==============================================
// Samplers (s-registers, space1)
//==============================================
namespace Samp {
    constexpr uint32_t PointClamp  = 0;
    constexpr uint32_t LinearClamp = 1;
}

//==============================================
// UAVs (u-registers, space1)
//==============================================
namespace UAV {
    constexpr uint32_t Output0 = 0;
    constexpr uint32_t Output1 = 1;
}

} // namespace PerPassSlots
```

**Step 2: Verify file compiles**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add RHI/PerPassSlots.h
git commit -m "feat(rhi): add PerPass slot constants header

Define reserved register ranges for Set 1 (space1):
- CB: b0 (PerPass)
- Tex: t0-t11 (G-Buffer, post-process inputs)
- Samp: s0-s1 (PointClamp, LinearClamp)
- UAV: u0-u1 (compute outputs)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 3: Create PerMaterial Slot Header

**Files:**
- Create: `RHI/PerMaterialSlots.h`

**Step 1: Create the slot header file**

```cpp
// RHI/PerMaterialSlots.h
#pragma once
#include <cstdint>

namespace PerMaterialSlots {

//==============================================
// Constant Buffers (b-registers, space2)
//==============================================
namespace CB {
    constexpr uint32_t Material = 0;
}

//==============================================
// Textures (t-registers, space2)
// Range: t0-t7 (8 slots for PBR + extras)
//==============================================
namespace Tex {
    constexpr uint32_t Albedo            = 0;
    constexpr uint32_t Normal            = 1;
    constexpr uint32_t MetallicRoughness = 2;
    constexpr uint32_t Emissive          = 3;
    constexpr uint32_t AO                = 4;
    constexpr uint32_t Height            = 5;  // Future: parallax
    constexpr uint32_t DetailNormal      = 6;  // Future: detail
    constexpr uint32_t Mask              = 7;  // Future: custom mask
}

//==============================================
// Samplers (s-registers, space2)
//==============================================
namespace Samp {
    constexpr uint32_t Material = 0;
}

} // namespace PerMaterialSlots
```

**Step 2: Verify file compiles**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add RHI/PerMaterialSlots.h
git commit -m "feat(rhi): add PerMaterial slot constants header

Define reserved register ranges for Set 2 (space2):
- CB: b0 (Material properties)
- Tex: t0-t7 (PBR textures + future slots)
- Samp: s0 (Material sampler)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 4: Create PerDraw Slot Header

**Files:**
- Create: `RHI/PerDrawSlots.h`

**Step 1: Create the slot header file**

```cpp
// RHI/PerDrawSlots.h
#pragma once
#include <cstdint>
#include <DirectXMath.h>

namespace PerDrawSlots {

//==============================================
// Push Constants (space3)
// Size limit: 128 bytes (DX12 root constants)
//==============================================
namespace Push {
    constexpr uint32_t PerDraw = 0;
}

// Must fit in 128 bytes (32 DWORDs)
struct CB_PerDraw {
    DirectX::XMFLOAT4X4 World;             // 64 bytes
    DirectX::XMFLOAT4X4 WorldInvTranspose; // 64 bytes
    // Total: 128 bytes
};

} // namespace PerDrawSlots
```

**Step 2: Verify file compiles**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add RHI/PerDrawSlots.h
git commit -m "feat(rhi): add PerDraw slot constants header

Define reserved register ranges for Set 3 (space3):
- Push: slot 0 (PerDraw transform data)
- CB_PerDraw struct: World + WorldInvTranspose (128 bytes)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 5: Add IPerFrameContributor Interface

**Files:**
- Create: `Engine/Rendering/IPerFrameContributor.h`

**Step 1: Create the interface header**

```cpp
// Engine/Rendering/IPerFrameContributor.h
#pragma once

namespace RHI {
    class IDescriptorSet;
}

// Interface for subsystems that contribute bindings to the PerFrame descriptor set.
// Subsystems implement PopulatePerFrameSet() to bind their resources to Set 0.
class IPerFrameContributor {
public:
    virtual ~IPerFrameContributor() = default;

    // Called once per frame to populate PerFrame descriptor set bindings.
    // Subsystem should call set->Bind() with its resources using PerFrameSlots constants.
    virtual void PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) = 0;
};
```

**Step 2: Verify file compiles**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add Engine/Rendering/IPerFrameContributor.h
git commit -m "feat(rendering): add IPerFrameContributor interface

Subsystems implement PopulatePerFrameSet() to contribute their
bindings to the shared PerFrame descriptor set (Set 0).

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 6: Add PopulatePerFrameSet to ClusteredLightingPass

**Files:**
- Modify: `Engine/Rendering/ClusteredLightingPass.h`
- Modify: `Engine/Rendering/ClusteredLightingPass.cpp`

**Step 1: Add interface inheritance and method declaration to header**

In `Engine/Rendering/ClusteredLightingPass.h`:

Add include at top:
```cpp
#include "IPerFrameContributor.h"
#include "RHI/PerFrameSlots.h"
```

Change class declaration:
```cpp
class CClusteredLightingPass : public IPerFrameContributor {
```

Add public method after `RenderDebug`:
```cpp
    // IPerFrameContributor implementation
    void PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) override;
```

**Step 2: Implement PopulatePerFrameSet in cpp**

In `Engine/Rendering/ClusteredLightingPass.cpp`, add include:
```cpp
#include "RHI/IDescriptorSet.h"
```

Add implementation at end of file:
```cpp
void CClusteredLightingPass::PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) {
    using namespace RHI;
    using namespace PerFrameSlots;

    // Bind clustered lighting buffers to PerFrame set
    perFrameSet->Bind({
        BindingSetItem::Buffer_SRV(Tex::Clustered_LightGrid, m_clusterDataBuffer.get()),
        BindingSetItem::Buffer_SRV(Tex::Clustered_LightIndexList, m_compactLightListBuffer.get()),
        BindingSetItem::Buffer_SRV(Tex::Clustered_LightData, m_pointLightBuffer.get()),
    });
}
```

**Step 3: Build and verify**

Run: `cmake --build build --target forfun 2>&1 | head -50`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add Engine/Rendering/ClusteredLightingPass.h Engine/Rendering/ClusteredLightingPass.cpp
git commit -m "feat(clustered): implement IPerFrameContributor

ClusteredLightingPass now exposes PopulatePerFrameSet() to bind
its buffers (LightGrid, LightIndexList, LightData) to Set 0
using PerFrameSlots::Tex constants.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 7: Create PassLayouts Header with FXAA Layout Factory

**Files:**
- Create: `Engine/Rendering/PassLayouts.h`

**Step 1: Create the pass layouts header**

```cpp
// Engine/Rendering/PassLayouts.h
#pragma once
#include "RHI/IDescriptorSet.h"
#include "RHI/PerPassSlots.h"

// Forward declare CB structs (defined in respective pass headers)
struct CB_FXAA;
struct CB_SSAO;
struct CB_SSR;
struct CB_Bloom;
struct CB_TAA;
struct CB_DeferredLighting;

namespace PassLayouts {

//==============================================
// FXAA Pass Layout
//==============================================
inline RHI::IDescriptorSetLayout* CreateFXAALayout(
    RHI::IDescriptorSetAllocator* alloc,
    uint32_t cbSize)
{
    using namespace RHI;
    using namespace PerPassSlots;

    return alloc->CreateLayout(
        BindingLayoutDesc("FXAA")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, cbSize))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SceneColor))
            .AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp))
    );
}

//==============================================
// Bloom Pass Layout (placeholder for future)
//==============================================
inline RHI::IDescriptorSetLayout* CreateBloomLayout(
    RHI::IDescriptorSetAllocator* alloc,
    uint32_t cbSize)
{
    using namespace RHI;
    using namespace PerPassSlots;

    return alloc->CreateLayout(
        BindingLayoutDesc("Bloom")
            .AddItem(BindingLayoutItem::VolatileCBV(CB::PerPass, cbSize))
            .AddItem(BindingLayoutItem::Texture_SRV(Tex::SceneColor))
            .AddItem(BindingLayoutItem::Sampler(Samp::LinearClamp))
    );
}

} // namespace PassLayouts
```

**Step 2: Build and verify**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add Engine/Rendering/PassLayouts.h
git commit -m "feat(rendering): add PassLayouts factory header

Centralized per-pass layout creation using PerPassSlots constants.
Currently includes FXAA and Bloom layout factories.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 8: Update FXAA to Use PassLayouts and PerPassSlots

**Files:**
- Modify: `Engine/Rendering/AntiAliasingPass.cpp`

**Step 1: Add includes**

At top of `Engine/Rendering/AntiAliasingPass.cpp`, add:
```cpp
#include "PassLayouts.h"
#include "RHI/PerPassSlots.h"
```

**Step 2: Update FXAA layout creation**

Find the FXAA layout creation code and update to use PassLayouts factory:
```cpp
// Replace direct CreateLayout call with:
m_fxaaLayout = PassLayouts::CreateFXAALayout(allocator, sizeof(CB_FXAA));
```

**Step 3: Update FXAA descriptor set binding**

Find the renderFXAA function and update bindings to use PerPassSlots:
```cpp
using namespace PerPassSlots;

m_fxaaDescSet->Bind({
    RHI::BindingSetItem::VolatileCBV(CB::PerPass, &cbFxaa, sizeof(CB_FXAA)),
    RHI::BindingSetItem::Texture_SRV(Tex::SceneColor, input),
    RHI::BindingSetItem::Sampler(Samp::LinearClamp, m_linearSampler.get()),
});
```

**Step 4: Build and verify**

Run: `cmake --build build --target forfun 2>&1 | head -50`
Expected: Build succeeds

**Step 5: Run FXAA test**

Run: `timeout 15 ./build/Debug/forfun.exe --test TestAntiAliasing`
Expected: Test passes

**Step 6: Commit**

```bash
git add Engine/Rendering/AntiAliasingPass.cpp
git commit -m "refactor(fxaa): migrate to PassLayouts and PerPassSlots

FXAA now uses:
- PassLayouts::CreateFXAALayout() for layout creation
- PerPassSlots::Tex::SceneColor for texture binding
- PerPassSlots::CB::PerPass for constant buffer

This serves as the reference implementation for other pass migrations.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 9: Create Common.hlsli Shader Include

**Files:**
- Create: `Shader/Common.hlsli`

**Step 1: Create the shared HLSL include**

```hlsl
// Shader/Common.hlsli
// Shared descriptor set declarations for all shaders
// Matches C++ slot constants in RHI/*Slots.h headers

#ifndef COMMON_HLSLI
#define COMMON_HLSLI

//==============================================
// Set 0: PerFrame (space0)
//==============================================

// Constant Buffers
cbuffer CB_PerFrame : register(b0, space0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float4x4 gInvView;
    float4x4 gInvProj;
    float4x4 gInvViewProj;
    float3 gCameraPos;
    float gTime;
    float2 gScreenSize;
    float gNearZ;
    float gFarZ;
};

// Global textures (t0-t3)
Texture2DArray gShadowMaps       : register(t0, space0);
Texture2D gBrdfLUT               : register(t1, space0);
TextureCubeArray gIrradiance     : register(t2, space0);
TextureCubeArray gPrefiltered    : register(t3, space0);

// Clustered lighting (t4-t6)
StructuredBuffer<uint2> gClusterData     : register(t4, space0);
StructuredBuffer<uint> gLightIndexList   : register(t5, space0);
StructuredBuffer<float4> gLightData      : register(t6, space0);

// Samplers
SamplerState gLinearClamp          : register(s0, space0);
SamplerState gLinearWrap           : register(s1, space0);
SamplerState gPointClamp           : register(s2, space0);
SamplerComparisonState gShadowSamp : register(s3, space0);
SamplerState gAniso                : register(s4, space0);

//==============================================
// Set 2: PerMaterial (space2)
//==============================================

cbuffer CB_Material : register(b0, space2) {
    float4 gAlbedoColor;
    float gRoughness;
    float gMetallic;
    float2 gMaterialPadding;
};

Texture2D gAlbedoTex             : register(t0, space2);
Texture2D gNormalTex             : register(t1, space2);
Texture2D gMetallicRoughnessTex  : register(t2, space2);
Texture2D gEmissiveTex           : register(t3, space2);
Texture2D gAOTex                 : register(t4, space2);
SamplerState gMaterialSampler    : register(s0, space2);

//==============================================
// Set 3: PerDraw (space3)
//==============================================

cbuffer CB_PerDraw : register(b0, space3) {
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};

#endif // COMMON_HLSLI
```

**Step 2: Verify shader compiles**

Run: `cmake --build build --target forfun 2>&1 | head -20`
Expected: Build succeeds (include not yet used)

**Step 3: Commit**

```bash
git add Shader/Common.hlsli
git commit -m "feat(shader): add Common.hlsli with set declarations

Shared HLSL include defining descriptor set bindings:
- Set 0 (space0): PerFrame resources
- Set 2 (space2): PerMaterial resources
- Set 3 (space3): PerDraw resources
- Set 1 (space1): Defined per-pass in individual shaders

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 10: Update TestDescriptorSet to Use Slot Constants

**Files:**
- Modify: `Tests/TestDescriptorSet.cpp`

**Step 1: Add includes**

At top of file, add:
```cpp
#include "RHI/PerFrameSlots.h"
#include "RHI/PerPassSlots.h"
```

**Step 2: Update test to use slot constants**

Find any hardcoded slot numbers and replace with constants:
```cpp
// Example: replace slot 0 with PerPassSlots::Tex::SceneColor
BindingLayoutItem::Texture_SRV(PerPassSlots::Tex::SceneColor)
```

**Step 3: Build and run test**

Run: `cmake --build build --target forfun && timeout 15 ./build/Debug/forfun.exe --test TestDescriptorSet`
Expected: Test passes

**Step 4: Commit**

```bash
git add Tests/TestDescriptorSet.cpp
git commit -m "test(descriptor): update TestDescriptorSet to use slot constants

Replace hardcoded slot numbers with PerFrameSlots and PerPassSlots
constants for consistency with new ownership model.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 11: Update CMakeLists.txt for New Headers

**Files:**
- Modify: `CMakeLists.txt`

**Step 1: Add new headers to source list**

Find the RHI headers section and add:
```cmake
RHI/PerFrameSlots.h
RHI/PerPassSlots.h
RHI/PerMaterialSlots.h
RHI/PerDrawSlots.h
```

Find the Engine/Rendering headers section and add:
```cmake
Engine/Rendering/IPerFrameContributor.h
Engine/Rendering/PassLayouts.h
```

Find the Shader section and add:
```cmake
Shader/Common.hlsli
```

**Step 2: Build and verify**

Run: `cmake --build build --target forfun`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add new descriptor set headers to CMakeLists

Add slot headers, IPerFrameContributor interface, PassLayouts,
and Common.hlsli to the build.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 12: Run Full Test Suite

**Files:** None (verification only)

**Step 1: Build full project**

Run: `cmake --build build --target forfun`
Expected: Build succeeds with no errors

**Step 2: Run TestDescriptorSet**

Run: `timeout 15 ./build/Debug/forfun.exe --test TestDescriptorSet`
Expected: Test passes

**Step 3: Run TestAntiAliasing (FXAA)**

Run: `timeout 15 ./build/Debug/forfun.exe --test TestAntiAliasing`
Expected: Test passes

**Step 4: Run TestDeferred (uses clustered lighting)**

Run: `timeout 30 ./build/Debug/forfun.exe --test TestDeferred`
Expected: Test passes

**Step 5: Final commit (if any fixes needed)**

```bash
git status
# If clean: no action needed
# If changes: commit fixes
```

---

## Summary

| Task | Component | Status |
|------|-----------|--------|
| 1 | PerFrameSlots.h | |
| 2 | PerPassSlots.h | |
| 3 | PerMaterialSlots.h | |
| 4 | PerDrawSlots.h | |
| 5 | IPerFrameContributor.h | |
| 6 | ClusteredLightingPass integration | |
| 7 | PassLayouts.h | |
| 8 | FXAA migration | |
| 9 | Common.hlsli | |
| 10 | TestDescriptorSet update | |
| 11 | CMakeLists.txt | |
| 12 | Full test verification | |

---

## Next Steps (After This Plan)

1. **Migrate more subsystems** - VolumetricLightmap, ReflectionProbeManager
2. **Migrate more passes** - SSAO, SSR, Bloom, TAA, DeferredLighting
3. **Update shaders** - Include Common.hlsli, use space0/1/2/3 registers
4. **Remove legacy BindToMainPass()** - After all passes migrated

---

**Last Updated**: 2026-01-26
