# Anti-Aliasing (FXAA/SMAA)

Post-process anti-aliasing for the deferred rendering pipeline.

## Overview

Anti-aliasing smooths jagged edges (aliasing artifacts) that occur when rendering diagonal lines and high-contrast edges. This implementation provides two screen-space algorithms:

- **FXAA** (Fast Approximate Anti-Aliasing): Single-pass, fast (~0.5ms @ 1080p)
- **SMAA** (Subpixel Morphological Anti-Aliasing): 3-pass, higher quality (~1.5ms @ 1080p)

## Pipeline Position

```
HDR Buffer
    │
    ▼
┌─────────────────┐
│   Bloom Pass    │  (optional, half-res)
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ PostProcessPass │  HDR → LDR (Tonemapping + Color Grading)
│                 │  Output: R8G8B8A8_UNORM_SRGB
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ AntiAliasingPass│  ← FXAA/SMAA applied here (LDR input required)
└─────────────────┘
    │
    ▼
┌─────────────────┐
│  Debug Lines    │  (not anti-aliased, stays sharp)
│  Grid Pass      │
└─────────────────┘
    │
    ▼
   Output
```

**Key Design Decision**: AA runs after tonemapping (needs LDR) but before debug overlays (debug lines should remain sharp).

## Architecture

### Files

| File | Purpose |
|------|---------|
| `Engine/SceneLightSettings.h` | `EAntiAliasingMode` enum, `SAntiAliasingSettings` struct |
| `Engine/Rendering/AntiAliasingPass.h` | Pass class declaration |
| `Engine/Rendering/AntiAliasingPass.cpp` | Implementation + embedded shaders |
| `Editor/Panels_SceneLightSettings.cpp` | UI dropdown for AA mode selection |

### Class Design

```cpp
class CAntiAliasingPass {
public:
    bool Initialize();
    void Shutdown();
    void Render(ITexture* input, ITexture* output, uint32_t w, uint32_t h,
                const SAntiAliasingSettings& settings);
    bool IsEnabled(const SAntiAliasingSettings& settings) const;

private:
    // FXAA: single PSO
    RHI::PipelineStatePtr m_fxaaPSO;

    // SMAA: 3 PSOs + intermediate textures + lookup textures
    RHI::PipelineStatePtr m_smaaEdgePSO;
    RHI::PipelineStatePtr m_smaaBlendPSO;
    RHI::PipelineStatePtr m_smaaNeighborPSO;
    RHI::TexturePtr m_smaaEdgesTex;   // RG8
    RHI::TexturePtr m_smaaBlendTex;   // RGBA8
    RHI::TexturePtr m_smaaAreaTex;    // Pre-computed (160x560 RG8)
    RHI::TexturePtr m_smaaSearchTex;  // Pre-computed (64x16 R8)
};
```

### Settings

```cpp
enum class EAntiAliasingMode : int {
    Off = 0,    // No anti-aliasing
    FXAA = 1,   // Fast Approximate AA
    SMAA = 2    // Subpixel Morphological AA
};

struct SAntiAliasingSettings {
    EAntiAliasingMode mode = EAntiAliasingMode::Off;

    // FXAA-specific
    float fxaaSubpixelQuality = 0.75f;   // 0.0 (sharp) to 1.0 (soft)
    float fxaaEdgeThreshold = 0.166f;    // Edge detection sensitivity
    float fxaaEdgeThresholdMin = 0.0833f;
};
```

## FXAA Algorithm

FXAA 3.11 (NVIDIA) - Single-pass edge-aware blur.

### How It Works

1. **Luma Calculation**: Convert RGB to luminance
2. **Edge Detection**: Compare center pixel luma with 4 neighbors (N/S/E/W)
3. **Early Exit**: Skip low-contrast areas (no visible aliasing)
4. **Edge Direction**: Determine if edge is horizontal or vertical
5. **Subpixel Blending**: Sample along edge direction and blend

### Constant Buffer

```cpp
struct CB_FXAA {
    float2 rcpFrame;           // 1.0 / resolution
    float subpixelQuality;     // Blur amount (0-1)
    float edgeThreshold;       // Edge detection threshold
    float edgeThresholdMin;    // Minimum threshold
};
```

### Performance

- **Cost**: ~0.3-0.5ms @ 1080p
- **Texture Samples**: 9 per pixel
- **Quality**: Good for most cases, slight blur on text

## SMAA Algorithm

SMAA 1x - 3-pass morphological anti-aliasing.

### Pass 1: Edge Detection

- **Input**: LDR color texture
- **Output**: Edge texture (RG8)
- **Algorithm**: Luma-based edge detection with threshold

```hlsl
// Detect edges using luma difference
float L = dot(color, float3(0.2126, 0.7152, 0.0722));
float Lleft = dot(colorLeft, float3(0.2126, 0.7152, 0.0722));
float Ltop = dot(colorTop, float3(0.2126, 0.7152, 0.0722));
float2 edges = step(threshold, abs(float2(L - Lleft, L - Ltop)));
```

### Pass 2: Blend Weight Calculation

- **Input**: Edge texture + Area/Search lookup textures
- **Output**: Blend weight texture (RGBA8)
- **Algorithm**: Pattern matching against pre-computed lookup tables

This is the most complex pass. It:
1. Searches along edges to find crossing patterns
2. Looks up pre-computed blend weights from AreaTex
3. Uses SearchTex to accelerate the search

### Pass 3: Neighborhood Blending

- **Input**: Original LDR + Blend weights
- **Output**: Final anti-aliased image
- **Algorithm**: Weighted blend of neighboring pixels

```hlsl
float4 blendWeights = blendTex.Sample(sampler, uv);
float4 color = inputTex.Sample(sampler, uv);

// Blend with neighbors based on weights
color = lerp(color, topColor, blendWeights.y);
color = lerp(color, bottomColor, blendWeights.w);
color = lerp(color, leftColor, blendWeights.x);
color = lerp(color, rightColor, blendWeights.z);
```

### Lookup Textures

| Texture | Size | Format | Purpose |
|---------|------|--------|---------|
| AreaTex | 160x560 | RG8 | Pre-computed area coverage for edge patterns |
| SearchTex | 64x16 | R8 | Acceleration structure for edge search |

These are static data from the SMAA reference implementation (~180KB total).

### Performance

- **Cost**: ~1.0-1.5ms @ 1080p
- **Memory**: ~20MB intermediate textures @ 1080p
- **Quality**: Sharper than FXAA, better edge preservation

## Integration

### DeferredRenderPipeline Changes

```cpp
// In DeferredRenderPipeline.h
#include "AntiAliasingPass.h"

class CDeferredRenderPipeline {
    CAntiAliasingPass m_aaPass;
    RHI::TexturePtr m_offLDR_PreAA;  // Intermediate when AA enabled
};

// In DeferredRenderPipeline.cpp Render()
EAntiAliasingMode aaMode = ctx.scene.GetLightSettings().antiAliasing.mode;
bool aaEnabled = (aaMode != EAntiAliasingMode::Off);

// PostProcess outputs to intermediate if AA enabled
ITexture* postProcessOutput = aaEnabled ? m_offLDR_PreAA.get() : m_offLDR.get();
m_postProcess.Render(m_offHDR.get(), bloomResult, postProcessOutput, ...);

// Apply AA
if (aaEnabled) {
    CScopedDebugEvent evt(cmdList, L"Anti-Aliasing");
    m_aaPass.Render(m_offLDR_PreAA.get(), m_offLDR.get(),
                    ctx.width, ctx.height,
                    ctx.scene.GetLightSettings().antiAliasing);
}

// Debug lines render to m_offLDR (after AA)
```

### Editor UI

Add dropdown in Scene Light Settings panel:

```cpp
static void DrawAntiAliasingSection(CSceneLightSettings& settings) {
    SectionHeader("Post-Processing: Anti-Aliasing");

    auto& aa = settings.antiAliasing;

    const char* modeNames[] = { "Off", "FXAA", "SMAA" };
    int currentMode = static_cast<int>(aa.mode);

    ImGui::PushItemWidth(150);
    if (ImGui::Combo("Mode##AA", &currentMode, modeNames, 3)) {
        aa.mode = static_cast<EAntiAliasingMode>(currentMode);
    }
    ImGui::PopItemWidth();

    if (aa.mode == EAntiAliasingMode::FXAA) {
        ImGui::SliderFloat("Subpixel Quality##FXAA", &aa.fxaaSubpixelQuality, 0.0f, 1.0f);
    }

    HelpTooltip(
        "Off: No anti-aliasing\n"
        "FXAA: Fast approximate AA (~0.5ms, slight blur)\n"
        "SMAA: Morphological AA (~1.5ms, sharper edges)");
}
```

## Comparison

| Feature | FXAA | SMAA |
|---------|------|------|
| Passes | 1 | 3 |
| Cost @ 1080p | ~0.5ms | ~1.5ms |
| Edge Quality | Good | Excellent |
| Text Clarity | Slight blur | Sharp |
| Memory | Minimal | ~20MB |
| Complexity | Simple | Complex |

## When to Use

- **FXAA**: Default choice, good balance of quality and performance
- **SMAA**: When edge quality matters (architectural visualization, UI-heavy scenes)
- **Off**: When using TAA/DLSS (temporal AA handles aliasing)

## References

- [FXAA 3.11 Whitepaper](http://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf)
- [SMAA: Enhanced Subpixel Morphological Antialiasing](http://www.iryoku.com/smaa/)
- [SMAA Reference Implementation](https://github.com/iryoku/smaa)

## Testing

```bash
# Run AA test
./build/Debug/forfun.exe --test TestAntiAliasing
```

Test cases:
1. FXAA enabled - verify edge smoothing
2. SMAA enabled - verify 3-pass execution
3. Mode switching at runtime
4. Resolution change handling

---

**Last Updated**: 2026-01-15
