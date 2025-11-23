// DebugLine.ps.hlsl
// Pixel Shader for debug line rendering

struct PSInput {
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR;
};

float4 main(PSInput input) : SV_TARGET {
    return input.color;
}
