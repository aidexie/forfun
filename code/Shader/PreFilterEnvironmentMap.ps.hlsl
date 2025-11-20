// Pre-filter Environment Map Pixel Shader
// Implements importance sampling for GGX specular reflection

TextureCube gEnvironmentMap : register(t0);
SamplerState gSampler : register(s0);

cbuffer CB_FaceIndex : register(b0) {
    int gFaceIndex;  // 0-5 for +X,-X,+Y,-Y,+Z,-Z
    float3 _pad0;
};

cbuffer CB_Roughness : register(b1) {
    float gRoughness;  // Surface roughness [0, 1]
    float gResolution; // Environment map resolution (for mip level selection)
    float2 _pad1;
};

static const float PI = 3.14159265359;

// Convert UV + face index to 3D direction (DirectX cubemap convention)
float3 GetCubemapDirection(float2 uv, int face) {
    float2 tc = uv * 2.0 - 1.0;  // UV [0,1] -> [-1,1]

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

// Hammersley low-discrepancy sequence
// Returns 2D point in [0,1]^2 for sample i out of N samples
float2 Hammersley(uint i, uint N) {
    // Van der Corput sequence (radical inverse in base 2)
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float radicalInverse = float(bits) * 2.3283064365386963e-10; // / 0x100000000

    return float2(float(i) / float(N), radicalInverse);
}

// GGX importance sampling
// Returns half vector H in tangent space based on roughness
float3 ImportanceSampleGGX(float2 Xi, float roughness) {
    float a = roughness * roughness;

    // Spherical coordinates
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // Convert to Cartesian (tangent space)
    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    return H;
}

// Build TBN matrix from normal (column-major for tangent→world transform)
float3x3 BuildTBN(float3 N) {
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = normalize(cross(N, T));

    // CRITICAL: HLSL float3x3(v1,v2,v3) creates row-major matrix
    // We need column-major for tangent→world, so transpose
    return transpose(float3x3(T, B, N));
}

// GGX normal distribution function
float D_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0001);
}

// Helper: Calculate sample count based on roughness
// Low roughness needs MORE samples to reduce noise from point light sources
uint GetSampleCount(float roughness) {
    if (roughness < 0.1) {
        return 65536u;  // Mirror-like: maximum samples
    } else if (roughness < 0.3) {
        return 32768u;  // Very smooth: high samples
    } else if (roughness < 0.6) {
        return 16384u;  // Medium: moderate samples
    } else {
        return 8192u;   // Rough: converges faster
    }
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Get reflection direction N for this pixel (assumes N = V = R for pre-filtering)
    float3 N = GetCubemapDirection(uv, gFaceIndex);
    float3 R = N;  // Reflection vector = normal (viewing from infinity)
    float3 V = R;  // View vector = reflection (simplified assumption)

    // DEBUG: Direct sample to test if environment map is accessible
    // Uncomment this to verify environment map sampling works
    // float3 directSample = gEnvironmentMap.SampleLevel(gSampler, N, 0).rgb;
    // return float4(directSample, 1.0);

    // Dynamic sample count based on roughness
    uint SAMPLE_COUNT = GetSampleCount(gRoughness);

    // Build tangent space basis
    float3x3 TBN = BuildTBN(N);

    // Accumulate pre-filtered color
    float3 prefilteredColor = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        // Generate sample
        float2 Xi = Hammersley(i, SAMPLE_COUNT);

        // Importance sample GGX to get half vector H in tangent space
        float3 H_tangent = ImportanceSampleGGX(Xi, gRoughness);

        // Transform H to world space
        float3 H = normalize(mul(TBN, H_tangent));

        // Calculate light direction L (reflection of V around H)
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = dot(N, L);
        if (NdotL > 0.0) {
            float mipLevel;

            // Special case: mirror-like surfaces (roughness < 0.02)
            // Directly sample highest resolution to avoid PDF singularity
            if (gRoughness < 0.02) {
                mipLevel = 0.0;
            } else {
                // Calculate proper mip level based on solid angle
                // This approximates the relationship between sample cone and texel cone
                float NdotH = max(dot(N, H), 0.0);
                float VdotH = max(dot(V, H), 0.0);

                // GGX PDF: D(h) * NdotH / (4 * VdotH)
                float D = D_GGX(NdotH, gRoughness);
                float pdf = max(D * NdotH / (4.0 * VdotH), 0.0001);  // Prevent division by zero

                // Solid angle of sample vs solid angle of texel
                // saTexel = 4*pi / (6 * res^2), saSample = 1 / (pdf * sampleCount)
                float saTexel = 4.0 * PI / (6.0 * gResolution * gResolution);
                float saSample = 1.0 / (pdf * float(SAMPLE_COUNT));

                // Mip level from solid angle ratio
                mipLevel = 0.5 * log2(saSample / saTexel);

                // Clamp to valid range [0, maxMipLevel]
                mipLevel = clamp(mipLevel, 0.0, 8.0);
            }

            float3 color = gEnvironmentMap.SampleLevel(gSampler, L, mipLevel).rgb;

            // Add to accumulation with proper weighting
            prefilteredColor += color * NdotL;
            totalWeight += NdotL;
        }
    }

    // Normalize
    prefilteredColor = prefilteredColor / max(totalWeight, 0.0001);

    return float4(prefilteredColor, 1.0);
}
