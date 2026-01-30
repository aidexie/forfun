// ============================================
// DoF_DS.ps.hlsl - Depth of Field Post-Process (SM 5.1)
// ============================================
// Descriptor Set version using PerPass layout (space1)
//
// Entry Points:
//   PSCoC            - Pass 1: Circle of Confusion calculation
//   PSDownsampleSplit - Pass 2: Downsample + near/far split
//   PSBlurH          - Pass 3: Horizontal Gaussian blur
//   PSBlurV          - Pass 4: Vertical Gaussian blur
//   PSComposite      - Pass 5: Bilateral upsample + composite
// ============================================

// ============================================
// Set 1: PerPass (space1)
// ============================================

// Constant Buffer (b0, space1) - max 64 bytes
cbuffer CB_DoF : register(b0, space1) {
    float gFocusDistance;   // Focus plane distance (world units)
    float gFocalRange;      // Depth range in focus
    float gAperture;        // f-stop value
    float gMaxCoCRadius;    // Max CoC in pixels
    float gNearZ;           // Camera near plane
    float gFarZ;            // Camera far plane
    float2 gTexelSize;      // 1.0 / resolution
    int gSampleCount;       // Number of blur samples
    float3 _pad;
};

// Texture SRVs (t0-t5, space1)
Texture2D gInput0 : register(t0, space1);  // Primary input (depth/HDR/color)
Texture2D gInput1 : register(t1, space1);  // Secondary input (CoC/CoC)
Texture2D gInput2 : register(t2, space1);  // Near blurred
Texture2D gInput3 : register(t3, space1);  // Far blurred
Texture2D gInput4 : register(t4, space1);  // Near CoC
Texture2D gInput5 : register(t5, space1);  // Far CoC

// Samplers (s0-s1, space1)
SamplerState gLinearSampler : register(s0, space1);
SamplerState gPointSampler : register(s1, space1);

// ============================================
// Structures
// ============================================
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutSplit {
    float4 nearColor : SV_Target0;  // Near layer color + alpha
    float4 farColor : SV_Target1;   // Far layer color + alpha
    float nearCoC : SV_Target2;     // Near CoC (absolute value)
    float farCoC : SV_Target3;      // Far CoC (absolute value)
};

// ============================================
// Helper Functions
// ============================================

// Linearize depth from reversed-Z depth buffer
float LinearizeDepth(float rawDepth) {
    // Reversed-Z: near=1, far=0
    // Convert to linear view-space depth
    float z = rawDepth;
    return gNearZ * gFarZ / (gFarZ + z * (gNearZ - gFarZ));
}

// Gaussian weights for 11-tap blur
static const float kGaussianWeights[11] = {
    0.0093, 0.028, 0.0659, 0.1216, 0.1756,
    0.1974,  // Center
    0.1756, 0.1216, 0.0659, 0.028, 0.0093
};

// ============================================
// Pass 1: Circle of Confusion Calculation
// ============================================
// Input0 = Depth buffer
float PSCoC(VSOut input) : SV_Target {
    float rawDepth = gInput0.SampleLevel(gPointSampler, input.uv, 0).r;
    float linearDepth = LinearizeDepth(rawDepth);

    // Calculate signed CoC
    // Negative = near field, Positive = far field, Zero = in focus
    float depthDiff = linearDepth - gFocusDistance;

    // Smooth transition using focal range
    float coc = depthDiff / gFocalRange;

    // Scale by aperture (lower f-stop = more blur)
    // Normalize aperture: f/2.8 as baseline (1.0), f/1.4 = 2.0, f/5.6 = 0.5
    float apertureScale = 2.8 / max(gAperture, 0.1);
    coc *= apertureScale;

    // Clamp to max radius (in normalized units, will be scaled to pixels later)
    coc = clamp(coc, -1.0, 1.0);

    return coc;
}

// ============================================
// Pass 2: Downsample + Near/Far Split
// ============================================
// Input0 = HDR color, Input1 = CoC buffer
PSOutSplit PSDownsampleSplit(VSOut input) {
    PSOutSplit output;

    // Sample color (bilinear for quality downsample)
    float4 color = gInput0.SampleLevel(gLinearSampler, input.uv, 0);

    // Sample CoC (point for accuracy)
    float coc = gInput1.SampleLevel(gPointSampler, input.uv, 0).r;

    // Split into near/far based on CoC sign
    if (coc < 0.0) {
        // Near field (in front of focus)
        output.nearColor = float4(color.rgb, 1.0);
        output.nearCoC = abs(coc);
        output.farColor = float4(0.0, 0.0, 0.0, 0.0);
        output.farCoC = 0.0;
    } else {
        // Far field (behind focus) or in-focus
        output.farColor = float4(color.rgb, 1.0);
        output.farCoC = coc;
        output.nearColor = float4(0.0, 0.0, 0.0, 0.0);
        output.nearCoC = 0.0;
    }

    return output;
}

// ============================================
// Pass 3: Horizontal Gaussian Blur
// ============================================
// Input0 = Color input, Input1 = CoC input
float4 PSBlurH(VSOut input) : SV_Target {
    static const float2 kBlurDir = float2(1.0, 0.0);  // Horizontal

    float centerCoC = gInput1.SampleLevel(gLinearSampler, input.uv, 0).r;

    // Find max CoC in neighborhood - this allows blur to spread into lower-CoC areas
    float maxNeighborCoC = centerCoC;
    for (int p = 0; p < 11; ++p) {
        float preOffset = (float)(p - 5);
        float2 preUV = input.uv + kBlurDir * gTexelSize * preOffset * gMaxCoCRadius;
        preUV = saturate(preUV);
        float neighborCoC = gInput1.SampleLevel(gLinearSampler, preUV, 0).r;
        maxNeighborCoC = max(maxNeighborCoC, neighborCoC);
    }

    // Use max neighbor CoC for blur radius (gather-as-scatter)
    float blurRadius = maxNeighborCoC * gMaxCoCRadius;

    // Early out if no blur needed
    if (blurRadius < 0.5) {
        return gInput0.SampleLevel(gLinearSampler, input.uv, 0);
    }

    float4 colorSum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;

    // 11-tap Gaussian blur
    for (int i = 0; i < 11; ++i) {
        float offset = (float)(i - 5);  // -5 to +5
        float2 sampleUV = input.uv + kBlurDir * gTexelSize * offset * blurRadius;
        sampleUV = saturate(sampleUV);

        float4 sampleColor = gInput0.SampleLevel(gLinearSampler, sampleUV, 0);
        float sampleCoC = gInput1.SampleLevel(gLinearSampler, sampleUV, 0).r;

        float weight = kGaussianWeights[i];

        // Weight by sample's CoC - higher CoC samples contribute more to blur
        // This simulates scatter: blurry pixels "spread" their color to neighbors
        float cocContribution = saturate(sampleCoC * 2.0 + 0.1);
        weight *= cocContribution;

        // Soft bilateral weight to prevent extreme CoC differences from mixing
        float cocDiff = abs(sampleCoC - centerCoC);
        weight *= exp(-cocDiff * 1.5);

        colorSum += sampleColor * weight;
        weightSum += weight;
    }

    // Fallback to center color if no valid samples
    if (weightSum < 0.0001) {
        return gInput0.SampleLevel(gLinearSampler, input.uv, 0);
    }

    return colorSum / weightSum;
}

// ============================================
// Pass 4: Vertical Gaussian Blur
// ============================================
// Input0 = Color input, Input1 = CoC input
float4 PSBlurV(VSOut input) : SV_Target {
    static const float2 kBlurDir = float2(0.0, 1.0);  // Vertical

    float centerCoC = gInput1.SampleLevel(gLinearSampler, input.uv, 0).r;

    // Find max CoC in neighborhood - this allows blur to spread into lower-CoC areas
    float maxNeighborCoC = centerCoC;
    for (int p = 0; p < 11; ++p) {
        float preOffset = (float)(p - 5);
        float2 preUV = input.uv + kBlurDir * gTexelSize * preOffset * gMaxCoCRadius;
        preUV = saturate(preUV);
        float neighborCoC = gInput1.SampleLevel(gLinearSampler, preUV, 0).r;
        maxNeighborCoC = max(maxNeighborCoC, neighborCoC);
    }

    // Use max neighbor CoC for blur radius (gather-as-scatter)
    float blurRadius = maxNeighborCoC * gMaxCoCRadius;

    // Early out if no blur needed
    if (blurRadius < 0.5) {
        return gInput0.SampleLevel(gLinearSampler, input.uv, 0);
    }

    float4 colorSum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;

    // 11-tap Gaussian blur
    for (int i = 0; i < 11; ++i) {
        float offset = (float)(i - 5);  // -5 to +5
        float2 sampleUV = input.uv + kBlurDir * gTexelSize * offset * blurRadius;
        sampleUV = saturate(sampleUV);

        float4 sampleColor = gInput0.SampleLevel(gLinearSampler, sampleUV, 0);
        float sampleCoC = gInput1.SampleLevel(gLinearSampler, sampleUV, 0).r;

        float weight = kGaussianWeights[i];

        // Weight by sample's CoC - higher CoC samples contribute more to blur
        // This simulates scatter: blurry pixels "spread" their color to neighbors
        float cocContribution = saturate(sampleCoC * 2.0 + 0.1);
        weight *= cocContribution;

        // Soft bilateral weight to prevent extreme CoC differences from mixing
        float cocDiff = abs(sampleCoC - centerCoC);
        weight *= exp(-cocDiff * 1.5);

        colorSum += sampleColor * weight;
        weightSum += weight;
    }

    // Fallback to center color if no valid samples
    if (weightSum < 0.0001) {
        return gInput0.SampleLevel(gLinearSampler, input.uv, 0);
    }

    return colorSum / weightSum;
}

// ============================================
// Pass 5: Bilateral Upsample + Composite
// ============================================
// Input0 = HDR input, Input1 = CoC buffer
// Input2 = Near blurred, Input3 = Far blurred
// Input4 = Near CoC, Input5 = Far CoC
float4 PSComposite(VSOut input) : SV_Target {
    // Sample original sharp image
    float4 sharpColor = gInput0.SampleLevel(gLinearSampler, input.uv, 0);

    // Sample full-res CoC
    float coc = gInput1.SampleLevel(gPointSampler, input.uv, 0).r;
    float absCoc = abs(coc);

    // Sample blurred layers (bilinear upsample)
    float4 nearBlurred = gInput2.SampleLevel(gLinearSampler, input.uv, 0);
    float4 farBlurred = gInput3.SampleLevel(gLinearSampler, input.uv, 0);
    float nearCocVal = gInput4.SampleLevel(gLinearSampler, input.uv, 0).r;
    float farCocVal = gInput5.SampleLevel(gLinearSampler, input.uv, 0).r;

    // Find max CoC from neighbors for blur spread detection
    float maxNearCoc = nearCocVal;
    float maxFarCoc = farCocVal;
    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            float2 offset = float2(i, j) * gTexelSize * 2.0;
            float2 sampleUV = saturate(input.uv + offset);
            maxNearCoc = max(maxNearCoc, gInput4.SampleLevel(gLinearSampler, sampleUV, 0).r);
            maxFarCoc = max(maxFarCoc, gInput5.SampleLevel(gLinearSampler, sampleUV, 0).r);
        }
    }

    float4 result = sharpColor;

    // Far field blend: use max far CoC to allow blur spread into sharp areas
    float farBlend = saturate(max(absCoc, maxFarCoc * 0.5) * 2.0);
    if (coc > 0.0 || maxFarCoc > 0.1) {
        // Weight by both local CoC and blurred layer alpha
        float farWeight = farBlend * saturate(farBlurred.a + 0.3);
        result = lerp(sharpColor, farBlurred, farWeight);
    }

    // Near field overlay (foreground occludes background)
    // Near blur should spread INTO background, so use max near CoC
    float nearBlend = saturate(max(nearCocVal, maxNearCoc * 0.7) * 2.5);
    if (nearBlurred.a > 0.01 || maxNearCoc > 0.1) {
        float nearWeight = nearBlend * saturate(nearBlurred.a + 0.2);
        result = lerp(result, nearBlurred, nearWeight);
    }

    return float4(result.rgb, 1.0);
}
