# Color Grading Post-Processing Effect

**Status**: ✅ **COMPLETED** (2026-01)

## Overview

Color grading post-processing effect applied after tone mapping in LDR space. Supports Lift/Gamma/Gain controls, saturation, contrast, temperature adjustments, and 3D LUT (Look-Up Table) for creative color looks.

**Dependencies**: Phase 3.2 G-Buffer (HDR intermediate buffer), PostProcessPass

---

## Architecture

```
HDR Buffer (after Bloom)
        │
        ▼
┌─────────────────────────────────────────────────────┐
│  PostProcessPass                                     │
│  ┌─────────────────────────────────────────────┐    │
│  │ 1. Sample HDR input                          │    │
│  │ 2. Add bloom contribution                    │    │
│  │ 3. Apply exposure                            │    │
│  │ 4. ACES tone mapping (HDR → LDR)             │    │
│  └─────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────┐    │
│  │ 5. COLOR GRADING (LDR space)                 │    │
│  │    a. Lift/Gamma/Gain (ASC CDL style)        │    │
│  │    b. Saturation adjustment                  │    │
│  │    c. Contrast adjustment                    │    │
│  │    d. Temperature (warm/cool)                │    │
│  │    e. 3D LUT application (optional)          │    │
│  └─────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────┐    │
│  │ 6. Output to sRGB (GPU auto gamma)           │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
        │
        ▼
    LDR Output (R8G8B8A8_UNORM_SRGB)
```

---

## Color Grading Pipeline

### 1. Lift/Gamma/Gain (ASC CDL Style)

Per-channel RGB controls for shadows, midtones, and highlights:

```hlsl
float3 ApplyLiftGammaGain(float3 color, float3 lift, float3 gamma, float3 gain) {
    // Lift: offset in shadows (add)
    color = color + lift * 0.1;

    // Gamma: power curve in midtones
    float3 gammaAdj = 1.0 / (1.0 + gamma);
    color = pow(max(color, 0.0001), gammaAdj);

    // Gain: multiply in highlights
    color = color * (1.0 + gain * 0.5);

    return color;
}
```

### 2. Saturation

Lerp between grayscale (luminance) and original color:

```hlsl
float3 ApplySaturation(float3 color, float saturation) {
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));  // Rec.709
    return lerp(float3(luma, luma, luma), color, 1.0 + saturation);
}
```

### 3. Contrast

Pivot around 0.5 midpoint:

```hlsl
float3 ApplyContrast(float3 color, float contrast) {
    return (color - 0.5) * (1.0 + contrast) + 0.5;
}
```

### 4. Temperature

Simple warm/cool shift on blue-orange axis:

```hlsl
float3 ApplyTemperature(float3 color, float temperature) {
    float3 warm = float3(1.05, 1.0, 0.95);
    float3 cool = float3(0.95, 1.0, 1.05);
    float3 tint = lerp(cool, warm, temperature * 0.5 + 0.5);
    return color * tint;
}
```

### 5. 3D LUT Application

32x32x32 lookup table for creative color looks:

```hlsl
float3 ApplyLUT(float3 color, float contribution) {
    if (contribution <= 0.0) return color;

    float lutSize = 32.0;
    float3 scale = (lutSize - 1.0) / lutSize;
    float3 offset = 0.5 / lutSize;
    float3 uvw = saturate(color) * scale + offset;

    float3 lutColor = lutTexture.Sample(samp, uvw).rgb;
    return lerp(color, lutColor, contribution);
}
```

---

## Files

### New Files

| File | Description |
|------|-------------|
| `Core/Loader/LUTLoader.h` | LUT data structure and loader declarations |
| `Core/Loader/LUTLoader.cpp` | .cube file parser and identity LUT generator |
| `Tests/TestColorGrading.cpp` | Automated test case |

### Modified Files

| File | Changes |
|------|---------|
| `Engine/SceneLightSettings.h` | Added `EColorGradingPreset` enum and `SColorGradingSettings` struct |
| `Engine/Rendering/ShowFlags.h` | Added `ColorGrading` flag |
| `Engine/Rendering/PostProcessPass.h` | Added color grading parameters and LUT members |
| `Engine/Rendering/PostProcessPass.cpp` | Integrated color grading shader |
| `Editor/Panels_SceneLightSettings.cpp` | Added color grading UI section |
| `Engine/Rendering/ForwardRenderPipeline.cpp` | Updated PostProcess call |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Updated PostProcess call |
| `CMakeLists.txt` | Added LUTLoader.cpp and TestColorGrading.cpp |

---

## Class Design

### EColorGradingPreset (SceneLightSettings.h)

```cpp
enum class EColorGradingPreset : int {
    Neutral = 0,   // No color grading (identity)
    Warm,          // Orange tint, slight saturation boost
    Cool,          // Blue tint, slight contrast boost
    Cinematic,     // Teal shadows, orange highlights, desaturated
    Custom         // User-defined or LUT-based
};
```

### SColorGradingSettings (SceneLightSettings.h)

```cpp
struct SColorGradingSettings {
    EColorGradingPreset preset = EColorGradingPreset::Neutral;
    std::string lutPath = "";  // Path to .cube file (Custom preset only)

    // Lift/Gamma/Gain (per-channel RGB, range -1 to +1)
    DirectX::XMFLOAT3 lift = {0.0f, 0.0f, 0.0f};   // Shadows
    DirectX::XMFLOAT3 gamma = {0.0f, 0.0f, 0.0f};  // Midtones
    DirectX::XMFLOAT3 gain = {0.0f, 0.0f, 0.0f};   // Highlights

    // Simple adjustments (range -1 to +1)
    float saturation = 0.0f;
    float contrast = 0.0f;
    float temperature = 0.0f;  // Negative = cool, Positive = warm

    void ApplyPreset(EColorGradingPreset newPreset);
};
```

### Preset Values

| Preset | Temperature | Saturation | Contrast | Lift | Gamma | Gain |
|--------|-------------|------------|----------|------|-------|------|
| Neutral | 0.0 | 0.0 | 0.0 | (0,0,0) | (0,0,0) | (0,0,0) |
| Warm | 0.3 | 0.1 | 0.0 | (0,0,0) | (0,0,0) | (0,0,0) |
| Cool | -0.3 | 0.0 | 0.1 | (0,0,0) | (0,0,0) | (0,0,0) |
| Cinematic | 0.05 | -0.1 | 0.15 | (0,-0.02,-0.05) | (0,0,0) | (0.05,0.02,-0.02) |

### SLUTData (LUTLoader.h)

```cpp
struct SLUTData {
    uint32_t size = 0;           // LUT dimension (e.g., 32 for 32x32x32)
    std::vector<float> data;     // RGB triplets (size^3 * 3 floats)
    float domainMin[3] = {0, 0, 0};
    float domainMax[3] = {1, 1, 1};
};
```

### LUT Functions (LUTLoader.h)

```cpp
// Load .cube file (Adobe/Resolve format)
bool LoadCubeFile(const std::string& path, SLUTData& outLUT);

// Generate identity LUT (output = input)
void GenerateIdentityLUT(uint32_t size, SLUTData& outLUT);
```

---

## .cube File Format

The `.cube` format is an industry-standard 3D LUT format used by Adobe, DaVinci Resolve, and other color grading software.

### Format Specification

```
# Comment lines start with #
TITLE "My LUT"
DOMAIN_MIN 0.0 0.0 0.0
DOMAIN_MAX 1.0 1.0 1.0
LUT_3D_SIZE 32

# RGB triplets (R varies fastest, then G, then B)
0.000000 0.000000 0.000000
0.031250 0.000000 0.000000
0.062500 0.000000 0.000000
...
```

### Supported Features

- `LUT_3D_SIZE`: Required, specifies cube dimension (2-256)
- `DOMAIN_MIN`/`DOMAIN_MAX`: Optional, defaults to 0.0-1.0
- `TITLE`: Optional, ignored
- Comments: Lines starting with `#`
- 1D LUTs: Not supported (error returned)

### Limitations

- Only 32x32x32 LUTs are currently supported
- Values outside [0,1] are clamped with warning

---

## Pipeline Integration

### PostProcessPass::Render()

```cpp
void CPostProcessPass::Render(
    ITexture* hdrInput,
    ITexture* bloomTexture,
    ITexture* ldrOutput,
    uint32_t width, uint32_t height,
    float exposure,
    float bloomIntensity,
    const SColorGradingSettings* colorGrading,  // NEW
    bool colorGradingEnabled);                   // NEW
```

### DeferredRenderPipeline.cpp

```cpp
// Post-Processing (HDR -> LDR with Color Grading)
if (ctx.showFlags.PostProcessing) {
    CScopedDebugEvent evt(cmdList, L"Post-Processing");
    const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
    const auto& colorGrading = ctx.scene.GetLightSettings().colorGrading;

    m_postProcess.Render(
        m_offHDR.get(),
        bloomResult,
        m_offLDR.get(),
        ctx.width, ctx.height,
        1.0f,  // exposure
        bloomSettings.enabled ? bloomSettings.intensity : 0.0f,
        &colorGrading,
        ctx.showFlags.ColorGrading);
}
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Located in "Post-Processing: Color Grading" section:

### Main Controls
- **Enable**: Toggle color grading on/off
- **Preset**: Dropdown (Neutral, Warm, Cool, Cinematic, Custom)
- **LUT File**: Text input for .cube path (Custom preset only)

### Adjustments
- **Saturation**: -1.0 (grayscale) to +1.0 (oversaturated)
- **Contrast**: -1.0 (flat) to +1.0 (high contrast)
- **Temperature**: -1.0 (cool/blue) to +1.0 (warm/orange)

### Advanced (Collapsible)
- **Lift (Shadows)**: RGB sliders (-1 to +1)
- **Gamma (Midtones)**: RGB sliders (-1 to +1)
- **Gain (Highlights)**: RGB sliders (-1 to +1)
- **Reset LGG**: Button to reset all LGG values to 0

---

## Constant Buffer Layout

```cpp
struct CB_PostProcess {
    float exposure;
    float bloomIntensity;
    float _pad0[2];

    DirectX::XMFLOAT3 lift;
    float saturation;

    DirectX::XMFLOAT3 gamma;
    float contrast;

    DirectX::XMFLOAT3 gain;
    float temperature;

    float lutContribution;
    int colorGradingEnabled;
    float _pad1[2];
};
```

---

## Test Case

**File**: `Tests/TestColorGrading.cpp`

```bash
timeout 30 E:/forfun/source/code/build/Debug/forfun.exe --test TestColorGrading
```

### Test Sequence

| Frame | Action | Expected Result |
|-------|--------|-----------------|
| 1 | Create test scene | 3 colored spheres + ground |
| 5 | Enable Neutral preset | No visible change |
| 15 | Screenshot | Baseline |
| 20 | Switch to Warm | Orange tint |
| 25 | Screenshot | Warm look |
| 30 | Switch to Cool | Blue tint |
| 35 | Screenshot | Cool look |
| 40 | Switch to Cinematic | Teal/orange, high contrast |
| 45 | Screenshot | Cinematic look |
| 50 | Custom LGG values | Color shift |
| 55 | Screenshot | LGG effect |
| 60 | High sat/contrast | Extreme values |
| 65 | Screenshot | Saturated look |
| 70 | Disable color grading | Normal rendering |
| 75 | Screenshot | No color grading |
| 80 | Test complete | Pass/Fail |

---

## Performance

### Memory Usage

| Resource | Size | Format |
|----------|------|--------|
| Neutral LUT | 512 KB | R32G32B32A32_FLOAT (32^3 × 16 bytes) |
| Custom LUT | 512 KB | R32G32B32A32_FLOAT (32^3 × 16 bytes) |

### GPU Cost

- Color grading adds minimal overhead (~0.1ms at 1080p)
- All operations are simple ALU (no texture fetches except LUT)
- LUT sampling uses trilinear filtering (single texture fetch)

---

## Implementation Notes

1. **LDR Space**: Color grading is applied after tone mapping to avoid HDR artifacts

2. **Order of Operations**: Lift/Gamma/Gain → Saturation → Contrast → Temperature → LUT

3. **LUT Caching**: Custom LUT is only reloaded when path changes

4. **Neutral LUT**: Identity LUT created at initialization for fallback

5. **Preset Auto-Switch**: Modifying any slider automatically switches to Custom preset

6. **DX11/DX12 Compatibility**: Uses RHI abstraction for all resource operations

7. **3D Texture**: LUT stored as `Texture3D` with `R32G32B32A32_FLOAT` format

---

## Future Improvements

- [ ] Support for different LUT sizes (17, 33, 64, 65)
- [ ] LUT resampling for non-32 sizes
- [ ] HDR LUT support (before tone mapping)
- [ ] Color wheels UI for Lift/Gamma/Gain
- [ ] Vignette effect integration
- [ ] Film grain effect
