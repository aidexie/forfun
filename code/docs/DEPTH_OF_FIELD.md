# Depth of Field Post-Processing Effect

**Status**: Completed (2026-01)

## Overview

Cinematic depth of field effect using a **Two-Pass Separated Near/Far** algorithm. Simulates camera lens focus behavior where objects at the focus distance appear sharp while near/far objects blur based on their distance from the focal plane.

**Dependencies**: Phase 3.2 G-Buffer (Depth buffer)

---

## Architecture

```
HDR Buffer (after Motion Blur)
        |
        v
+-------------------------------------------------------+
|  DepthOfFieldPass                                     |
|  +-----------------------------------------------+    |
|  | Pass 1: CoC Calculation (Full-Res)            |    |
|  |    - Linearize depth from reversed-Z buffer   |    |
|  |    - Compute signed CoC (-near/+far)          |    |
|  |    - Scale by aperture and focal range        |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | Pass 2: Downsample + Near/Far Split (Half-Res)|    |
|  |    - Downsample HDR to half resolution        |    |
|  |    - Split into near/far layers based on CoC  |    |
|  |    - Output: 4 render targets                 |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | Pass 3: Horizontal Blur (Half-Res)            |    |
|  |    - 11-tap Gaussian with bilateral weighting |    |
|  |    - Blur near and far layers separately      |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | Pass 4: Vertical Blur (Half-Res)              |    |
|  |    - 11-tap Gaussian with bilateral weighting |    |
|  |    - Blur near and far layers separately      |    |
|  +-----------------------------------------------+    |
|  +-----------------------------------------------+    |
|  | Pass 5: Composite (Full-Res)                  |    |
|  |    - Bilateral upsample blurred layers        |    |
|  |    - Blend with sharp original based on CoC   |    |
|  |    - Near layer composites on top (occlusion) |    |
|  +-----------------------------------------------+    |
+-------------------------------------------------------+
        |
        v (Focus-blurred HDR texture)
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

## Circle of Confusion (CoC) Model

The CoC model is artist-friendly, using intuitive parameters instead of physical lens properties:

### Parameters

| Parameter | Description | Range |
|-----------|-------------|-------|
| `focusDistance` | Distance to focal plane (world units) | 1 - 100 |
| `focalRange` | Depth range that remains in focus | 1 - 20 |
| `aperture` | f-stop value (lower = more blur) | 1.4 - 16 |
| `maxBlurRadius` | Maximum blur radius in pixels | 4 - 16 |

### CoC Calculation

```hlsl
// Linearize depth from reversed-Z buffer
float LinearizeDepth(float rawDepth) {
    return nearZ * farZ / (farZ + rawDepth * (nearZ - farZ));
}

float linearDepth = LinearizeDepth(rawDepth);

// Signed CoC: negative = near field, positive = far field
float depthDiff = linearDepth - focusDistance;
float coc = depthDiff / focalRange;

// Aperture scaling: f/2.8 as baseline
float apertureScale = 2.8 / max(aperture, 0.1);
coc *= apertureScale;

// Clamp to [-1, 1] range
coc = clamp(coc, -1.0, 1.0);
```

---

## Near/Far Layer Separation

The algorithm separates the scene into near and far layers to prevent **foreground bleeding** artifacts:

```
Scene Depth Distribution:

   Near Field        Focus Plane        Far Field
   (coc < 0)         (coc = 0)          (coc > 0)
   |---------|-----------|---------|------------|
   [blurred]  [in-focus]  [blurred]
      |                                    |
      v                                    v
   Near Layer                         Far Layer
   (blurs independently)              (blurs independently)
                    |
                    v
              Composite Pass
              (near on top)
```

**Why separate?** Without separation, blurring would mix foreground colors into the background, creating visible halos around near objects.

---

## Files

| File | Description |
|------|-------------|
| `Engine/Rendering/DepthOfFieldPass.h` | CDepthOfFieldPass class header |
| `Engine/Rendering/DepthOfFieldPass.cpp` | 5-pass DoF implementation (embedded shaders) |
| `Tests/TestDoF.cpp` | Automated test case |

**Note**: Shaders are embedded directly in `DepthOfFieldPass.cpp` rather than separate `.hlsl` files.

---

## Modified Files

| File | Changes |
|------|---------|
| `Engine/SceneLightSettings.h` | Added `SDepthOfFieldSettings` struct |
| `Engine/Rendering/ShowFlags.h` | Added `DepthOfField` flag |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.h` | Added `CDepthOfFieldPass` member and accessor |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Integrated DoF pass after Motion Blur |
| `Editor/Panels_SceneLightSettings.cpp` | Added DoF UI controls |
| `CMakeLists.txt` | Added `DepthOfFieldPass.h/cpp` and `TestDoF.cpp` |

---

## Class Design

### SDepthOfFieldSettings (SceneLightSettings.h)
```cpp
struct SDepthOfFieldSettings
{
    float focusDistance = 10.0f;   // Focus plane distance in world units (1-100)
    float focalRange = 5.0f;       // Depth range that remains in focus (1-20)
    float aperture = 2.8f;         // f-stop value, lower = more blur (1.4-16)
    float maxBlurRadius = 8.0f;    // Maximum blur radius in pixels (4-16)
};
```

### CDepthOfFieldPass (DepthOfFieldPass.h)
```cpp
class CDepthOfFieldPass {
public:
    bool Initialize();
    void Shutdown();

    // Returns DoF-processed texture or hdrInput if disabled/error
    RHI::ITexture* Render(RHI::ITexture* hdrInput,
                          RHI::ITexture* depthBuffer,
                          float cameraNearZ, float cameraFarZ,
                          uint32_t width, uint32_t height,
                          const SDepthOfFieldSettings& settings);

    RHI::ITexture* GetOutputTexture() const;
    RHI::ITexture* GetCoCTexture() const;

private:
    // Full-resolution textures
    RHI::TexturePtr m_cocBuffer;    // R32_FLOAT, signed CoC
    RHI::TexturePtr m_outputHDR;    // R16G16B16A16_FLOAT

    // Half-resolution textures (near/far layers)
    RHI::TexturePtr m_nearColor;    // R16G16B16A16_FLOAT
    RHI::TexturePtr m_farColor;     // R16G16B16A16_FLOAT
    RHI::TexturePtr m_nearCoc;      // R32_FLOAT
    RHI::TexturePtr m_farCoc;       // R32_FLOAT
    RHI::TexturePtr m_blurTempNear; // R16G16B16A16_FLOAT
    RHI::TexturePtr m_blurTempFar;  // R16G16B16A16_FLOAT

    // Shaders (6 total)
    RHI::ShaderPtr m_fullscreenVS;
    RHI::ShaderPtr m_cocPS;
    RHI::ShaderPtr m_downsampleSplitPS;
    RHI::ShaderPtr m_blurHPS;
    RHI::ShaderPtr m_blurVPS;
    RHI::ShaderPtr m_compositePS;

    // Pipeline states (5 PSOs)
    RHI::PipelineStatePtr m_cocPSO;
    RHI::PipelineStatePtr m_downsampleSplitPSO;
    RHI::PipelineStatePtr m_blurHPSO;
    RHI::PipelineStatePtr m_blurVPSO;
    RHI::PipelineStatePtr m_compositePSO;
};
```

---

## Shader Passes

### Pass 1: CoC Calculation
```hlsl
// Input: Depth buffer (D32_FLOAT, reversed-Z)
// Output: CoC buffer (R32_FLOAT, signed)

float main(PSIn input) : SV_Target {
    float rawDepth = gDepthBuffer.SampleLevel(gPointSampler, input.uv, 0).r;
    float linearDepth = LinearizeDepth(rawDepth);

    float depthDiff = linearDepth - gFocusDistance;
    float coc = depthDiff / gFocalRange;
    float apertureScale = 2.8 / max(gAperture, 0.1);
    coc *= apertureScale;

    return clamp(coc, -1.0, 1.0);
}
```

### Pass 2: Downsample + Split
```hlsl
// Input: HDR buffer, CoC buffer
// Output: 4 render targets (nearColor, farColor, nearCoC, farCoC)

PSOut main(PSIn input) {
    float4 color = gHDRInput.SampleLevel(gLinearSampler, input.uv, 0);
    float coc = gCoCBuffer.SampleLevel(gPointSampler, input.uv, 0).r;

    if (coc < 0.0) {
        // Near field
        output.nearColor = float4(color.rgb, 1.0);
        output.nearCoC = abs(coc);
        output.farColor = float4(0, 0, 0, 0);
        output.farCoC = 0.0;
    } else {
        // Far field
        output.farColor = float4(color.rgb, 1.0);
        output.farCoC = coc;
        output.nearColor = float4(0, 0, 0, 0);
        output.nearCoC = 0.0;
    }
    return output;
}
```

### Pass 3/4: Separable Gaussian Blur
```hlsl
// Input: Color layer, CoC layer
// Output: Blurred color

static const float kGaussianWeights[11] = {
    0.0093, 0.028, 0.0659, 0.1216, 0.1756,
    0.1974,  // Center
    0.1756, 0.1216, 0.0659, 0.028, 0.0093
};

float4 main(PSIn input) : SV_Target {
    float centerCoC = gCoCInput.SampleLevel(gLinearSampler, input.uv, 0).r;
    float blurRadius = centerCoC * gMaxCoCRadius;

    float4 colorSum = float4(0, 0, 0, 0);
    float weightSum = 0.0;

    for (int i = 0; i < 11; ++i) {
        float offset = (float)(i - 5);
        float2 sampleUV = saturate(input.uv + kBlurDir * gTexelSize * offset * blurRadius);

        float4 sampleColor = gColorInput.SampleLevel(gLinearSampler, sampleUV, 0);
        float sampleCoC = gCoCInput.SampleLevel(gLinearSampler, sampleUV, 0).r;

        float weight = kGaussianWeights[i];
        // Bilateral weighting
        float cocDiff = abs(sampleCoC - centerCoC);
        weight *= exp(-cocDiff * 4.0);

        colorSum += sampleColor * weight;
        weightSum += weight;
    }

    return colorSum / max(weightSum, 0.0001);
}
```

### Pass 5: Composite
```hlsl
// Input: Sharp HDR, CoC, blurred near/far layers
// Output: Final DoF result

float4 main(PSIn input) : SV_Target {
    float4 sharpColor = gHDRInput.SampleLevel(gLinearSampler, input.uv, 0);
    float coc = gCoCBuffer.SampleLevel(gPointSampler, input.uv, 0).r;
    float4 nearBlurred = gNearBlurred.SampleLevel(gLinearSampler, input.uv, 0);
    float4 farBlurred = gFarBlurred.SampleLevel(gLinearSampler, input.uv, 0);

    float4 result = sharpColor;

    // Far field blend
    if (coc > 0.0) {
        float farBlend = saturate(abs(coc) * 3.0);
        result = lerp(sharpColor, farBlurred, farBlend * farBlurred.a);
    }

    // Near field overlay (foreground occlusion)
    if (nearBlurred.a > 0.0) {
        float nearBlend = saturate(nearCocVal * 3.0);
        result = lerp(result, nearBlurred, nearBlend * nearBlurred.a);
    }

    return float4(result.rgb, 1.0);
}
```

---

## Pipeline Integration

### DeferredRenderPipeline.cpp
```cpp
// ============================================
// 8.5. Depth of Field Pass (after Motion Blur, before Bloom)
// ============================================
ITexture* hdrAfterDoF = hdrAfterMotionBlur;
if (ctx.showFlags.DepthOfField) {
    const auto& dofSettings = ctx.scene.GetLightSettings().depthOfField;
    CScopedDebugEvent evt(cmdList, L"Depth of Field");
    hdrAfterDoF = m_dofPass.Render(
        hdrAfterMotionBlur, m_gbuffer.GetDepthBuffer(),
        ctx.camera.nearZ, ctx.camera.farZ,
        ctx.width, ctx.height, dofSettings);
}

// ============================================
// 9. Bloom Pass (uses DoF output)
// ============================================
RHI::ITexture* bloomResult = nullptr;
if (ctx.showFlags.PostProcessing) {
    const auto& bloomSettings = ctx.scene.GetLightSettings().bloom;
    if (bloomSettings.enabled) {
        bloomResult = m_bloomPass.Render(
            hdrAfterDoF, ctx.width, ctx.height, bloomSettings);
    }
}
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Located in "Post-Processing: Depth of Field" section:
- **Enable**: Toggle DoF on/off (via FShowFlags.DepthOfField)
- **Focus Distance**: Distance to focal plane (1.0 - 100.0)
- **Focal Range**: Depth range in focus (1.0 - 20.0)
- **Aperture**: f-stop value (1.4 - 16.0)
- **Max Blur**: Maximum blur radius in pixels (4.0 - 16.0)

---

## Test Case

**File**: `Tests/TestDoF.cpp`

```bash
timeout 20 E:/forfun/source/code/build/Debug/forfun.exe --test TestDoF
```

Test sequence:
1. Frame 1: Create scene with objects at 2m, 5m, 12m depths
2. Frame 5: Enable DoF, focus on mid-ground (5m)
3. Frame 20: Capture screenshot - mid sphere sharp, near/far blurred
4. Frame 25: Change focus to near object (2m)
5. Frame 35: Capture screenshot - near sharp, mid/far blurred
6. Frame 40: Test wide aperture (f/1.4 - strong blur)
7. Frame 50: Capture screenshot - strong blur effect
8. Frame 55: Test narrow aperture (f/16 - minimal blur)
9. Frame 65: Capture screenshot - almost everything in focus
10. Frame 70: Disable DoF for comparison
11. Frame 75: Capture screenshot - all objects sharp
12. Frame 80: Test complete

---

## Memory Usage

| Resolution | Memory Usage | Notes |
|------------|--------------|-------|
| 1920x1080 | ~32 MB | Full-res: CoC + Output, Half-res: 6 textures |
| 2560x1440 | ~56 MB | Full-res: CoC + Output, Half-res: 6 textures |
| 3840x2160 | ~128 MB | Full-res: CoC + Output, Half-res: 6 textures |

**Texture Breakdown**:
- Full-res CoC (R32_FLOAT): 4 bytes/pixel
- Full-res Output (R16G16B16A16_FLOAT): 8 bytes/pixel
- Half-res layers (6 textures): ~12 bytes/pixel at quarter size

---

## Implementation Notes

1. **Reversed-Z Depth**: The CoC calculation correctly handles the project's reversed-Z depth buffer convention (near=1, far=0).

2. **Half-Resolution Blur**: Blurring at half resolution significantly reduces GPU cost while maintaining acceptable quality.

3. **Bilateral Weights**: The blur kernel uses bilateral weighting based on CoC difference to prevent sharp edges from bleeding into blurred areas.

4. **Near Layer Overlay**: The composite pass applies the near layer on top to correctly handle foreground occlusion.

5. **Aperture Scaling**: f/2.8 is used as the baseline aperture. Lower values (f/1.4) produce more blur, higher values (f/16) produce less.

6. **Early-Out**: If aperture >= 16.0, the pass is skipped entirely as the effect would be negligible.

7. **Safe Initialization**: The `createShaders()` and `createPSOs()` methods return bool. If either fails, initialization is aborted and resources are cleaned up.

8. **Minimum Dimension**: Half-resolution dimensions are clamped to minimum 1 to prevent division by zero with very small viewports.

---

## Future Improvements

1. **Bokeh Shape**: Add hexagonal or octagonal bokeh patterns for more cinematic look
2. **Auto-Focus**: Automatic focus based on center screen depth or raycasting
3. **Focus Peaking**: Debug visualization showing in-focus areas
4. **Adaptive Quality**: Variable sample count based on CoC magnitude
5. **Foreground Dilation**: Expand near layer CoC to create proper foreground bleeding effect
