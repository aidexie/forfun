// ============================================
// IrradianceConvolution_DS.ps.hlsl - Irradiance PS (SM 5.1)
// ============================================
// Descriptor Set version using PerPass layout (space1)
// Computes diffuse irradiance by convolving environment map over hemisphere
//
// Entry Point: main
// ============================================

// ============================================
// Set 1: PerPass (space1)
// ============================================

// Constant Buffer (b0, space1)
cbuffer CB_FaceIndex : register(b0, space1) {
    int gFaceIndex;  // 0-5 for +X,-X,+Y,-Y,+Z,-Z
    float3 _pad;
};

// Texture SRV (t0, space1)
TextureCube gEnvironmentMap : register(t0, space1);

// Sampler (s0, space1)
SamplerState gSampler : register(s0, space1);

// ============================================
// Constants
// ============================================
static const float PI = 3.14159265359;

// ============================================
// Helper Functions
// ============================================

// Convert UV + face index to 3D direction
float3 GetCubemapDirection(float2 uv, int face) {
    // UV: [0,1] x [0,1], convert to [-1,1] x [-1,1]
    // Note: Texture V is top-to-bottom, so we DON'T flip here initially
    float2 tc = uv * 2.0 - 1.0;  // tc.x = u, tc.y = v, both in [-1, 1]

    // D3D11 cubemap convention (right-handed coordinate system)
    // Texture V goes top-to-bottom, so negate for world Y
    float3 dir;
    switch (face) {
        case 0: dir = float3( 1.0, -tc.y, -tc.x); break;  // +X (right)
        case 1: dir = float3(-1.0, -tc.y,  tc.x); break;  // -X (left)
        case 2: dir = float3( tc.x,  1.0,  tc.y); break;  // +Y (top)
        case 3: dir = float3( tc.x, -1.0, -tc.y); break;  // -Y (bottom)
        case 4: dir = float3( tc.x, -tc.y,  1.0); break;  // +Z (front)
        case 5: dir = float3(-tc.x, -tc.y, -1.0); break;  // -Z (back)
    }

    return normalize(dir);
}

// Build TBN matrix from normal
// Returns matrix that transforms tangent-space vectors to world-space
float3x3 BuildTBN(float3 N) {
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = normalize(cross(N, T));  // Add normalize for numerical stability

    // CRITICAL: In HLSL, float3x3(v1, v2, v3) creates a ROW-major matrix:
    // [ v1.x  v1.y  v1.z ]
    // [ v2.x  v2.y  v2.z ]
    // [ v3.x  v3.y  v3.z ]
    //
    // We need COLUMN-major for tangent->world transform:
    // [ T.x  B.x  N.x ]
    // [ T.y  B.y  N.y ]
    // [ T.z  B.z  N.z ]
    //
    // So we must TRANSPOSE the row-major construction
    return transpose(float3x3(T, B, N));
}

// Convolution: integrate incoming light over hemisphere
float3 ConvolveIrradiance(float3 N) {
    float3 irradiance = float3(0.0, 0.0, 0.0);

    // Build TBN (tangent-to-world transform matrix)
    // This is CRITICAL: we sample in tangent space (hemisphere around local Z)
    // but must transform to world space before sampling the environment map
    float3x3 TBN = BuildTBN(N);

    float sampleCount = 0.0;

    float sampleDelta = 0.005;
    float nrSamples = 0.0;
    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // spherical to cartesian (in tangent space)
            float3 tangentSample = float3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
            // tangent space to world
            float3 sampleVec = mul(TBN, tangentSample);
            irradiance += gEnvironmentMap.SampleLevel(gSampler, sampleVec, 0).rgb * cos(theta) * sin(theta);
            nrSamples+=1.0;
        }
    }
    irradiance = PI * irradiance * (1.0 / float(nrSamples));

    return irradiance;
}

// ============================================
// Pixel Shader
// ============================================
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Get direction for this pixel in the cubemap face
    float3 N = GetCubemapDirection(uv, gFaceIndex);

    // Convolve irradiance
    float3 irradiance = ConvolveIrradiance(N);
    return float4(irradiance, 1.0);
}
