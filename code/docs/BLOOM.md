# Bloom Post-Processing Effect

## Overview

HDR Bloom effect using **Dual Kawase Blur** algorithm with 5 mip levels. Extracts bright pixels from HDR buffer and creates a soft glow effect.

**Dependencies**: Phase 3.2 G-Buffer (HDR intermediate buffer)

---

## Architecture

```
HDR Buffer (after TransparentPass)
        │
        ▼
┌─────────────────────────────────────────────────────┐
│  BloomPass                                          │
│  ┌─────────────────────────────────────────────┐   │
│  │ 1. Threshold (extract bright pixels)         │   │
│  │    HDR → Mip[0] (half res)                   │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 2. Downsample Chain (5 levels)               │   │
│  │    1/2 → 1/4 → 1/8 → 1/16 → 1/32             │   │
│  │    (Kawase downsample: 5-tap diagonal)       │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 3. Upsample Chain (5 levels, additive)       │   │
│  │    1/32 → 1/16 → 1/8 → 1/4 → 1/2             │   │
│  │    (Kawase upsample: 9-tap tent filter)      │   │
│  └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
        │
        ▼ (Bloom result at 1/2 res)
┌─────────────────────────────────────────────────────┐
│  PostProcessPass (modified)                         │
│  - Sample HDR input                                 │
│  - Add bloom texture (bilinear upsample to full)    │
│  - Apply exposure                                   │
│  - ACES tonemapping                                 │
│  - Output to LDR                                    │
└─────────────────────────────────────────────────────┘
        │
        ▼
    LDR Output
```

---

## Dual Kawase Blur Algorithm

### Downsample (5-tap)
Sample center (weight 4) + 4 diagonal corners (weight 1 each), divide by 8:
```
    [-1,-1]     [+1,-1]
         \     /
          [0,0]  (center × 4)
         /     \
    [-1,+1]     [+1,+1]
```

### Upsample (9-tap tent filter)
Sample center + 4 axis + 4 diagonal with tent weights:
```
    [1]   [2]   [1]
    [2]   [4]   [2]   / 16
    [1]   [2]   [1]
```

---

## Files to Create

| File | Description |
|------|-------------|
| `Engine/Rendering/BloomPass.h` | BloomPass class header |
| `Engine/Rendering/BloomPass.cpp` | BloomPass implementation |
| `Shader/BloomThreshold.ps.hlsl` | Threshold pixel shader |
| `Shader/BloomDownsample.ps.hlsl` | Downsample pixel shader |
| `Shader/BloomUpsample.ps.hlsl` | Upsample pixel shader |
| `Tests/TestBloom.cpp` | Automated test case |

---

## Files to Modify

| File | Changes |
|------|---------|
| `Engine/SceneLightSettings.h` | Add `SBloomSettings` struct |
| `Engine/Rendering/PostProcessPass.h` | Add bloom texture input parameter |
| `Engine/Rendering/PostProcessPass.cpp` | Composite bloom in shader |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.h` | Add `CBloomPass` member |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Integrate bloom pass |
| `Editor/Panels_SceneLightSettings.cpp` | Add bloom UI controls |
| `CMakeLists.txt` | Add `BloomPass.cpp` |

---

## Class Design

### SBloomSettings (SceneLightSettings.h)
```cpp
struct SBloomSettings
{
    bool enabled = true;
    float threshold = 1.0f;   // Luminance cutoff (0-5)
    float intensity = 1.0f;   // Bloom strength (0-3)
    float scatter = 0.7f;     // Mip blend factor (0-1)
};
```

### CBloomPass (BloomPass.h)
```cpp
class CBloomPass {
public:
    bool Initialize();
    void Shutdown();

    // Returns bloom texture (half resolution)
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          uint32_t width, uint32_t height,
                          const SBloomSettings& settings);

private:
    static constexpr int kMipCount = 5;

    // Mip chain: R11G11B10_FLOAT for bandwidth efficiency
    RHI::TexturePtr m_mipChain[kMipCount];
    uint32_t m_mipWidth[kMipCount];
    uint32_t m_mipHeight[kMipCount];

    // Shaders
    RHI::ShaderPtr m_fullscreenVS;
    RHI::ShaderPtr m_thresholdPS;
    RHI::ShaderPtr m_downsamplePS;
    RHI::ShaderPtr m_upsamplePS;

    // PSOs
    RHI::PipelineStatePtr m_thresholdPSO;
    RHI::PipelineStatePtr m_downsamplePSO;
    RHI::PipelineStatePtr m_upsamplePSO;  // Additive blend

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::SamplerPtr m_linearSampler;

    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;

    void ensureMipChain(uint32_t width, uint32_t height);
    void createShaders();
    void createPSOs();
};
```

---

## Shader Specifications

### BloomThreshold.ps.hlsl
```hlsl
cbuffer CB_BloomThreshold : register(b0) {
    float2 gTexelSize;   // 1.0 / inputSize
    float gThreshold;    // Luminance threshold
    float gSoftKnee;     // Soft transition (0.5)
};

// Soft threshold with knee for smooth falloff
// Rec.709 luminance: 0.2126*R + 0.7152*G + 0.0722*B
```

### BloomDownsample.ps.hlsl
```hlsl
cbuffer CB_BloomDownsample : register(b0) {
    float2 gTexelSize;   // 1.0 / inputSize
    float2 _pad;
};

// 5-tap Kawase downsample
// Center × 4 + 4 diagonals, divide by 8
```

### BloomUpsample.ps.hlsl
```hlsl
cbuffer CB_BloomUpsample : register(b0) {
    float2 gTexelSize;   // 1.0 / inputSize
    float gScatter;      // Blend with previous level
    float _pad;
};

// 9-tap tent filter upsample
// Additive blend with previous mip level
```

---

## Pipeline Integration

### DeferredRenderPipeline.cpp (line ~358)
```cpp
// ============================================
// 7. Bloom Pass (NEW - between Transparent and PostProcess)
// ============================================
RHI::ITexture* bloomResult = nullptr;
if (ctx.showFlags.PostProcessing) {
    const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
    if (bloomSettings.enabled) {
        CScopedDebugEvent evt(cmdList, L"Bloom");
        bloomResult = m_bloomPass.Render(
            m_offHDR.get(), ctx.width, ctx.height, bloomSettings);
    }
}

// ============================================
// 8. Post-Processing (HDR -> LDR) - MODIFIED
// ============================================
if (ctx.showFlags.PostProcessing) {
    CScopedDebugEvent evt(cmdList, L"Post-Processing");
    const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
    m_postProcess.Render(
        m_offHDR.get(),
        bloomResult,  // NEW: can be nullptr
        m_offLDR.get(),
        ctx.width, ctx.height,
        1.0f,  // exposure
        bloomSettings.enabled ? bloomSettings.intensity : 0.0f);
}
```

---

## PostProcessPass Modifications

### PostProcessPass.h
```cpp
void Render(RHI::ITexture* hdrInput,
            RHI::ITexture* bloomTexture,  // NEW (nullable)
            RHI::ITexture* ldrOutput,
            uint32_t width, uint32_t height,
            float exposure = 1.0f,
            float bloomIntensity = 1.0f);  // NEW
```

### PostProcessPass.cpp - Shader Changes
```hlsl
Texture2D hdrTexture : register(t0);
Texture2D bloomTexture : register(t1);  // NEW
SamplerState samp : register(s0);

cbuffer CB_PostProcess : register(b0) {
    float gExposure;
    float gBloomIntensity;  // NEW
    float2 _pad;
};

float4 main(PSIn input) : SV_Target {
    float3 hdrColor = hdrTexture.Sample(samp, input.uv).rgb;

    // Add bloom (bloom texture is half res, bilinear upsample)
    if (gBloomIntensity > 0.0) {
        float3 bloom = bloomTexture.Sample(samp, input.uv).rgb;
        hdrColor += bloom * gBloomIntensity;
    }

    hdrColor *= gExposure;
    float3 ldrColor = ACESFilm(hdrColor);
    return float4(ldrColor, 1.0);
}
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Add after "Clustered Lighting Debug" section (~line 350):
```cpp
// ============================================
// Post-Processing: Bloom Section
// ============================================
ImGui::Spacing();
ImGui::Spacing();
ImGui::Text("Post-Processing: Bloom");
ImGui::Separator();

auto& bloom = settings.bloom;
ImGui::Checkbox("Enable##Bloom", &bloom.enabled);

if (bloom.enabled) {
    ImGui::PushItemWidth(150);
    ImGui::SliderFloat("Threshold##Bloom", &bloom.threshold, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Intensity##Bloom", &bloom.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Scatter##Bloom", &bloom.scatter, 0.0f, 1.0f, "%.2f");
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Threshold: Luminance cutoff for bloom\n"
            "Intensity: Bloom brightness multiplier\n"
            "Scatter: Blend factor between blur levels");
    }
}
```

---

## Test Case (TestBloom.cpp)

```cpp
#include "Core/Testing/TestFramework.h"
#include "Engine/Scene.h"

class TestBloom : public CTestCase {
public:
    const char* GetName() const override { return "TestBloom"; }

    void OnStart() override {
        // Use default scene with emissive objects
        auto& settings = CScene::Instance().GetLightSettings();
        settings.bloom.enabled = true;
        settings.bloom.threshold = 1.0f;
        settings.bloom.intensity = 1.5f;
        settings.bloom.scatter = 0.7f;
    }

    void OnFrame(int frame) override {
        if (frame == 20) {
            CaptureScreenshot("bloom_enabled");
            CScene::Instance().GetLightSettings().bloom.enabled = false;
        }
        if (frame == 22) {
            CaptureScreenshot("bloom_disabled");
            Pass("Bloom rendering completed");
            Exit();
        }
    }
};

REGISTER_TEST(TestBloom)
```

---

## Performance Targets

| Resolution | Mip Chain Memory | Target Time |
|------------|-----------------|-------------|
| 1920×1080  | ~8 MB           | < 1.5 ms    |
| 2560×1440  | ~14 MB          | < 2.0 ms    |
| 3840×2160  | ~32 MB          | < 3.0 ms    |

---

## Implementation Notes

1. **Texture Format**: `R11G11B10_FLOAT` for mip chain (no alpha, bandwidth efficient)

2. **Mip Sizing**:
   - Mip[0]: width/2 × height/2 (threshold output)
   - Mip[1-4]: successive halves

3. **Upsample Blend**: Additive blend state for accumulating during upsample chain

4. **Resource Hazards**: Unbind RT before using as SRV (follow PostProcessPass pattern)

5. **DX11/DX12 Compatibility**: Use RHI abstraction for all resource operations

---

## Implementation Order

1. Create shader files (Threshold, Downsample, Upsample)
2. Create BloomPass.h/cpp
3. Add SBloomSettings to SceneLightSettings.h
4. Modify PostProcessPass (header + implementation)
5. Integrate into DeferredRenderPipeline
6. Add UI controls
7. Update CMakeLists.txt
8. Create TestBloom
9. Test with DX11 and DX12
