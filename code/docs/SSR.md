# Screen-Space Reflections (SSR)

This document describes the Screen-Space Reflections system implementation.

**Related Files:**
- `Engine/Rendering/SSRPass.h` - SSR pass header
- `Engine/Rendering/SSRPass.cpp` - SSR pass implementation
- `Shader/SSR.cs.hlsl` - SSR compute shader
- `Shader/SSRComposite.cs.hlsl` - SSR composite shader
- `Engine/Rendering/HiZPass.h` - Hi-Z pyramid (required dependency)

---

## Overview

The SSR system implements screen-space reflections with multiple algorithm modes, ordered from simplest to most complex. It provides four modes with varying performance/quality trade-offs.

**Key Features:**
- Multiple ray marching algorithms (simple linear to Hi-Z accelerated)
- GGX importance sampling for physically-based rough reflections
- Temporal accumulation for noise reduction
- Fresnel-weighted blending with IBL fallback
- Quality presets (Low/Medium/High/Ultra)
- Full reversed-Z depth buffer support

**Reference:** "Efficient GPU Screen-Space Ray Tracing" - Morgan McGuire & Michael Mara (2014)

---

## Architecture

### Pipeline Integration

```
Deferred Pipeline Flow:
┌─────────────────┐
│  G-Buffer Pass  │
└────────┬────────┘
         ↓
┌─────────────────┐
│   Hi-Z Build    │  ← Builds depth pyramid from G-Buffer depth
└────────┬────────┘
         ↓
┌─────────────────┐
│  Deferred       │
│  Lighting       │  ← Renders scene with IBL reflections
└────────┬────────┘
         ↓
┌─────────────────┐
│   SSR Trace     │  ← Traces rays against Hi-Z pyramid
└────────┬────────┘
         ↓
┌─────────────────┐
│  SSR Composite  │  ← Blends SSR with HDR buffer
└─────────────────┘
```

### Class Structure

```cpp
class CSSRPass {
public:
    // Lifecycle
    bool Initialize();
    void Shutdown();

    // Rendering
    void Render(...);      // Trace SSR rays
    void Composite(...);   // Blend SSR into HDR

    // Output
    ITexture* GetSSRTexture() const;

    // Settings
    SSSRSettings& GetSettings();
};
```

---

## SSR Modes

Modes are ordered from simplest to most complex (0 → 3).

### Mode 0: SimpleLinear

Basic linear ray march without Hi-Z acceleration. Educational/debugging mode.

**Characteristics:**
- Fixed stride stepping against full-res depth buffer
- No Hi-Z pyramid required
- Simplest algorithm, easiest to understand
- Slower than Hi-Z modes (samples every step)

**Use Case:** Learning, debugging, fallback when Hi-Z unavailable

### Mode 1: HiZ Trace (Default)

Single ray per pixel using Hi-Z accelerated ray marching.

**Characteristics:**
- Fast performance via hierarchical depth sampling
- Best for smooth/mirror-like surfaces
- May show aliasing on rough surfaces
- No temporal noise

**Use Case:** Real-time games, smooth reflective surfaces

### Mode 2: Stochastic

Multiple rays per pixel with GGX importance sampling.

**Characteristics:**
- Better quality for rough surfaces
- Physically-based reflection distribution
- Temporal noise (without accumulation)
- 2-8x cost vs HiZ Trace

**Parameters:**
- `numRays` (1-8): Rays traced per pixel
- `brdfBias` (0-1): Blend between uniform and GGX sampling

**Use Case:** Cinematic rendering, rough metal/water

### Mode 3: Temporal

Stochastic sampling + temporal history accumulation.

**Characteristics:**
- Highest quality
- Smooth reflections on rough surfaces
- Requires motion vectors for reprojection
- May show ghosting on fast motion

**Parameters:**
- `temporalBlend` (0-0.98): History blend factor
- `motionThreshold` (0.001-0.1): Motion rejection threshold

**Use Case:** High-quality rendering, static/slow scenes

---

## Algorithm Details

### Simple Linear Ray Marching

The SimpleLinear algorithm performs basic fixed-stride ray marching:

```
1. Transform ray to screen-space (start UV, end UV, depths)
2. Calculate step size = normalize(rayDir) * texelSize * stride
3. Early-out if reflectDir.z < 0 (ray points behind camera in LH coords)
4. For each step (up to maxSteps):
   a. Advance UV and depth by step size
   b. Sample full-res depth buffer
   c. If ray depth <= scene depth (reversed-Z):
      - If within thickness threshold → HIT
      - Else continue (passed through thin geometry)
5. Apply edge fade and distance fade to confidence
```

**Left-Handed Coordinate System:**
- +Z points into the scene (forward)
- -Z points behind the camera
- Skip rays with `reflectDir.z < 0` (cannot hit screen pixels)

**Advantages:** Simple, no Hi-Z dependency, easy to debug
**Disadvantages:** Slower (no acceleration), may miss thin features

### Hi-Z Ray Marching

The Hi-Z algorithm uses cell-based traversal for efficient ray-scene intersection:

```
1. Transform ray to screen-space (ssPos = UV + depth)
2. Normalize ray direction per-pixel: ssDir = ssRay / rayLengthPixels
3. Start at mip level 0, track consecutive "in front" steps
4. For each step:
   a. Sample Hi-Z at current mip level
   b. If ray is behind depth (reversed-Z: rayDepth <= sceneDepth):
      - If at mip 0 and within thickness → HIT
      - If at mip 0 but too thick → continue (passed through)
      - If at coarse mip → decrease mip level (refine)
   c. If ray is in front:
      - Calculate cell boundaries at current mip
      - Step to nearest cell boundary (exact distance)
      - After 3 safe steps, increase mip level (accelerate)
5. Apply edge fade, distance fade, and back-face fade to confidence
```

**Cell-Based Stepping:**
- At each mip level, cells are `2^mip` pixels wide
- Ray advances exactly to cell boundary (no fixed stride patterns)
- Eliminates stripe artifacts from adaptive stride scaling

**Conservative Mip Increase:**
- Only increase mip after 3 consecutive "in front" steps
- Prevents oscillation between mip levels during refinement

**Reversed-Z Handling:**
- Near plane = 1.0, Far plane = 0.0
- Ray is "behind" surface when `rayDepth <= sceneDepth`

**Back-Face Rejection:**
- Confidence is reduced when hitting back-facing surfaces
- Uses `dot(-rayDir, hitNormal)` to detect back faces

### GGX Importance Sampling

For stochastic modes, reflection directions are sampled from the GGX distribution:

```hlsl
float3 SampleGGX(float2 Xi, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a2 - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}
```

The half-vector H is transformed from tangent space to view space, then the reflection direction is computed as `reflect(viewDir, H)`.

### Temporal Accumulation

Temporal mode reduces noise by accumulating samples over multiple frames:

```
1. Compute current frame SSR (stochastic, 1-2 rays)
2. Reproject pixel to previous frame using prevViewProj
3. Sample history buffer at reprojected UV
4. Compute motion magnitude for rejection
5. Clamp history to neighborhood bounds (reduce ghosting)
6. Blend: result = lerp(current, history, blendFactor * motionRejection)
```

### Composite Pass

The composite pass blends SSR results with the existing HDR buffer:

```hlsl
// Fresnel-Schlick approximation
float3 F0 = lerp(0.04, albedo, metallic);
float3 fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

// Roughness attenuation
float roughnessWeight = 1.0 - saturate(roughness / roughnessFade);

// Final blend
float blendWeight = confidence * roughnessWeight * ssrIntensity;
float3 ssrContribution = ssrColor * fresnel * blendWeight;
finalColor = hdrColor + ssrContribution * (1.0 - metallic * 0.5 * blendWeight);
```

---

## Settings Reference

### SSSRSettings Structure

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enabled` | bool | true | Enable/disable SSR |
| `quality` | ESSRQuality | High | Quality preset |
| `mode` | ESSRMode | HiZTrace | Algorithm mode |
| `maxDistance` | float | 50.0 | Maximum ray distance (view-space units) |
| `thickness` | float | 0.5 | Surface thickness for hit detection |
| `stride` | float | 1.0 | Initial ray step size (pixels) |
| `strideZCutoff` | float | 100.0 | View-Z at which stride scales |
| `maxSteps` | int | 64 | Maximum ray march iterations |
| `binarySearchSteps` | int | 8 | Binary search refinement steps |
| `jitterOffset` | float | 0.0 | Temporal jitter (0-1) |
| `fadeStart` | float | 0.8 | Edge fade start (screen UV) |
| `fadeEnd` | float | 1.0 | Edge fade end (screen UV) |
| `roughnessFade` | float | 0.5 | Roughness cutoff for SSR |
| `intensity` | float | 1.0 | SSR brightness multiplier |
| `numRays` | int | 4 | Rays per pixel (stochastic/temporal) |
| `brdfBias` | float | 0.5 | BRDF sampling bias (0=uniform, 1=GGX) |
| `temporalBlend` | float | 0.9 | History blend factor |
| `motionThreshold` | float | 0.01 | Motion rejection threshold |

### Quality Presets

| Preset | maxSteps | binarySearchSteps | stride | numRays |
|--------|----------|-------------------|--------|---------|
| Low | 32 | 4 | 2.0 | 1 |
| Medium | 48 | 6 | 1.5 | 2 |
| High | 64 | 8 | 1.0 | 4 |
| Ultra | 96 | 12 | 0.5 | 8 |
| Custom | (user-defined) | | | |

---

## Resource Requirements

### Textures

| Texture | Format | Size | Usage |
|---------|--------|------|-------|
| SSR Result | R16G16B16A16_FLOAT | Full-res | SRV + UAV |
| SSR History | R16G16B16A16_FLOAT | Full-res | SRV + UAV |
| Blue Noise | R8G8B8A8_UNORM | 64x64 | SRV |
| Black Fallback | R16G16B16A16_FLOAT | 1x1 | SRV |

### Dependencies

- **Hi-Z Pass:** Required for accelerated ray marching
- **G-Buffer:** Depth, Normal+Roughness
- **HDR Buffer:** Scene color (with UAV support for composite)

---

## Performance Considerations

### GPU Cost Factors

1. **Screen Resolution:** Linear scaling with pixel count
2. **Max Steps:** Direct impact on trace cost
3. **Num Rays:** Linear scaling for stochastic modes
4. **Hi-Z Mip Levels:** More mips = faster convergence

### Optimization Tips

1. **Use Quality Presets:** Start with Medium, adjust as needed
2. **Roughness Fade:** Set to 0.3-0.5 to skip rough surfaces early
3. **Max Distance:** Reduce for indoor scenes
4. **Temporal Mode:** Use fewer rays (1-2) since accumulation smooths result

### Performance Estimates (RTX 2060, 1080p)

| Mode | Quality | Approx. Cost |
|------|---------|--------------|
| SimpleLinear | High | ~1.0ms |
| HiZ Trace | High | ~0.5ms |
| Stochastic | High (4 rays) | ~1.5ms |
| Temporal | High (2 rays) | ~1.0ms |

---

## Known Limitations

1. **Screen-Space Only:** Cannot reflect off-screen objects
2. **Single Bounce:** No multi-bounce reflections
3. **Thickness Heuristic:** May miss thin objects
4. **Temporal Ghosting:** Fast motion may show artifacts in temporal mode
5. **No Transparency:** Cannot trace through transparent surfaces

### Edge Cases

- **Sky Pixels:** Early-out when depth is near far plane (reversed-Z: depth < 0.0001)
- **Grazing Angles:** Confidence reduced for rays nearly parallel to surface
- **Screen Edges:** Confidence fades smoothly at screen boundaries

---

## Debug Visualization

The G-Buffer debug modes include SSR visualization:

- **SSR Result** (Mode 17): Shows raw SSR color output
- **SSR Confidence** (Mode 18): Shows hit confidence (white = hit, black = miss)

Access via: Scene Light Settings → G-Buffer Debug Visualization

---

## Code Examples

### Basic Usage

```cpp
// In render pipeline initialization
m_ssrPass.Initialize();

// In render loop
if (m_ssrPass.GetSettings().enabled && m_hiZPass.GetSettings().enabled) {
    m_ssrPass.Render(cmdList,
                     gbuffer.GetDepthBuffer(),
                     gbuffer.GetNormalRoughness(),
                     m_hiZPass.GetHiZTexture(),
                     hdrBuffer,
                     width, height,
                     m_hiZPass.GetMipCount(),
                     viewMatrix, projMatrix,
                     nearZ, farZ);

    m_ssrPass.Composite(cmdList,
                        hdrBuffer,
                        gbuffer.GetWorldPosMetallic(),
                        gbuffer.GetNormalRoughness(),
                        width, height,
                        cameraPosition);
}
```

### Changing Settings at Runtime

```cpp
auto& settings = m_ssrPass.GetSettings();

// Switch to temporal mode for high quality
settings.mode = ESSRMode::Temporal;
settings.numRays = 2;
settings.temporalBlend = 0.9f;

// Or use a preset
settings.ApplyPreset(ESSRQuality::Ultra);
```

---

## Future Improvements

Potential enhancements for consideration:

1. **Cone Tracing:** Sample multiple Hi-Z mips based on roughness
2. **Hierarchical Stochastic:** Trace at lower resolution, upsample
3. **Bent Normal Occlusion:** Use SSAO bent normals for better sampling
4. **Variable Rate Shading:** Lower rate for rough surfaces
5. **Async Compute:** Overlap SSR with other work

---

**Last Updated:** 2026-01-14
