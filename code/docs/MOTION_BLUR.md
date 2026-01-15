# Motion Blur Post-Processing Effect

**Status**: Completed (2026-01)

## Overview

Camera motion blur effect using **linear velocity sampling** along the velocity direction. Reads velocity vectors from G-Buffer and blurs the HDR image along the motion direction.

**Dependencies**: Phase 3.2 G-Buffer (Velocity buffer RT4)

---

## Architecture

```
HDR Buffer (after Auto Exposure)
        |
        v
+-------------------------------------------------------+
|  MotionBlurPass                                       |
|  +-----------------------------------------------+    |
|  | 1. Sample velocity from G-Buffer RT4          |    |
|  |    (R16G16_FLOAT, UV-space motion vectors)    |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | 2. Early-out if velocity < threshold          |    |
|  |    (Skip blur for static pixels)              |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | 3. Clamp velocity to max blur radius          |    |
|  |    (Prevent excessive blur artifacts)         |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | 4. Sample HDR along velocity direction        |    |
|  |    (N samples with tent filter weighting)     |    |
|  +-----------------------------------------------+    |
+-------------------------------------------------------+
        |
        v (Motion-blurred HDR texture)
+-------------------------------------------------------+
|  BloomPass (optional)                                 |
+-------------------------------------------------------+
        |
        v
+-------------------------------------------------------+
|  PostProcessPass                                      |
|  - Tonemapping                                        |
|  - Gamma correction                                   |
+-------------------------------------------------------+
        |
        v
    LDR Output
```

---

## Linear Blur Algorithm

### Velocity Sampling
- Read velocity from G-Buffer RT4 (UV-space motion vectors)
- Scale by intensity parameter
- Clamp magnitude to max blur radius (in pixels)

### Sample Distribution
Sample along velocity direction from -0.5 to +0.5:
```
    t = -0.5                t = 0                t = +0.5
       |--------------------[center]------------------|
       ^                      ^                       ^
    sample 0              sample N/2              sample N-1
```

### Tent Filter Weighting
Weight samples by distance from center:
```
weight = 1.0 - abs(t * 2.0)

    1.0  |        /\
         |       /  \
         |      /    \
    0.0  |-----/------\-----
        -0.5   0.0   +0.5
```

---

## Files

| File | Description |
|------|-------------|
| `Engine/Rendering/MotionBlurPass.h` | MotionBlurPass class header |
| `Engine/Rendering/MotionBlurPass.cpp` | MotionBlurPass implementation (embedded shaders) |
| `Tests/TestMotionBlur.cpp` | Automated test case |

**Note**: Shaders are embedded directly in `MotionBlurPass.cpp` rather than separate `.hlsl` files.

---

## Modified Files

| File | Changes |
|------|---------|
| `Engine/SceneLightSettings.h` | Added `SMotionBlurSettings` struct |
| `Engine/Rendering/ShowFlags.h` | Added `MotionBlur` flag |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.h` | Added `CMotionBlurPass` member and accessor |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Integrated motion blur pass |
| `Editor/Panels_SceneLightSettings.cpp` | Added motion blur UI controls |
| `CMakeLists.txt` | Added `MotionBlurPass.h/cpp` and `TestMotionBlur.cpp` |

---

## Class Design

### SMotionBlurSettings (SceneLightSettings.h)
```cpp
struct SMotionBlurSettings
{
    float intensity = 0.5f;      // Blur strength multiplier (0-2)
    int sampleCount = 12;        // Samples along velocity (4-32)
    float maxBlurPixels = 32.0f; // Maximum blur radius in pixels (8-128)
};
```

### CMotionBlurPass (MotionBlurPass.h)
```cpp
class CMotionBlurPass {
public:
    bool Initialize();
    void Shutdown();

    // Returns motion-blurred texture or hdrInput if disabled/error
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          RHI::ITexture* velocityBuffer,
                          uint32_t width, uint32_t height,
                          const SMotionBlurSettings& settings);

    RHI::ITexture* GetOutputTexture() const;

private:
    // Output texture (R16G16B16A16_FLOAT, same as input)
    RHI::TexturePtr m_outputHDR;

    // Shaders (embedded in .cpp)
    RHI::ShaderPtr m_fullscreenVS;
    RHI::ShaderPtr m_motionBlurPS;

    // Pipeline state
    RHI::PipelineStatePtr m_pso;

    // Resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::SamplerPtr m_linearSampler;  // For HDR input
    RHI::SamplerPtr m_pointSampler;   // For velocity buffer

    uint32_t m_cachedWidth = 0;
    uint32_t m_cachedHeight = 0;
};
```

---

## Shader Implementation (Embedded)

### Vertex Shader
Standard fullscreen quad vertex shader (NDC space).

### Pixel Shader
```hlsl
float4 main(PSIn input) : SV_Target {
    // Sample velocity (UV-space motion vector)
    float2 velocity = gVelocityBuffer.SampleLevel(gPointSampler, input.uv, 0).rg;
    velocity *= gIntensity;

    // Early out if velocity is negligible
    float velocityMag = length(velocity);
    if (velocityMag < 0.0001) {
        return gHDRInput.SampleLevel(gLinearSampler, input.uv, 0);
    }

    // Clamp velocity to max blur radius
    float2 texSize;
    gHDRInput.GetDimensions(texSize.x, texSize.y);
    float2 maxBlurUV = gMaxBlurPixels / texSize;
    if (velocityMag > length(maxBlurUV)) {
        velocity = normalize(velocity) * length(maxBlurUV);
    }

    // Accumulate samples along velocity direction
    float3 color = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < gSampleCount; ++i) {
        float t = (float)i / (float)(gSampleCount - 1) - 0.5;
        float2 sampleUV = saturate(input.uv + velocity * t);
        float3 sampleColor = gHDRInput.SampleLevel(gLinearSampler, sampleUV, 0).rgb;
        float weight = 1.0 - abs(t * 2.0);  // Tent filter
        color += sampleColor * weight;
        totalWeight += weight;
    }

    return float4(color / totalWeight, 1.0);
}
```

**Important**: Uses `SampleLevel()` instead of `Sample()` to avoid gradient instruction issues in dynamic loops.

---

## Pipeline Integration

### DeferredRenderPipeline.cpp
```cpp
// ============================================
// 7. Motion Blur Pass (after Auto Exposure, before Bloom)
// ============================================
ITexture* hdrAfterMotionBlur = m_offHDR.get();
if (ctx.showFlags.MotionBlur) {
    const auto& mbSettings = ctx.scene.GetLightSettings().motionBlur;
    CScopedDebugEvent evt(cmdList, L"Motion Blur");
    hdrAfterMotionBlur = m_motionBlurPass.Render(
        m_offHDR.get(),
        m_gbuffer.GetVelocity(),
        ctx.width, ctx.height,
        mbSettings);
}

// ============================================
// 8. Bloom Pass (uses motion-blurred HDR)
// ============================================
RHI::ITexture* bloomResult = nullptr;
if (ctx.showFlags.PostProcessing) {
    const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
    if (bloomSettings.enabled) {
        CScopedDebugEvent evt(cmdList, L"Bloom");
        bloomResult = m_bloomPass.Render(
            hdrAfterMotionBlur, ctx.width, ctx.height, bloomSettings);
    }
}
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Located in "Post-Processing: Motion Blur" section:
- **Enable**: Toggle motion blur on/off (via FShowFlags.MotionBlur)
- **Intensity**: Blur strength multiplier (0.0 - 2.0)
- **Sample Count**: Number of samples along velocity (4 - 32)
- **Max Blur Pixels**: Maximum blur radius in pixels (8.0 - 128.0)

---

## Test Case

**File**: `Tests/TestMotionBlur.cpp`

```bash
timeout 30 E:/forfun/source/code/build/Debug/forfun.exe --test TestMotionBlur
```

Test sequence:
1. Frame 1: Create test scene with spheres and cubes
2. Frame 5: Capture static scene (no motion blur)
3. Frame 10: Enable motion blur
4. Frame 11-20: Rotate camera to generate velocity vectors
5. Frame 15: Capture motion blur during rotation
6. Frame 25-26: Test high intensity motion blur
7. Frame 30-31: Test low intensity motion blur
8. Frame 35-36: Disable motion blur, verify no blur despite camera movement
9. Frame 40: Test complete

---

## Performance

| Resolution | Memory Usage | Notes |
|------------|--------------|-------|
| 1920x1080  | ~16 MB       | Single R16G16B16A16_FLOAT output |
| 2560x1440  | ~28 MB       | Single R16G16B16A16_FLOAT output |
| 3840x2160  | ~64 MB       | Single R16G16B16A16_FLOAT output |

**Sample Count Impact**:
- 8 samples: Minimal quality, fastest
- 12 samples: Good balance (default)
- 16-32 samples: High quality, more GPU cost

---

## Implementation Notes

1. **Velocity Buffer**: Uses G-Buffer RT4 (R16G16_FLOAT) which stores UV-space motion vectors computed from current and previous frame view-projection matrices.

2. **Camera Motion Only**: Current implementation only captures camera motion. Per-object motion blur would require storing per-object velocity in the G-Buffer pass.

3. **SampleLevel vs Sample**: Must use `SampleLevel()` in the blur loop because `Sample()` uses gradient instructions which cannot be used in loops with dynamic iteration counts.

4. **Early-Out Optimization**: Pixels with negligible velocity skip the blur loop entirely, improving performance for mostly static scenes.

5. **Max Blur Clamping**: Prevents excessive blur artifacts by clamping velocity magnitude to a configurable maximum.

6. **Tent Filter**: Weights samples by distance from center, giving more importance to the current pixel position and creating smoother blur trails.

7. **Pipeline Order**: Motion blur is applied after Auto Exposure but before Bloom, ensuring correct HDR values are blurred.

---

## Future Improvements

1. **Per-Object Motion Blur**: Store per-object velocity in G-Buffer for moving objects
2. **Tile-Based Optimization**: Skip tiles with no velocity for better performance
3. **Depth-Aware Blur**: Prevent background bleeding into foreground
4. **Variable Sample Count**: Adaptive sampling based on velocity magnitude
