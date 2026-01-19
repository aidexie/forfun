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

    // Composite: blend between sharp and blurred based on CoC
    float4 result = sharpColor;

    if (coc > 0.0) {
        // Far field: blend sharp -> blurred based on CoC
        float farBlend = saturate(absCoc * 3.0);  // Smooth transition
        result = lerp(sharpColor, farBlurred, farBlend * farBlurred.a);
    }

    // Near field always overlays on top (foreground occludes background)
    if (nearBlurred.a > 0.0) {
        float nearBlend = saturate(nearCocVal * 3.0);
        result = lerp(result, nearBlurred, nearBlend * nearBlurred.a);
    }

    return float4(result.rgb, 1.0);
}
