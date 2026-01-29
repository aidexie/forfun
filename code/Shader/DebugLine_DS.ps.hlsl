// DebugLine_DS.ps.hlsl
// Pixel Shader for debug line rendering (SM 5.1 - Descriptor Set path)

struct PSInput {
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR;
};

float4 main(PSInput input) : SV_TARGET {
    return input.color;
}
