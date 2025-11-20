// BRDF LUT Pixel Shader
// Generates 2D lookup table for Split Sum Approximation (UE4 method)
// Input: UV.x = cos(NdotV), UV.y = roughness
// Output: RG = (scale, bias) for Fresnel-Schlick approximation

static const float PI = 3.14159265359;

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

// Schlick-GGX geometry function (single direction)
float G_SchlickGGX(float NdotV, float roughness) {
    // Note: For IBL, we use a different k than for direct lighting
    // k_IBL = (roughness^2) / 2
    float a = roughness;
    float k = (a * a) / 2.0;

    float denom = NdotV * (1.0 - k) + k;
    return NdotV / max(denom, 0.001);
}

// Smith's method for geometry shadowing (combines V and L directions)
float G_Smith(float NdotV, float NdotL, float roughness) {
    float ggx1 = G_SchlickGGX(NdotV, roughness);
    float ggx2 = G_SchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Generate BRDF integration map
float2 IntegrateBRDF(float NdotV, float roughness) {
    // View vector in tangent space (pointing up along Z)
    float3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);  // sin(theta)
    V.y = 0.0;
    V.z = NdotV;                       // cos(theta)

    float A = 0.0;  // Scale for F0 (accumulates (1-F) * G * cosTheta)
    float B = 0.0;  // Bias (accumulates F * G * cosTheta)

    // Normal points straight up in tangent space
    float3 N = float3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        // Generate sample
        float2 Xi = Hammersley(i, SAMPLE_COUNT);

        // Importance sample GGX to get half vector H in tangent space
        float3 H = ImportanceSampleGGX(Xi, roughness);

        // Calculate light direction L (reflection of V around H)
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = saturate(L.z);
        float NdotH = saturate(H.z);
        float VdotH = saturate(dot(V, H));

        if (NdotL > 0.0) {
            // Geometry term
            float G = G_Smith(NdotV, NdotL, roughness);

            // Fresnel term (Schlick approximation)
            // F(θ) = F0 + (1 - F0) * (1 - cos(θ))^5
            // We factor out F0: F(θ) = F0 * (1 - Fc) + Fc
            // where Fc = (1 - cos(θ))^5
            float Fc = pow(1.0 - VdotH, 5.0);

            // For importance sampling, we need to weight by:
            // BRDF * cosTheta / pdf
            // pdf = D * NdotH / (4 * VdotH) for GGX importance sampling
            // BRDF = D * F * G / (4 * NdotV * NdotL)
            // So: (D * F * G / (4 * NdotV * NdotL)) * NdotL / (D * NdotH / (4 * VdotH))
            //   = (F * G * VdotH) / (NdotV * NdotH)

            float G_Vis = (G * VdotH) / max(NdotH * NdotV, 0.001);

            // Accumulate
            // A = ∫(1 - Fc) * G_Vis dω  (scale for F0)
            // B = ∫Fc * G_Vis dω        (bias, doesn't depend on F0)
            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    // Average over samples
    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);

    return float2(A, B);
}

struct PSInput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    // UV coordinates map to:
    // X: cos(NdotV) [0, 1] (0 = grazing angle, 1 = perpendicular)
    // Y: roughness [0, 1] (0 = mirror, 1 = fully rough)

    float NdotV = input.uv.x;
    float roughness = input.uv.y;

    // Clamp to avoid edge artifacts
    NdotV = clamp(NdotV, 0.0, 1.0);
    roughness = clamp(roughness, 0.0, 1.0);

    // Compute BRDF integration
    float2 brdf = IntegrateBRDF(NdotV, roughness);

    // Output: R = scale, G = bias
    // Final Fresnel in shader: F0 * brdf.x + brdf.y
    return float4(brdf.x, brdf.y, 0.0, 1.0);
}
