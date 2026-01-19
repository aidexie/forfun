// ============================================
// Depth of Field - Pass 1: Circle of Confusion Calculation
// ============================================
// Computes signed Circle of Confusion from depth buffer
// Negative = near field (in front of focus)
// Positive = far field (behind focus)
// Zero = in focus
// ============================================

cbuffer CB_CoC : register(b0) {
    float gFocusDistance;
    float gFocalRange;
    float gAperture;
    float gMaxCoCRadius;
    float gNearZ;
    float gFarZ;
    float2 gTexelSize;
};

Texture2D gDepthBuffer : register(t0);
SamplerState gPointSampler : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Linearize depth from reversed-Z depth buffer
float LinearizeDepth(float rawDepth) {
    // Reversed-Z: near=1, far=0
    // Convert to linear view-space depth
    float z = rawDepth;
    return gNearZ * gFarZ / (gFarZ + z * (gNearZ - gFarZ));
}

float main(PSIn input) : SV_Target {
    float rawDepth = gDepthBuffer.SampleLevel(gPointSampler, input.uv, 0).r;
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
