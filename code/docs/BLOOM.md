# Bloom Post-Processing Effect

**Status**: ✅ **COMPLETED** (2025-12)

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

## Files

| File | Description |
|------|-------------|
| `Engine/Rendering/BloomPass.h` | BloomPass class header |
| `Engine/Rendering/BloomPass.cpp` | BloomPass implementation (embedded shaders) |
| `Tests/TestBloom.cpp` | Automated test case |

**Note**: Shaders are embedded directly in `BloomPass.cpp` rather than separate `.hlsl` files.

---

## Modified Files

| File | Changes |
|------|---------|
| `Engine/SceneLightSettings.h` | Added `SBloomSettings` struct |
| `Engine/Rendering/PostProcessPass.h` | Added bloom texture input parameter |
| `Engine/Rendering/PostProcessPass.cpp` | Composite bloom in shader |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.h` | Added `CBloomPass` member |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Integrated bloom pass |
| `Editor/Panels_SceneLightSettings.cpp` | Added bloom UI controls |
| `CMakeLists.txt` | Added `BloomPass.cpp` |

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

    // Returns bloom texture (half resolution) or nullptr if disabled
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          uint32_t width, uint32_t height,
                          const SBloomSettings& settings);

    // Get the final bloom result texture (half resolution)
    RHI::ITexture* GetBloomTexture() const;

private:
    static constexpr int kMipCount = 5;

    // Mip chain: R16G16B16A16_FLOAT for HDR precision
    RHI::TexturePtr m_mipChain[kMipCount];
    uint32_t m_mipWidth[kMipCount];
    uint32_t m_mipHeight[kMipCount];

    // Shaders (embedded in .cpp)
    RHI::ShaderPtr m_fullscreenVS;
    RHI::ShaderPtr m_thresholdPS;
    RHI::ShaderPtr m_downsamplePS;
    RHI::ShaderPtr m_upsamplePS;

    // PSOs
    RHI::PipelineStatePtr m_thresholdPSO;
    RHI::PipelineStatePtr m_downsamplePSO;
    RHI::PipelineStatePtr m_upsamplePSO;       // Opaque write
    RHI::PipelineStatePtr m_upsampleBlendPSO;  // Additive blend

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::SamplerPtr m_linearSampler;
    RHI::TexturePtr m_blackTexture;  // Fallback when disabled

    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;
};
```

---

## Shader Implementation (Embedded)

### Threshold Shader
- Soft threshold with knee for smooth falloff
- Rec.709 luminance: `0.2126*R + 0.7152*G + 0.0722*B`
- Firefly clamping: `min(bloom, 10.0)`

### Downsample Shader
- 5-tap Kawase: center × 4 + 4 diagonals, divide by 8

### Upsample Shader
- 9-tap tent filter
- `gScatter` parameter controls contribution from lower mip
- Additive blend for accumulation

---

## Pipeline Integration

### DeferredRenderPipeline.cpp
```cpp
// ============================================
// 7. Bloom Pass (between Transparent and PostProcess)
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
// 8. Post-Processing (HDR -> LDR)
// ============================================
if (ctx.showFlags.PostProcessing) {
    CScopedDebugEvent evt(cmdList, L"Post-Processing");
    const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
    m_postProcess.Render(
        m_offHDR.get(),
        bloomResult,  // can be nullptr
        m_offLDR.get(),
        ctx.width, ctx.height,
        1.0f,  // exposure
        bloomSettings.enabled ? bloomSettings.intensity : 0.0f);
}
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Located in "Post-Processing: Bloom" section:
- **Enable**: Toggle bloom on/off
- **Threshold**: Luminance cutoff (0.0 - 5.0)
- **Intensity**: Bloom brightness multiplier (0.0 - 3.0)
- **Scatter**: Blend factor between blur levels (0.0 - 1.0)

---

## Test Case

**File**: `Tests/TestBloom.cpp`

```bash
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestBloom
```

Test captures screenshots with bloom enabled/disabled for visual comparison.

---

## Performance

| Resolution | Mip Chain Memory | Actual Format |
|------------|-----------------|---------------|
| 1920×1080  | ~10 MB          | R16G16B16A16_FLOAT |
| 2560×1440  | ~18 MB          | R16G16B16A16_FLOAT |
| 3840×2160  | ~40 MB          | R16G16B16A16_FLOAT |

**Note**: Using `R16G16B16A16_FLOAT` instead of `R11G11B10_FLOAT` for HDR precision.

---

## Implementation Notes

1. **Texture Format**: `R16G16B16A16_FLOAT` for mip chain (HDR precision)

2. **Mip Sizing**:
   - Mip[0]: width/2 × height/2 (threshold output)
   - Mip[1-4]: successive halves

3. **Upsample Blend**: Additive blend state for accumulating during upsample chain

4. **Resource Hazards**: `UnbindRenderTargets()` before using RT as SRV

5. **Fallback**: 1×1 black texture returned when bloom is disabled

6. **DX11/DX12 Compatibility**: Uses RHI abstraction for all resource operations
