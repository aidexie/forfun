// ============================================
// Depth of Field - Pass 5: Bilateral Upsample + Composite
// ============================================
// Upsamples blurred near/far layers and composites with
// the sharp original image based on Circle of Confusion
// ============================================

cbuffer CB_Composite : register(b0) {
    float2 gTexelSize;
    float2 _pad;
};

Texture2D gHDRInput : register(t0);      // Original sharp HDR
Texture2D gCoCBuffer : register(t1);     // Full-res CoC
Texture2D gNearBlurred : register(t2);   // Blurred near layer (half-res)
Texture2D gFarBlurred : register(t3);    // Blurred far layer (half-res)
Texture2D gNearCoC : register(t4);       // Near CoC (half-res)
Texture2D gFarCoC : register(t5);        // Far CoC (half-res)
SamplerState gLinearSampler : register(s0);
SamplerState gPointSampler : register(s1);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target {
    // Sample original sharp image
    float4 sharpColor = gHDRInput.SampleLevel(gLinearSampler, input.uv, 0);

    // Sample full-res CoC
    float coc = gCoCBuffer.SampleLevel(gPointSampler, input.uv, 0).r;
    float absCoc = abs(coc);

    // Sample blurred layers (bilinear upsample)
    float4 nearBlurred = gNearBlurred.SampleLevel(gLinearSampler, input.uv, 0);
    float4 farBlurred = gFarBlurred.SampleLevel(gLinearSampler, input.uv, 0);
    float nearCocVal = gNearCoC.SampleLevel(gLinearSampler, input.uv, 0).r;
    float farCocVal = gFarCoC.SampleLevel(gLinearSampler, input.uv, 0).r;

    // Find max CoC from neighbors for blur spread detection
    float maxNearCoc = nearCocVal;
    float maxFarCoc = farCocVal;
    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            float2 offset = float2(i, j) * gTexelSize * 2.0;
            float2 sampleUV = saturate(input.uv + offset);
            maxNearCoc = max(maxNearCoc, gNearCoC.SampleLevel(gLinearSampler, sampleUV, 0).r);
            maxFarCoc = max(maxFarCoc, gFarCoC.SampleLevel(gLinearSampler, sampleUV, 0).r);
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
