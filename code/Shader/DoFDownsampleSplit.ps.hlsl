// ============================================
// Depth of Field - Pass 2: Downsample + Near/Far Split
// ============================================
// Downsamples HDR to half-res and splits into near/far layers
// based on the sign of the Circle of Confusion
// ============================================

Texture2D gHDRInput : register(t0);
Texture2D gCoCBuffer : register(t1);
SamplerState gLinearSampler : register(s0);
SamplerState gPointSampler : register(s1);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut {
    float4 nearColor : SV_Target0;  // Near layer color + alpha
    float4 farColor : SV_Target1;   // Far layer color + alpha
    float nearCoC : SV_Target2;     // Near CoC (absolute value)
    float farCoC : SV_Target3;      // Far CoC (absolute value)
};

PSOut main(PSIn input) {
    PSOut output;

    // Sample color (bilinear for quality downsample)
    float4 color = gHDRInput.SampleLevel(gLinearSampler, input.uv, 0);

    // Sample CoC (point for accuracy)
    float coc = gCoCBuffer.SampleLevel(gPointSampler, input.uv, 0).r;

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
