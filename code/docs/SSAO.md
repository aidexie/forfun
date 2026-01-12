# Screen-Space Ambient Occlusion (SSAO)

**Status**: ✅ **COMPLETED** (2025-01)

## Overview

Screen-Space Ambient Occlusion (SSAO) computes contact shadows in corners, crevices, and where objects meet surfaces. Implements three algorithms with runtime selection via `SceneLightSettings`.

**Dependencies**: Phase 3.2 G-Buffer (Depth + Normal buffers)

---

## Algorithm Comparison

| Algorithm | Method | Quality | Performance | Used By |
|-----------|--------|---------|-------------|---------|
| **GTAO** | Horizon integral with exact cosine-weighted formula | Best | Medium | UE5, Unity HDRP |
| **HBAO** | Horizon angle with sin() approximation | Good | Fast | NVIDIA games |
| **Crytek** | Random hemisphere sampling | Classic | Fastest | Crysis (2007) |

**Default**: GTAO (most physically accurate)

---

## Architecture

```
G-Buffer Depth + Normal (Full-Res)
        │
        ▼
┌─────────────────────────────────────────────────────┐
│  SSAOPass                                           │
│  ┌─────────────────────────────────────────────┐   │
│  │ 1. Depth Downsample (CSDownsampleDepth)     │   │
│  │    Full-Res → Half-Res (closest depth)      │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 2. SSAO Compute (CSMain)                    │   │
│  │    Half-Res AO (GTAO/HBAO/Crytek)          │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 3. Bilateral Blur H (CSBlurH)               │   │
│  │    Edge-preserving horizontal blur          │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 4. Bilateral Blur V (CSBlurV)               │   │
│  │    Edge-preserving vertical blur            │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │ 5. Bilateral Upsample (CSBilateralUpsample) │   │
│  │    Half-Res → Full-Res (depth-aware)        │   │
│  └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
        │
        ▼ (SSAO texture, R8_UNORM, Full-Res)
┌─────────────────────────────────────────────────────┐
│  DeferredLightingPass                               │
│  - Sample SSAO at t18                               │
│  - Multiply with material AO: finalAO = ao * ssao  │
│  - Apply to ambient lighting term                   │
└─────────────────────────────────────────────────────┘
```

---

## Files

| File | Description |
|------|-------------|
| `Engine/Rendering/SSAOPass.h` | SSAOPass class header, settings structs |
| `Engine/Rendering/SSAOPass.cpp` | SSAOPass implementation |
| `Shader/SSAO.cs.hlsl` | All compute shaders (5 entry points) |
| `Tests/TestSSAO.cpp` | Automated test case |

---

## Modified Files

| File | Changes |
|------|---------|
| `Engine/SceneLightSettings.h` | Added `SSSAOSettings` struct |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.h` | Added `CSSAOPass` member |
| `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` | Integrated SSAO pass |
| `Engine/Rendering/Deferred/DeferredLightingPass.cpp` | Bind SSAO texture at t18 |
| `Shader/DeferredLighting.ps.hlsl` | Sample and apply SSAO |
| `Editor/Panels_SceneLightSettings.cpp` | Added SSAO UI controls |
| `CMakeLists.txt` | Added `SSAOPass.cpp` |

---

## Class Design

### SSSAOSettings (SceneLightSettings.h)
```cpp
enum class ESSAOAlgorithm : int {
    GTAO = 0,    // Ground Truth AO (most accurate)
    HBAO = 1,    // Horizon-Based AO (NVIDIA)
    Crytek = 2,  // Original SSAO (Crysis 2007)
};

struct SSSAOSettings {
    ESSAOAlgorithm algorithm = ESSAOAlgorithm::GTAO;
    float radius = 0.5f;            // View-space AO radius
    float intensity = 1.5f;         // AO strength multiplier
    float falloffStart = 0.2f;      // Distance falloff start (0-1)
    float falloffEnd = 1.0f;        // Distance falloff end
    float depthSigma = 0.1f;        // Bilateral blur depth threshold
    int numSlices = 10;             // Direction slices (2-16)
    int numSteps = 20;              // Steps per direction
    int blurRadius = 2;             // Bilateral blur radius (1-4)
    bool enabled = true;
};
```

### CSSAOPass (SSAOPass.h)
```cpp
class CSSAOPass {
public:
    bool Initialize();
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    void Render(ICommandList* cmdList,
                ITexture* depthBuffer,     // G-Buffer depth
                ITexture* normalBuffer,    // G-Buffer RT1
                uint32_t width, uint32_t height,
                const XMMATRIX& view,
                const XMMATRIX& proj,
                float nearZ, float farZ);

    // Returns SSAO texture (white fallback if disabled)
    ITexture* GetSSAOTexture() const;

    SSSAOSettings& GetSettings();

private:
    // Compute shaders (5 entry points in SSAO.cs.hlsl)
    ShaderPtr m_ssaoCS;        // CSMain
    ShaderPtr m_blurHCS;       // CSBlurH
    ShaderPtr m_blurVCS;       // CSBlurV
    ShaderPtr m_upsampleCS;    // CSBilateralUpsample
    ShaderPtr m_downsampleCS;  // CSDownsampleDepth

    // Half-resolution textures
    TexturePtr m_ssaoRaw;          // Raw AO (noisy)
    TexturePtr m_ssaoBlurTemp;     // After horizontal blur
    TexturePtr m_ssaoHalfBlurred;  // After vertical blur
    TexturePtr m_depthHalfRes;     // Downsampled depth

    // Full-resolution output
    TexturePtr m_ssaoFinal;        // Final upsampled AO

    // Resources
    TexturePtr m_noiseTexture;     // 4x4 random rotations
    TexturePtr m_whiteFallback;    // 1x1 white (disabled fallback)
    SamplerPtr m_pointSampler;
    SamplerPtr m_linearSampler;
};
```

---

## Algorithm Details

### GTAO (Ground Truth Ambient Occlusion)

**Reference**: "Practical Real-Time Strategies for Accurate Indirect Occlusion" - Jimenez et al. (2016)

Computes the **exact visibility integral** over the cosine-weighted hemisphere:

```
For each pixel:
  1. Reconstruct view-space position from depth
  2. Get view-space normal from G-Buffer
  3. For each slice direction (2-16 slices):
     a. Project normal onto slice plane → get angle (n)
     b. March in BOTH directions along slice
     c. Find horizon angles h₁ (negative) and h₂ (positive)
     d. Clamp horizons to hemisphere
     e. Compute slice AO using GTAO integral
  4. Average all slices
  5. Output: AO ∈ [0,1] where 1 = fully lit
```

**GTAO Integral Formula** (Paper Equation 3):
```
â(h, n) = ¼ × [-cos(2h - n) + cos(n) + 2h·sin(n)]
```

### HBAO (Horizon-Based Ambient Occlusion)

**Reference**: "Image-Space Horizon-Based Ambient Occlusion" - Bavoil & Sainz (NVIDIA, 2008)

Simpler horizon-based approach using sin() approximation:

```
For each direction (2-16 directions):
  1. March along ray, tracking highest horizon angle
  2. Horizon angle = angle between sample and surface normal
  3. AO contribution = (1 - sin(horizon)) × falloff
```

### Crytek SSAO (Classic)

**Reference**: "Finding Next Gen - CryEngine 2" - Mittring (GDC 2007)

Original hemisphere sampling approach:

```
For each sample in kernel (up to 16):
  1. Apply random rotation from noise texture
  2. Sample depth at offset position
  3. Check if sample is in front hemisphere (NdotS > 0)
  4. Accumulate occlusion weighted by distance and NdotS
```

---

## Shader Implementation

### Entry Points (SSAO.cs.hlsl)

| Entry Point | Purpose | Resolution |
|-------------|---------|------------|
| `CSMain` | Main SSAO compute (algorithm selection) | Half-res |
| `CSBlurH` | Horizontal bilateral blur | Half-res |
| `CSBlurV` | Vertical bilateral blur | Half-res |
| `CSBilateralUpsample` | Edge-preserving upsample | Full-res |
| `CSDownsampleDepth` | Depth downsample for upsample | Half-res |

### Constant Buffers

```hlsl
// Main SSAO (b0)
cbuffer CB_SSAO {
    float4x4 gProj, gInvProj, gView;
    float2 gTexelSize, gNoiseScale;
    float gRadius, gIntensity;
    float gFalloffStart, gFalloffEnd;
    int gNumSlices, gNumSteps;
    float gThicknessHeuristic;
    int gAlgorithm;           // 0=GTAO, 1=HBAO, 2=Crytek
    uint gUseReversedZ;       // Reversed-Z support
};
```

### Debug Visualization Modes

Set `algorithm >= 100` to enable debug output:

| Mode | Value | Output |
|------|-------|--------|
| Raw Depth | 100 | Depth buffer [0,1] |
| Linear Depth | 101 | View-space Z normalized |
| View Pos Z | 102 | Position.z sign check |
| View Normal Z | 103 | Normal.z (facing camera = white) |
| Sample Diff | 104 | Reconstruction accuracy test |

---

## Pipeline Integration

### DeferredRenderPipeline.cpp

Insert between Clustered Lighting and Deferred Lighting:

```cpp
// 5. Clustered Lighting Compute
{ ... }

// 5.5 SSAO Pass
if (m_ssaoPass.GetSettings().enabled) {
    CScopedDebugEvent evt(cmdList, L"SSAO Pass");
    m_ssaoPass.Render(cmdList,
                      m_gbuffer.GetDepthBuffer(),
                      m_gbuffer.GetNormalRoughness(),
                      ctx.width, ctx.height,
                      viewMatrix, projMatrix,
                      nearZ, farZ);
}

// 6. Deferred Lighting Pass
m_lightingPass.Render(...);  // SSAO bound internally
```

### DeferredLighting.ps.hlsl

```hlsl
Texture2D gSSAO : register(t18);

// In main():
float materialAO = gbuffer.ao;              // From material texture
float ssao = gSSAO.Sample(gPointSampler, uv).r;
float finalAO = materialAO * ssao;          // Multiplicative

// Apply to ambient term:
float3 ambient = (diffuseGI + specularIBL) * finalAO;
```

---

## UI Controls (Panels_SceneLightSettings.cpp)

Located in "SSAO" collapsing header:

- **Enable**: Toggle SSAO on/off
- **Algorithm**: Dropdown (GTAO / HBAO / Crytek)
- **Radius**: AO radius in world units (0.1 - 2.0)
- **Intensity**: AO strength multiplier (0.0 - 13.0)
- **Falloff Start**: Distance falloff start (0.0 - 1.0)
- **Slices**: Number of direction slices (2 - 16)
- **Steps**: Steps per direction (4 - 32)
- **Blur Radius**: Bilateral blur radius (1 - 4)

---

## Texture Formats & Memory

| Texture | Format | Resolution | Memory @ 1080p |
|---------|--------|------------|----------------|
| m_ssaoRaw | R8_UNORM | Half (960×540) | 0.5 MB |
| m_ssaoBlurTemp | R8_UNORM | Half (960×540) | 0.5 MB |
| m_ssaoHalfBlurred | R8_UNORM | Half (960×540) | 0.5 MB |
| m_depthHalfRes | R32_FLOAT | Half (960×540) | 2.0 MB |
| m_ssaoFinal | R8_UNORM | Full (1920×1080) | 2.0 MB |
| m_noiseTexture | R8G8B8A8_UNORM | 4×4 | 64 bytes |

**Total**: ~5.5 MB additional VRAM

---

## Test Case

**File**: `Tests/TestSSAO.cpp`

```bash
timeout 15 E:/forfun/source/code/build/Debug/forfun.exe --test TestSSAO
```

Test verifies:
1. SSAO produces visible darkening (average AO < 0.95)
2. SSAO is not completely black (average AO > 0.3)
3. Screenshots with SSAO enabled/disabled for comparison

---

## Performance

| Pass | GPU Time @ 1080p | Notes |
|------|------------------|-------|
| Depth Downsample | ~0.05 ms | Closest depth selection |
| SSAO Compute | ~0.3 ms | Half-res, 10 slices × 20 steps |
| Blur H + V | ~0.1 ms | Half-res bilateral |
| Upsample | ~0.05 ms | Bilateral to full-res |
| **Total** | **~0.5 ms** | All SSAO passes |

Performance scales with `numSlices × numSteps`. For faster results:
- Reduce slices to 4-6
- Reduce steps to 8-12
- Use HBAO or Crytek algorithm

---

## Reversed-Z Support

SSAO fully supports reversed-Z depth buffers:

- **Standard-Z**: Near=0, Far=1, sky at 1.0, closest=min
- **Reversed-Z**: Near=1, Far=0, sky at 0.0, closest=max

The `gUseReversedZ` flag controls:
- Sky detection (`IsSkyDepth()`)
- Closest depth selection in downsample
- Bilateral blur edge detection

---

## Implementation Notes

1. **Half-Resolution**: SSAO computed at 1/2 resolution for performance, then bilateral upsampled to preserve edges

2. **Noise Texture**: 4×4 R8G8 texture storing cos/sin of random angles, tiled across screen to randomize sampling patterns

3. **Bilateral Filtering**: Both blur and upsample use depth-aware weighting to preserve edges at depth discontinuities

4. **White Fallback**: When SSAO is disabled, a 1×1 white texture is returned to avoid null checks in lighting shader

5. **View-Space Transform**: World normals from G-Buffer are transformed to view-space for proper horizon angle calculation

6. **DX11/DX12 Compatibility**: Uses RHI abstraction for all resource operations

---

## References

- **GTAO**: "Practical Real-Time Strategies for Accurate Indirect Occlusion" - Jimenez, Wu, Pesce, Jarabo (2016)
- **HBAO**: "Image-Space Horizon-Based Ambient Occlusion" - Bavoil, Sainz (NVIDIA, 2008)
- **Crytek SSAO**: "Finding Next Gen - CryEngine 2" - Mittring (GDC 2007)

## reflection
1. cc给出的算法有较多错误
2. GTAO本身有一定的复杂度，我低估了它的难度
3. 我对屏幕空间相关的问题不够熟悉。主要是屏幕空间的坐标系的问题，例如uv和xy坐标系不一致(y和v轴方向相反的)，z+的轴是向屏幕里的。
4. 从数学推导到真正实现上缺失了步骤。

## improve
1. 从最早算法版本开始实现，一直到最新的版本。有cc在，这种实现方式是不会增加实现时间，对理解这个算法帮助很大。
2. 基于数学的理解从基础到实现要走完整。