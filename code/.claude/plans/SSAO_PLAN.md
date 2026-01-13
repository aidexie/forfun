# Phase 3.3.2 SSAO Implementation Plan

## Overview

Implement **Screen-Space Ambient Occlusion (SSAO)** using the **GTAO (Ground Truth Ambient Occlusion)** algorithm as a compute shader pass in the deferred rendering pipeline.

**Input**: G-Buffer depth + normals
**Output**: Single-channel AO texture (R8_UNORM)
**Resolution**: Half-resolution (upsampled to full-res)
**Temporal**: Deferred to TAA phase (3.3.4)
**Settings Storage**: SceneLightSettings (per-scene serialization)
**Integration Point**: After Clustered Lighting Compute, before Deferred Lighting Pass

---

## Algorithm: GTAO (Ground Truth Ambient Occlusion)

### Why GTAO?

| Aspect | SSAO | HBAO | GTAO |
|--------|------|------|------|
| Method | Random hemisphere sampling | Horizon ray marching | Horizon + proper integral |
| Physical Basis | Approximate | Better (sin approx) | **Exact** (cosine-weighted) |
| Quality | Noisy | Good | **Best** |
| Used By | Crysis (2007) | NVIDIA games | **UE5, Unity HDRP** |

### GTAO Core Algorithm

GTAO computes the **exact visibility integral** over the cosine-weighted hemisphere using horizon angles:

```
For each pixel P:
  1. Reconstruct view-space position from depth
  2. Get view-space normal N from G-Buffer
  3. For each slice direction (2-4 slices through hemisphere):
     a. Project normal onto the slice plane → get projected normal angle (n)
     b. March in BOTH directions (+/-) along the slice
     c. Find horizon angles h₁ (negative dir) and h₂ (positive dir)
     d. Clamp horizons to hemisphere: h₁,h₂ ∈ [-π/2, π/2]
     e. Compute slice AO using GTAO integral formula
  4. Average all slices
  5. Output: AO value [0,1] where 1 = fully lit, 0 = fully occluded
```

### GTAO Integral Formula

For a single slice with horizon angles h₁, h₂ and projected normal angle n:

```
SliceAO = 0.25 * (-cos(2*h₁) + cos(2*n) + 2*h₁*sin(n))    // negative horizon
        + 0.25 * (-cos(2*h₂) + cos(2*n) + 2*h₂*sin(n))    // positive horizon

where:
  h₁, h₂ = horizon angles (clamped to [-π/2, π/2])
  n = angle of normal projected onto slice plane
```

This formula computes the **exact cosine-weighted solid angle** blocked by occluders.

---

## Architecture

### New Files

```
Engine/Rendering/
├── SSAOPass.h              # Pass class declaration
└── SSAOPass.cpp            # Pass implementation

Shader/
└── SSAO.cs.hlsl            # SSAO compute shader (main + blur + upsample)
```

### Class Design

```cpp
// Engine/Rendering/SSAOPass.h
class CSSAOPass {
public:
    // Lifecycle
    bool Initialize();
    void Shutdown();

    // Rendering
    void Render(ICommandList* cmdList,
                ITexture* depthBuffer,      // G-Buffer depth
                ITexture* normalBuffer,     // G-Buffer RT1 (normal.xyz)
                uint32_t width, uint32_t height,
                const XMMATRIX& proj,       // For depth linearization
                const XMMATRIX& invProj);   // For position reconstruction

    // Output
    ITexture* GetSSAOTexture() const { return m_ssaoFinal.get(); }

    // Settings (exposed to Editor)
    struct Settings {
        float radius = 0.5f;        // World-space AO radius
        float intensity = 1.0f;     // AO strength multiplier
        float bias = 0.025f;        // Depth bias to avoid self-occlusion
        int numDirections = 4;      // 4 or 8 directions
        int numSteps = 4;           // Steps per direction
        bool enabled = true;
    };
    Settings& GetSettings() { return m_settings; }

private:
    // Compute shaders
    RHI::ShaderPtr m_ssaoCS;        // Main SSAO compute (half-res)
    RHI::ShaderPtr m_blurHCS;       // Horizontal blur (half-res)
    RHI::ShaderPtr m_blurVCS;       // Vertical blur (half-res)
    RHI::ShaderPtr m_upsampleCS;    // Bilateral upsample to full-res

    // Pipeline states
    RHI::PipelineStatePtr m_ssaoPSO;
    RHI::PipelineStatePtr m_blurHPSO;
    RHI::PipelineStatePtr m_blurVPSO;
    RHI::PipelineStatePtr m_upsamplePSO;

    // Half-resolution textures
    RHI::TexturePtr m_ssaoRaw;         // Raw SSAO (half-res, noisy)
    RHI::TexturePtr m_ssaoBlurTemp;    // Temp for separable blur (half-res)
    RHI::TexturePtr m_ssaoHalfBlurred; // Blurred SSAO (half-res)

    // Full-resolution output
    RHI::TexturePtr m_ssaoFinal;       // Final upsampled SSAO (full-res)

    // Half-res depth for upsample
    RHI::TexturePtr m_depthHalfRes;    // Downsampled depth for bilateral upsample

    // Noise texture (4x4 random directions)
    RHI::TexturePtr m_noiseTexture;
    RHI::SamplerPtr m_pointSampler;
    RHI::SamplerPtr m_linearSampler;

    Settings m_settings;
    bool m_initialized = false;
};
```

---

## Pipeline Integration

### DeferredRenderPipeline Changes

**File**: `Engine/Rendering/Deferred/DeferredRenderPipeline.h`

```cpp
class CDeferredRenderPipeline : public CRenderPipeline {
    // ... existing members ...

    // Add SSAO pass
    CSSAOPass m_ssaoPass;

public:
    // Expose settings to editor
    CSSAOPass::Settings& GetSSAOSettings() { return m_ssaoPass.GetSettings(); }
};
```

**File**: `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp`

Insert SSAO between Clustered Lighting and Deferred Lighting (around line 305):

```cpp
// 5. Clustered Lighting Compute
{
    CScopedDebugEvent evt(cmdList, L"Clustered Lighting Compute");
    m_clusteredLighting.BuildClusterGrid(...);
    m_clusteredLighting.CullLights(...);
}

// *** NEW: 5.5. SSAO Pass ***
if (m_ssaoPass.GetSettings().enabled) {
    CScopedDebugEvent evt(cmdList, L"SSAO Pass");
    m_ssaoPass.Render(cmdList,
                      m_gbuffer.GetDepthBuffer(),
                      m_gbuffer.GetNormalRoughness(),
                      ctx.width, ctx.height,
                      projMatrix, invProjMatrix);
}

// 6. Deferred Lighting Pass
m_lightingPass.Render(..., m_ssaoPass.GetSSAOTexture(), ...);
```

### DeferredLightingPass Changes

**File**: `Engine/Rendering/Deferred/DeferredLightingPass.h`

Add SSAO texture parameter:

```cpp
void Render(
    const CCamera& camera,
    CScene& scene,
    CGBuffer& gbuffer,
    RHI::ITexture* ssaoTexture,    // NEW: Can be nullptr if disabled
    RHI::ITexture* hdrOutput,
    // ... rest unchanged
);
```

**File**: `Shader/DeferredLighting.ps.hlsl`

Add SSAO texture binding and combine with material AO:

```hlsl
// New texture slot (after existing t5)
Texture2D gSSAO : register(t18);  // Use high slot to avoid conflicts

// In main():
float materialAO = rt2.a;
float ssao = gSSAO.Sample(gPointSampler, i.uv).r;
float finalAO = materialAO * ssao;  // Multiplicative combination

// Replace existing ao usage:
float3 ambient = (kD_IBL * diffuseGI + specularIBL) * finalAO;  // was: * ao
```

---

## Shader Implementation

### SSAO.cs.hlsl Structure (GTAO Algorithm)

```hlsl
// ============================================
// GTAO Compute Shader (Ground Truth Ambient Occlusion)
// ============================================
// Reference: "Practical Real-Time Strategies for Accurate Indirect Occlusion"
//            Jorge Jimenez, Xian-Chun Wu, Angelo Pesce, Adrian Jarabo (2016)

// Input textures
Texture2D gDepth : register(t0);
Texture2D gNormal : register(t1);
Texture2D gNoise : register(t2);

// Output
RWTexture2D<float> gSSAOOutput : register(u0);

// Samplers
SamplerState gPointSampler : register(s0);

// Constants
cbuffer CB_SSAO : register(b0) {
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gView;         // For world→view normal transform
    float2 gTexelSize;      // 1.0 / resolution (half-res)
    float2 gNoiseScale;     // resolution / 4.0 (noise tiling)
    float gRadius;          // AO radius in view-space units
    float gIntensity;       // AO strength multiplier
    float gFalloffStart;    // Distance falloff start (0.0-1.0 of radius)
    float gFalloffEnd;      // Distance falloff end (should be 1.0)
    int gNumSlices;         // Number of direction slices (2-4)
    int gNumSteps;          // Steps per direction (4-8)
    float gThicknessHeuristic; // Thin object heuristic threshold
    float _pad;
};

#define PI 3.14159265359
#define HALF_PI 1.5707963268

// ============================================
// Helper Functions
// ============================================

// Linearize depth to view-space Z
float LinearizeDepth(float depth) {
    return gProj[3][2] / (depth - gProj[2][2]);
}

// Reconstruct view-space position from UV and depth
float3 ReconstructViewPos(float2 uv, float depth) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;  // DirectX convention
    float4 viewPos = mul(clipPos, gInvProj);
    return viewPos.xyz / viewPos.w;
}

// Get view-space normal
float3 GetViewNormal(float2 uv) {
    float3 worldNormal = gNormal.SampleLevel(gPointSampler, uv, 0).xyz;
    // Transform world normal to view space
    float3 viewNormal = mul((float3x3)gView, worldNormal);
    return normalize(viewNormal);
}

// Distance-based falloff
float ComputeFalloff(float dist) {
    float t = saturate((dist - gFalloffStart * gRadius) / ((gFalloffEnd - gFalloffStart) * gRadius));
    return 1.0 - t * t;
}

// ============================================
// GTAO Integral Functions
// ============================================

// Compute the inner integral for a single horizon
// h = horizon angle, n = projected normal angle
float GTAOIntegral(float h, float n) {
    float cosN = cos(n);
    float sinN = sin(n);
    // Integral: 0.25 * (-cos(2h) + cos(2n) + 2h*sin(n))
    return 0.25 * (-cos(2.0 * h) + cosN * cosN + 2.0 * h * sinN);
}

// Compute AO for a single slice given two horizon angles and normal angle
float GTAOSlice(float h1, float h2, float n) {
    // h1 = horizon in negative direction (should be negative angle)
    // h2 = horizon in positive direction (should be positive angle)
    // n  = projected normal angle

    // Clamp horizons to hemisphere
    h1 = max(h1, n - HALF_PI);
    h2 = min(h2, n + HALF_PI);

    // Compute integral for both horizons
    float ao = GTAOIntegral(h2, n) - GTAOIntegral(h1, n);

    // Normalize to [0,1] (full hemisphere would be 1.0)
    return ao;
}

// ============================================
// GTAO Main Function
// ============================================

float ComputeGTAO(float3 viewPos, float3 viewNormal, float2 uv, float2 noiseDir) {
    float totalAO = 0.0;

    // Slice angle step
    float sliceAngleStep = PI / float(gNumSlices);

    for (int slice = 0; slice < gNumSlices; slice++) {
        // Slice direction in screen space (rotated by noise)
        float sliceAngle = float(slice) * sliceAngleStep;
        float2 sliceDir;
        sliceDir.x = cos(sliceAngle) * noiseDir.x - sin(sliceAngle) * noiseDir.y;
        sliceDir.y = sin(sliceAngle) * noiseDir.x + cos(sliceAngle) * noiseDir.y;

        // Convert screen direction to view-space direction (on XY plane at Z=1)
        float3 sliceDirView = float3(sliceDir.x, sliceDir.y, 0.0);
        sliceDirView = normalize(sliceDirView - viewNormal * dot(viewNormal, sliceDirView));

        // Project normal onto slice plane to get 'n' angle
        float3 orthoDir = normalize(cross(sliceDirView, float3(0, 0, 1)));
        float3 projNormal = viewNormal - orthoDir * dot(viewNormal, orthoDir);
        projNormal = normalize(projNormal);
        float n = atan2(dot(projNormal, sliceDirView), projNormal.z);

        // March in both directions to find horizons
        float h1 = -HALF_PI;  // Negative direction horizon
        float h2 = -HALF_PI;  // Positive direction horizon (will become positive)

        float stepScale = gRadius / float(gNumSteps);

        // Negative direction (h1)
        for (int s = 1; s <= gNumSteps; s++) {
            float2 sampleUV = uv - sliceDir * gTexelSize * float(s) * stepScale;
            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (sampleDepth >= 1.0) continue;

            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist < gRadius) {
                float falloff = ComputeFalloff(dist);
                float3 sampleDir = diff / dist;

                // Horizon angle relative to view direction (Z axis)
                float horizonAngle = atan2(dot(sampleDir, sliceDirView), -sampleDir.z);
                h1 = max(h1, horizonAngle * falloff + (1.0 - falloff) * (-HALF_PI));
            }
        }

        // Positive direction (h2)
        for (int s = 1; s <= gNumSteps; s++) {
            float2 sampleUV = uv + sliceDir * gTexelSize * float(s) * stepScale;
            if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) break;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            if (sampleDepth >= 1.0) continue;

            float3 samplePos = ReconstructViewPos(sampleUV, sampleDepth);
            float3 diff = samplePos - viewPos;
            float dist = length(diff);

            if (dist < gRadius) {
                float falloff = ComputeFalloff(dist);
                float3 sampleDir = diff / dist;

                float horizonAngle = atan2(dot(sampleDir, sliceDirView), -sampleDir.z);
                h2 = max(h2, horizonAngle * falloff + (1.0 - falloff) * (-HALF_PI));
            }
        }

        // Negate h1 for proper integral (it's in negative direction)
        h1 = -h1;

        // Compute slice AO
        float sliceAO = GTAOSlice(-h1, h2, n);
        totalAO += sliceAO;
    }

    // Average and apply intensity
    totalAO /= float(gNumSlices);
    totalAO = saturate(1.0 - totalAO * gIntensity);

    return totalAO;
}

// ============================================
// Main Compute Shader
// ============================================

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixelCoord = dispatchThreadID.xy;
    float2 uv = (float2(pixelCoord) + 0.5) * gTexelSize;

    // Sample depth
    float depth = gDepth.SampleLevel(gPointSampler, uv, 0).r;

    // Skip sky pixels
    if (depth >= 1.0) {
        gSSAOOutput[pixelCoord] = 1.0;
        return;
    }

    // Reconstruct view-space position and normal
    float3 viewPos = ReconstructViewPos(uv, depth);
    float3 viewNormal = GetViewNormal(uv);

    // Sample noise for random rotation (tiles across screen)
    float2 noiseUV = uv * gNoiseScale;
    float2 noise = gNoise.SampleLevel(gPointSampler, noiseUV, 0).xy;

    // Convert noise to rotation direction
    float noiseAngle = noise.x * PI * 2.0;
    float2 noiseDir = float2(cos(noiseAngle), sin(noiseAngle));

    // Compute GTAO
    float ao = ComputeGTAO(viewPos, viewNormal, uv, noiseDir);

    gSSAOOutput[pixelCoord] = ao;
}
```

### Bilateral Blur Shader

```hlsl
// ============================================
// Bilateral Blur (Edge-Preserving)
// ============================================

Texture2D gSSAOInput : register(t0);
Texture2D gDepth : register(t1);
RWTexture2D<float> gSSAOOutput : register(u0);

cbuffer CB_Blur : register(b0) {
    float2 gBlurDirection;  // (1,0) for horizontal, (0,1) for vertical
    float2 gTexelSize;
    float gDepthSigma;      // Depth edge threshold
    int gBlurRadius;        // 2-4 typical
    float2 _pad;
};

static const float GAUSSIAN_WEIGHTS[5] = {0.227, 0.194, 0.121, 0.054, 0.016};

[numthreads(8, 8, 1)]
void CSBlurH(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixelCoord = dispatchThreadID.xy;
    float2 uv = (float2(pixelCoord) + 0.5) * gTexelSize;

    float centerDepth = gDepth.SampleLevel(gPointSampler, uv, 0).r;
    float centerAO = gSSAOInput.SampleLevel(gPointSampler, uv, 0).r;

    // Skip sky
    if (centerDepth >= 1.0) {
        gSSAOOutput[pixelCoord] = 1.0;
        return;
    }

    float aoSum = centerAO * GAUSSIAN_WEIGHTS[0];
    float weightSum = GAUSSIAN_WEIGHTS[0];

    for (int i = 1; i <= gBlurRadius; i++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            float2 offset = float2(1, 0) * gTexelSize * float(i * sign);
            float2 sampleUV = uv + offset;

            float sampleDepth = gDepth.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOInput.SampleLevel(gPointSampler, sampleUV, 0).r;

            // Bilateral weight: depth difference
            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * depthDiff / (gDepthSigma * gDepthSigma));

            float weight = GAUSSIAN_WEIGHTS[i] * depthWeight;
            aoSum += sampleAO * weight;
            weightSum += weight;
        }
    }

    gSSAOOutput[pixelCoord] = aoSum / weightSum;
}

[numthreads(8, 8, 1)]
void CSBlurV(uint3 dispatchThreadID : SV_DispatchThreadID) {
    // Same as CSBlurH but with float2(0, 1) direction
    // ... (identical logic, vertical direction)
}
```

### Bilateral Upsample Shader

```hlsl
// ============================================
// Bilateral Upsample (Half-Res → Full-Res)
// ============================================

Texture2D gSSAOHalfRes : register(t0);
Texture2D gDepthHalfRes : register(t1);
Texture2D gDepthFullRes : register(t2);
RWTexture2D<float> gSSAOFullRes : register(u0);

cbuffer CB_Upsample : register(b0) {
    float2 gFullResTexelSize;
    float2 gHalfResTexelSize;
    float gDepthSigma;
    float3 _pad;
};

[numthreads(8, 8, 1)]
void CSBilateralUpsample(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 fullResCoord = dispatchThreadID.xy;
    float2 fullResUV = (float2(fullResCoord) + 0.5) * gFullResTexelSize;

    // Sample depth at full resolution
    float centerDepth = gDepthFullRes.SampleLevel(gPointSampler, fullResUV, 0).r;

    // Skip sky
    if (centerDepth >= 1.0) {
        gSSAOFullRes[fullResCoord] = 1.0;
        return;
    }

    // Sample 4 nearest half-res AO values
    float aoSum = 0.0;
    float weightSum = 0.0;

    // 2x2 bilinear with depth weighting
    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            float2 offset = (float2(x, y) - 0.5) * gHalfResTexelSize;
            float2 sampleUV = fullResUV + offset;

            float sampleDepth = gDepthHalfRes.SampleLevel(gPointSampler, sampleUV, 0).r;
            float sampleAO = gSSAOHalfRes.SampleLevel(gLinearSampler, sampleUV, 0).r;

            // Depth-aware weight
            float depthDiff = abs(centerDepth - sampleDepth);
            float weight = exp(-depthDiff * depthDiff * gDepthSigma);

            aoSum += sampleAO * weight;
            weightSum += weight;
        }
    }

    gSSAOFullRes[fullResCoord] = aoSum / max(weightSum, 0.0001);
}
```

---

## Editor Integration

### Scene Light Settings Panel

Add SSAO controls to the existing lighting panel:

```cpp
// In Panels_SceneLightSettings.cpp

if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen)) {
    auto& ssaoSettings = pipeline->GetSSAOSettings();

    ImGui::Checkbox("Enabled", &ssaoSettings.enabled);

    if (ssaoSettings.enabled) {
        ImGui::SliderFloat("Radius", &ssaoSettings.radius, 0.1f, 2.0f);
        ImGui::SliderFloat("Intensity", &ssaoSettings.intensity, 0.0f, 3.0f);
        ImGui::SliderFloat("Bias", &ssaoSettings.bias, 0.0f, 0.1f);

        const char* dirOptions[] = {"4 Directions", "8 Directions"};
        int dirIndex = (ssaoSettings.numDirections == 8) ? 1 : 0;
        if (ImGui::Combo("Quality", &dirIndex, dirOptions, 2)) {
            ssaoSettings.numDirections = (dirIndex == 1) ? 8 : 4;
        }
    }
}
```

### Debug Visualization

Add SSAO to G-Buffer debug modes:

```cpp
// In DeferredRenderPipeline.h
enum class EGBufferDebugMode : int {
    // ... existing modes ...
    SSAO = 11          // NEW: Visualize SSAO buffer
};
```

---

## Texture Formats & Memory

**Half-Resolution Strategy**: SSAO computed at half-res (540p for 1080p), then bilateral upsampled.

| Texture | Format | Resolution | Memory @ 1080p |
|---------|--------|------------|----------------|
| m_ssaoRaw | R8_UNORM | Half (960x540) | 0.5 MB |
| m_ssaoBlurTemp | R8_UNORM | Half (960x540) | 0.5 MB |
| m_ssaoHalfBlurred | R8_UNORM | Half (960x540) | 0.5 MB |
| m_ssaoFinal | R8_UNORM | Full (1920x1080) | 2 MB |
| m_noiseTexture | R8G8_UNORM | 4x4 | 32 bytes |

**Total**: ~3.5 MB additional VRAM

---

## Implementation Steps

### Step 1: SSAOPass Infrastructure
- [ ] Create `SSAOPass.h` with class declaration
- [ ] Create `SSAOPass.cpp` with Initialize/Shutdown
- [ ] Create half-res textures (raw, temp, blurred)
- [ ] Create full-res output texture
- [ ] Create half-res depth texture for upsample
- [ ] Create noise texture (4x4 random rotations)
- [ ] Create samplers (point, linear)

### Step 2: SSAO Compute Shader (HBAO at Half-Res)
- [ ] Create `SSAO.cs.hlsl` with HBAO implementation
- [ ] Implement depth linearization and position reconstruction
- [ ] Implement horizon tracing with 4/8 directions
- [ ] Compile and test raw AO output at half-res

### Step 3: Bilateral Blur (Half-Res)
- [ ] Add `CSBlurH` horizontal blur entry point
- [ ] Add `CSBlurV` vertical blur entry point
- [ ] Implement depth-aware bilateral weighting
- [ ] Create blur PSOs and dispatch

### Step 4: Bilateral Upsample (Half-Res → Full-Res)
- [ ] Add `CSBilateralUpsample` entry point
- [ ] Sample full-res depth for edge detection
- [ ] Implement depth-weighted bilinear interpolation
- [ ] Create upsample PSO and dispatch

### Step 5: Pipeline Integration
- [ ] Add `CSSAOPass` member to `CDeferredRenderPipeline`
- [ ] Insert SSAO pass between clustered lighting and deferred lighting
- [ ] Modify `CDeferredLightingPass::Render()` to accept SSAO texture
- [ ] Update `DeferredLighting.ps.hlsl` to sample SSAO at slot t18
- [ ] Combine with material AO: `finalAO = materialAO * ssao`

### Step 6: SceneLightSettings Integration
- [ ] Add SSAO settings struct to `SceneLightSettings.h`
- [ ] Add serialization for SSAO settings (JSON)
- [ ] Add SSAO section to Scene Light Settings panel (ImGui)
- [ ] Expose: enabled, radius, intensity, bias, numDirections

### Step 7: Debug Visualization
- [ ] Add `SSAO = 11` to `EGBufferDebugMode` enum
- [ ] Implement SSAO buffer visualization in debug pass
- [ ] Allow viewing raw half-res vs final upsampled

### Step 8: Testing & Validation
- [ ] Create `TestSSAO` automated test
- [ ] Verify corners/crevices are darkened
- [ ] Verify no halo artifacts at depth edges
- [ ] Test DX11 and DX12 backends
- [ ] Performance profiling (target < 0.5ms at 1080p)

---

## Test Case: TestSSAO

```cpp
// Tests/TestSSAO.cpp

class CTestSSAO : public ITestCase {
    void Setup() override {
        // Load scene with varied geometry (corners, walls, objects)
        LoadScene("test_ssao_scene.json");

        // Enable SSAO with default settings
        auto& settings = GetPipeline()->GetSSAOSettings();
        settings.enabled = true;
        settings.radius = 0.5f;
        settings.intensity = 1.0f;
    }

    void OnFrame(int frame) override {
        if (frame == 20) {
            // Screenshot with SSAO
            TakeScreenshot("ssao_enabled");

            // Verify SSAO buffer is not all white (occlusion exists)
            auto ssaoTex = GetPipeline()->GetSSAOPass().GetSSAOTexture();
            auto pixels = ReadbackTexture(ssaoTex);
            float avgAO = ComputeAverage(pixels);

            ASSERT(avgAO < 0.95f, "SSAO should produce some occlusion");
            ASSERT(avgAO > 0.3f, "SSAO should not be completely black");
        }

        if (frame == 40) {
            // Disable SSAO and compare
            GetPipeline()->GetSSAOSettings().enabled = false;
        }

        if (frame == 60) {
            TakeScreenshot("ssao_disabled");
            EndTest();
        }
    }
};
REGISTER_TEST(CTestSSAO);
```

---

## Performance Targets (Half-Resolution)

| Metric | Target | Notes |
|--------|--------|-------|
| GPU Time (SSAO) | < 0.3 ms @ 1080p | Half-res compute, 4 directions |
| GPU Time (Blur) | < 0.1 ms @ 1080p | Half-res bilateral blur |
| GPU Time (Upsample) | < 0.1 ms @ 1080p | Bilateral upsample to full-res |
| **Total** | **< 0.5 ms @ 1080p** | All SSAO passes combined |
| Memory | < 4 MB | All SSAO textures combined |

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Halo artifacts at edges | Tune bilateral blur/upsample depth sigma |
| Noise visible | Increase blur radius or add temporal filter in TAA phase |
| DX12 resource state issues | Follow existing pattern from ClusteredLightingPass |
| Performance regression | Profile each pass, reduce directions/steps if needed |

---

## Files to Modify

**New Files:**
- `Engine/Rendering/SSAOPass.h`
- `Engine/Rendering/SSAOPass.cpp`
- `Shader/SSAO.cs.hlsl`
- `Tests/TestSSAO.cpp`

**Modified Files:**
- `Engine/Rendering/Deferred/DeferredRenderPipeline.h` - Add SSAOPass member
- `Engine/Rendering/Deferred/DeferredRenderPipeline.cpp` - Integrate SSAO pass
- `Engine/Rendering/Deferred/DeferredLightingPass.h` - Add SSAO texture param
- `Engine/Rendering/Deferred/DeferredLightingPass.cpp` - Bind SSAO texture
- `Shader/DeferredLighting.ps.hlsl` - Sample and apply SSAO (t18)
- `Engine/SceneLightSettings.h` - Add SSAO settings struct
- `Editor/Panels_SceneLightSettings.cpp` - Add SSAO UI controls
- `CMakeLists.txt` - Add new source files

---

## Acceptance Criteria

1. SSAO produces visible darkening in corners and crevices
2. No halo artifacts at object edges
3. Works correctly on both DX11 and DX12 backends
4. Performance < 0.5ms at 1080p
5. Editor controls adjust AO in real-time
6. TestSSAO passes
7. SSAO combines correctly with material AO from textures
