# Auto Exposure (Eye Adaptation)

**Status**: ✅ **COMPLETED** (2026-01)

## Overview

Histogram-based automatic exposure adjustment that simulates human eye adaptation. Analyzes scene luminance distribution and smoothly adjusts exposure to maintain proper brightness across varying lighting conditions.

**Dependencies**: Phase 3.2 G-Buffer (HDR intermediate buffer), PostProcessPass

**Reference**: [Automatic Exposure - Krzysztof Narkowicz](https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/)

---

## Architecture

```
HDR Buffer (after Lighting/Transparent Pass)
        │
        ▼
┌─────────────────────────────────────────────────────┐
│  AutoExposurePass                                   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 1. Histogram Pass (Compute Shader)           │   │
│  │    - Build 256-bin luminance histogram       │   │
│  │    - Log2 luminance scale [-8, +4] EV        │   │
│  │    - Center-weighted metering (Gaussian)     │   │
│  │    - Shared memory accumulation              │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 2. Adaptation Pass (Compute Shader)          │   │
│  │    - Parallel reduction (256 threads)        │   │
│  │    - Calculate weighted average luminance    │   │
│  │    - Compute target exposure (middle gray)   │   │
│  │    - Asymmetric temporal smoothing           │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 3. Debug Overlay (Pixel Shader, optional)    │   │
│  │    - Render histogram visualization          │   │
│  │    - Show current/target exposure            │   │
│  └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
        │
        ▼ (Exposure buffer: [current, target, maxBin])
┌─────────────────────────────────────────────────────┐
│  PostProcessPass                                    │
│  - Read exposure from GPU buffer (GPU-only path)   │
│  - Apply exposure before tonemapping               │
│  - ACES filmic tonemapping                         │
│  - Output to LDR                                   │
└─────────────────────────────────────────────────────┘
        │
        ▼
    LDR Output
```

---

## Algorithm

### 1. Histogram Generation

Each pixel's luminance is mapped to one of 256 bins in log2 space:

```
Luminance Range: [2^-8, 2^4] = [0.004, 16]
Log2 Range: [-8, +4] EV

binIndex = saturate((log2(luminance) - minLog) / (maxLog - minLog)) * 255
```

**Center Weighting**: Gaussian falloff from screen center (configurable via `centerWeight`):
```hlsl
float2 offset = (uv - 0.5) * 2.0;  // [-1, 1]
float distSq = dot(offset, offset);
float gaussianWeight = exp(-distSq / (2.0 * sigma * sigma));
return lerp(1.0, gaussianWeight, gCenterWeight);
```

### 2. Average Luminance Calculation

Parallel reduction across 256 threads computes weighted geometric mean:
```hlsl
avgLogLum = sum(weight[i] * log2(luminance[i])) / sum(weight[i])
avgLuminance = exp2(avgLogLum)
```

### 3. Target Exposure

Exposure is calculated to bring average luminance to middle gray (0.18):
```hlsl
targetExposure = 0.18 / avgLuminance
targetExposure *= exp2(exposureCompensation)  // User adjustment in EV
targetExposure = clamp(targetExposure, minExposure, maxExposure)
```

### 4. Temporal Adaptation

Asymmetric exponential smoothing simulates eye adaptation:
- **Dark → Bright**: Faster adaptation (eyes adjust quickly to bright light)
- **Bright → Dark**: Slower adaptation (eyes take longer in darkness)

```hlsl
float adaptSpeed = (targetExposure > currentExposure) ? adaptSpeedUp : adaptSpeedDown;
float alpha = 1.0 - exp(-deltaTime / adaptSpeed);
newExposure = lerp(currentExposure, targetExposure, alpha);
```

---

## Files

| File | Description |
|------|-------------|
| `Engine/Rendering/AutoExposurePass.h` | Class header, constant buffer structs |
| `Engine/Rendering/AutoExposurePass.cpp` | Implementation, debug shaders (embedded) |
| `Shader/AutoExposure.cs.hlsl` | Compute shaders (histogram, adaptation) |
| `Tests/TestAutoExposure.cpp` | Automated test case |

---

## Class Design

### SAutoExposureSettings (SceneLightSettings.h)
```cpp
struct SAutoExposureSettings {
    float minEV = -4.0f;              // Minimum exposure (EV stops)
    float maxEV = 4.0f;               // Maximum exposure (EV stops)
    float adaptSpeedUp = 1.0f;        // Dark→Bright adaptation (seconds)
    float adaptSpeedDown = 1.5f;      // Bright→Dark adaptation (seconds)
    float exposureCompensation = 0.0f; // Manual adjustment (EV stops)
    float centerWeight = 0.5f;        // Center metering weight (0-1)
};
```

### CB_AutoExposure (Constant Buffer)
```cpp
struct alignas(16) CB_AutoExposure {
    DirectX::XMFLOAT2 screenSize;
    DirectX::XMFLOAT2 rcpScreenSize;
    float minLogLuminance;    // -8.0 (log2)
    float maxLogLuminance;    // +4.0 (log2)
    float centerWeight;
    float deltaTime;
    float minExposure;        // pow(2, minEV)
    float maxExposure;        // pow(2, maxEV)
    float adaptSpeedUp;
    float adaptSpeedDown;
    float exposureCompensation;
    float _pad0, _pad1, _pad2;
};
```

### CAutoExposurePass
```cpp
class CAutoExposurePass {
public:
    bool Initialize();
    void Shutdown();

    // Compute exposure from HDR scene luminance
    void Render(RHI::ICommandList* cmdList,
                RHI::ITexture* hdrInput,
                uint32_t width, uint32_t height,
                float deltaTime,
                const SAutoExposureSettings& settings);

    // Render debug histogram overlay (on LDR target)
    void RenderDebugOverlay(RHI::ICommandList* cmdList,
                           RHI::ITexture* renderTarget,
                           uint32_t width, uint32_t height);

    // Get exposure buffer for GPU-only path (bind to tonemapping shader)
    // Buffer: [0]=current, [1]=target, [2]=maxHistogramValue
    RHI::IBuffer* GetExposureBuffer() const;

private:
    // Compute shaders
    RHI::ShaderPtr m_histogramCS;
    RHI::ShaderPtr m_adaptationCS;

    // Debug visualization shaders (embedded)
    RHI::ShaderPtr m_debugVS;
    RHI::ShaderPtr m_debugPS;

    // Pipeline states
    RHI::PipelineStatePtr m_histogramPSO;
    RHI::PipelineStatePtr m_adaptationPSO;
    RHI::PipelineStatePtr m_debugPSO;

    // Buffers
    RHI::BufferPtr m_histogramBuffer;    // UAV: 256 uint32_t bins
    RHI::BufferPtr m_exposureBuffer;     // UAV: 3 floats [current, target, maxBin]
    RHI::BufferPtr m_histogramReadback;  // Staging (future CPU readback)
    RHI::BufferPtr m_exposureReadback;   // Staging (future CPU readback)
};
```

---

## Shader Implementation

### AutoExposure.cs.hlsl

#### CSBuildHistogram
- **Dispatch**: `(width/16, height/16, 1)` - 16×16 threads per group
- **Shared Memory**: 256 uint32_t bins for local accumulation
- **Algorithm**:
  1. Clear shared memory histogram
  2. Each thread samples one pixel, computes luminance
  3. Map luminance to bin index (log2 scale)
  4. Apply center weight, scale to integer
  5. Atomic add to shared memory
  6. Sync, then atomic add to global histogram

#### CSAdaptExposure
- **Dispatch**: `(1, 1, 1)` - Single group, 256 threads
- **Shared Memory**: Partial sums, weights, max values
- **Algorithm**:
  1. Each thread loads one histogram bin
  2. Parallel reduction for sum and max
  3. Thread 0 computes average luminance
  4. Calculate target exposure (middle gray = 0.18)
  5. Apply asymmetric temporal smoothing
  6. Write results: `[current, target, maxBin]`

---

## GPU-Only Path

The exposure value is computed entirely on GPU and passed directly to the tonemapping shader without CPU readback:

```cpp
// DeferredRenderPipeline.cpp
RHI::IBuffer* exposureBuffer = nullptr;
if (ctx.showFlags.AutoExposure) {
    m_autoExposurePass.Render(cmdList, m_offHDR.get(), ...);
    exposureBuffer = m_autoExposurePass.GetExposureBuffer();
}

// PostProcessPass reads exposure from GPU buffer
m_postProcess.Render(hdrInput, bloomTexture, ldrOutput,
                     width, height, 1.0f, exposureBuffer, ...);
```

**PostProcess Shader**:
```hlsl
StructuredBuffer<float> exposureBuffer : register(t3);

float exposure = gExposure;  // Fallback
if (gUseExposureBuffer) {
    exposure = exposureBuffer[0];  // GPU-computed value
}
hdrColor *= exposure;
```

---

## Pipeline Integration

### DeferredRenderPipeline.cpp
```cpp
// ============================================
// 7. Auto Exposure (HDR luminance analysis)
// ============================================
RHI::IBuffer* exposureBuffer = nullptr;
if (ctx.showFlags.AutoExposure) {
    const auto& aeSettings = ctx.scene.GetLightSettings().autoExposure;
    CScopedDebugEvent evt(cmdList, L"Auto Exposure");
    m_autoExposurePass.Render(cmdList, m_offHDR.get(), ctx.width, ctx.height,
                              ctx.deltaTime, aeSettings);
    exposureBuffer = m_autoExposurePass.GetExposureBuffer();
}

// ... Bloom Pass ...

// ============================================
// 10. Post-Processing (HDR -> LDR)
// ============================================
if (ctx.showFlags.PostProcessing) {
    CScopedDebugEvent evt(cmdList, L"Post-Processing");
    m_postProcess.Render(hdrAfterMotionBlur, bloomResult, m_offLDR.get(),
                         ctx.width, ctx.height, 1.0f, exposureBuffer, bloomIntensity,
                         &ctx.scene.GetLightSettings().colorGrading,
                         ctx.showFlags.ColorGrading);
}

// ============================================
// 12. Auto Exposure Debug Overlay
// ============================================
if (ctx.showFlags.AutoExposure) {
    CScopedDebugEvent evt(cmdList, L"Auto Exposure Debug");
    m_autoExposurePass.RenderDebugOverlay(cmdList, m_offLDR.get(), ctx.width, ctx.height);
}
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Located in "Post-Processing: Auto Exposure" section:
- **Enable**: Toggle auto exposure on/off (ShowFlags.AutoExposure)
- **Min EV**: Minimum exposure limit (-8 to 0)
- **Max EV**: Maximum exposure limit (0 to 8)
- **Adapt Speed Up**: Dark→Bright transition time (0.1 - 5.0 seconds)
- **Adapt Speed Down**: Bright→Dark transition time (0.1 - 5.0 seconds)
- **Exposure Compensation**: Manual adjustment (-3 to +3 EV)
- **Center Weight**: Metering center bias (0 = uniform, 1 = center-only)

---

## Test Case

**File**: `Tests/TestAutoExposure.cpp`

```bash
timeout 30 E:/forfun/source/code/build/Debug/forfun.exe --test TestAutoExposure
```

Test scenarios:
1. Moderate lighting (intensity 3.0) - baseline
2. Bright scene (intensity 15.0) - exposure should decrease
3. Dark scene (intensity 0.5) - exposure should increase
4. Disabled comparison - shows raw dark scene

---

## Performance

| Component | Cost |
|-----------|------|
| Histogram Pass | ~0.1ms (compute, 16×16 groups) |
| Adaptation Pass | ~0.01ms (single dispatch, 256 threads) |
| Debug Overlay | ~0.05ms (fullscreen triangle, optional) |

**Memory**:
| Buffer | Size |
|--------|------|
| Histogram | 1 KB (256 × 4 bytes) |
| Exposure | 12 bytes (3 floats) |
| Readback (staging) | 1 KB + 12 bytes |

---

## Configuration Constants

```cpp
namespace AutoExposureConfig {
    constexpr uint32_t HISTOGRAM_BINS = 256;
    constexpr uint32_t HISTOGRAM_THREAD_GROUP_SIZE = 16;
    constexpr float MIN_LOG_LUMINANCE = -8.0f;  // log2(1/256)
    constexpr float MAX_LOG_LUMINANCE = 4.0f;   // log2(16)
}
```

---

## Implementation Notes

1. **GPU-Only Path**: Exposure is computed on GPU and passed directly to tonemapping shader via structured buffer. No CPU readback required.

2. **Center Weighting**: Gaussian falloff prioritizes center of screen (like camera center-weighted metering).

3. **Asymmetric Adaptation**: Mimics human vision - faster adaptation to bright light, slower to darkness.

4. **Integer Weight Scaling**: Histogram weights are scaled by 1000 to preserve precision in integer atomics.

5. **First Frame Detection**: Shader detects `exposure <= 0` to initialize on first frame.

6. **Max Histogram Value**: Computed via parallel reduction and stored in exposure buffer for debug visualization (eliminates O(256) per-pixel loop).

7. **Resource Barriers**: UAV barriers between histogram clear, build, and adaptation passes.

8. **DX11/DX12 Compatibility**: Uses RHI abstraction for all resource operations.
