# TAA (Temporal Anti-Aliasing)

## Overview

Temporal Anti-Aliasing (TAA) is a post-processing technique that reduces aliasing by accumulating samples across multiple frames. Unlike traditional anti-aliasing methods (MSAA, FXAA), TAA achieves supersampling-quality results at a fraction of the cost by leveraging temporal coherence.

## How TAA Works

### Core Concept

1. **Sub-pixel Jitter**: Each frame, the camera projection matrix is offset by a sub-pixel amount using a Halton(2,3) sequence
2. **Temporal Accumulation**: Current frame is blended with reprojected history from previous frames
3. **Motion Compensation**: Per-pixel motion vectors enable accurate history reprojection
4. **History Validation**: Various techniques prevent ghosting from invalid history samples

### Pipeline Position

```
Depth Pre-Pass → G-Buffer → Lighting → SSR → TAA → Auto Exposure → Motion Blur → Bloom → Post-Process
                                              ↑
                                         HDR Space
```

TAA runs in HDR space after SSR and before tone mapping to preserve high dynamic range detail.

## Algorithm Levels

The implementation provides 6 progressive algorithm levels, each building on the previous:

| Level | Name | Description | Quality | Performance |
|-------|------|-------------|---------|-------------|
| 0 | Off | Passthrough (no TAA) | - | - |
| 1 | Basic | Simple history blend | Poor (heavy ghosting) | Fastest |
| 2 | Neighborhood Clamp | Min/max AABB clamping | Acceptable | Fast |
| 3 | Variance Clip | Statistical clipping in YCoCg | Good | Medium |
| 4 | Catmull-Rom | + Sharper history sampling | Very Good | Medium |
| 5 | Motion Rejection | + Velocity/depth rejection | Excellent | Medium |
| 6 | Production | + Sharpening | Production | Slowest |

### Level 1: Basic (Simple Blend)

```hlsl
result = lerp(current, history, 0.9);
```

Simple exponential moving average. Demonstrates the concept but produces heavy ghosting on moving objects.

### Level 2: Neighborhood Clamping

Constrains history to the min/max color range of the current frame's 3x3 neighborhood:

```hlsl
float3 minC, maxC;
ComputeNeighborhoodMinMax(pixel, minC, maxC);
history = clamp(history, minC, maxC);
```

Reduces ghosting but can cause flickering on high-contrast edges.

### Level 3: Variance Clipping (YCoCg)

Uses statistical variance instead of min/max, and operates in YCoCg color space for better perceptual results:

```hlsl
// Compute mean and variance in YCoCg
float3 mean = m1 / 9.0;
float3 variance = sqrt(max(m2/9.0 - mean*mean, 0));

// Clip to variance-based AABB
float3 aabbMin = mean - gamma * variance;
float3 aabbMax = mean + gamma * variance;
history = ClipToAABB(history, aabbMin, aabbMax);
```

### Level 4: Catmull-Rom History Sampling

Replaces bilinear history sampling with a 5-tap Catmull-Rom filter for sharper results:

```hlsl
float4 SampleHistoryCatmullRom(float2 uv) {
    // Optimized 5-tap bicubic-like filter
    // Preserves sharpness while reducing temporal blur
}
```

### Level 5: Motion & Depth Rejection

Handles disocclusion and fast motion by reducing history weight:

```hlsl
// Velocity-based rejection
float velocityWeight = saturate(1.0 - velocityLength * velocityScale);

// Depth-based rejection (disocclusion detection)
float depthWeight = saturate(1.0 - depthGradient * depthScale);

// Combined weight
float finalBlend = historyBlend * velocityWeight * depthWeight;
```

### Level 6: Production (Sharpening)

Adds post-TAA sharpening to counteract temporal blur:

```hlsl
// Unsharp mask
float3 blur = (top + bottom + left + right) * 0.25;
float3 sharp = center + (center - blur) * sharpenStrength;
```

## Configuration

### Settings Structure

```cpp
struct STAASettings {
    ETAAAlgorithm algorithm = ETAAAlgorithm::Production;

    // Core parameters
    float history_blend = 0.95f;            // History weight (0.8-0.98)

    // Level 3+ (Variance Clipping)
    float variance_clip_gamma = 1.0f;       // Variance clip box scale (0.75-1.5)

    // Level 5+ (Motion Rejection)
    float velocity_rejection_scale = 0.1f;  // Scale for velocity-based rejection
    float depth_rejection_scale = 100.0f;   // Scale for depth-based rejection

    // Level 6 (Sharpening)
    bool sharpening_enabled = true;
    float sharpening_strength = 0.2f;       // 0.0-0.5 recommended

    // Jitter settings
    uint32_t jitter_samples = 8;            // 4, 8, or 16
};
```

### Recommended Settings

| Use Case | Algorithm | History Blend | Sharpening |
|----------|-----------|---------------|------------|
| Performance | VarianceClip (3) | 0.9 | Off |
| Balanced | MotionRejection (5) | 0.93 | Off |
| Quality | Production (6) | 0.95 | 0.2 |
| Ultra | Production (6) | 0.97 | 0.25 |

## Integration

### Enabling TAA

```cpp
// Via ShowFlags
auto& showFlags = CEditorContext::Instance().GetShowFlags();
showFlags.TAA = true;

// Configure settings
auto& settings = pipeline->GetTAAPass().GetSettings();
settings.algorithm = ETAAAlgorithm::Production;
settings.history_blend = 0.95f;
```

### Camera Jitter

TAA requires sub-pixel jitter on the projection matrix. This is handled automatically when TAA is enabled:

```cpp
// In DeferredRenderPipeline::Render()
camera.SetTAAEnabled(ctx.showFlags.TAA);
camera.SetJitterSampleCount(m_taaPass.GetSettings().jitter_samples);

// Use jittered projection in passes
XMMATRIX proj = camera.GetJitteredProjectionMatrix(width, height);
```

### Motion Vectors

TAA uses the velocity buffer from the G-Buffer (RT4: R16G16_FLOAT) for reprojection. Motion vectors are computed in the G-Buffer pass using current and previous frame view-projection matrices.

## Files

| File | Description |
|------|-------------|
| `Engine/Rendering/TAAPass.h` | TAA pass class and settings |
| `Engine/Rendering/TAAPass.cpp` | TAA implementation |
| `Shader/TAA.cs.hlsl` | TAA compute shader (all algorithms) |
| `Shader/TAASharpen.cs.hlsl` | Sharpening compute shader |
| `Engine/Camera.h/cpp` | Halton jitter implementation |
| `Tests/TestTAA.cpp` | TAA test case |

## Debugging

### Invalidate History

Force TAA to restart accumulation (useful after scene changes):

```cpp
pipeline->GetTAAPass().InvalidateHistory();
```

### Debug Visualization

The G-Buffer debug modes include velocity visualization:
- **Velocity**: Shows motion vectors as color (useful for debugging reprojection)

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Heavy ghosting | Algorithm too simple | Use Level 3+ |
| Flickering | Variance gamma too low | Increase `variance_clip_gamma` |
| Blurry image | Too much history blend | Reduce `history_blend` or enable sharpening |
| Trails on fast motion | Insufficient rejection | Increase `velocity_rejection_scale` |
| Disocclusion artifacts | Depth rejection too weak | Increase `depth_rejection_scale` |

## Performance

Approximate GPU cost at 1080p:

| Algorithm | Cost |
|-----------|------|
| Basic | ~0.2ms |
| Neighborhood Clamp | ~0.3ms |
| Variance Clip | ~0.4ms |
| Catmull-Rom | ~0.5ms |
| Motion Rejection | ~0.6ms |
| Production (with sharpen) | ~0.8ms |

## References

- "High Quality Temporal Supersampling" - Brian Karis (Epic Games, SIGGRAPH 2014)
- "Temporal Reprojection Anti-Aliasing in INSIDE" - Playdead (GDC 2016)
- "Filmic SMAA" - Jorge Jimenez (SIGGRAPH 2016)
- "A Survey of Temporal Antialiasing Techniques" - Lei Yang et al. (2020)
