// ============================================
// DXR Lightmap Baking - Common Utilities
// ============================================
// Shared functions for ray tracing lightmap baking:
// - Random number generation (PCG)
// - Hemisphere sampling
// - Spherical harmonics (L1/L2)
// - Utility functions

#ifndef LIGHTMAP_BAKE_COMMON_HLSL
#define LIGHTMAP_BAKE_COMMON_HLSL

// ============================================
// Constants
// ============================================

static const float PI = 3.14159265359f;
static const float TWO_PI = 6.28318530718f;
static const float INV_PI = 0.31830988618f;
static const float INV_TWO_PI = 0.15915494309f;

// ============================================
// Random Number Generation (PCG)
// ============================================

// Initialize RNG state from voxel index and frame
uint InitRNG(uint3 voxelIdx, uint frameIdx) {
    // Hash the inputs together for a unique seed
    uint seed = voxelIdx.x * 1973u + voxelIdx.y * 9277u + voxelIdx.z * 26699u + frameIdx * 103u;
    return seed;
}

// Initialize RNG with additional sample index
uint InitRNGWithSample(uint3 voxelIdx, uint frameIdx, uint sampleIdx) {
    uint seed = voxelIdx.x * 1973u + voxelIdx.y * 9277u + voxelIdx.z * 26699u;
    seed += frameIdx * 103u + sampleIdx * 12289u;
    return seed;
}

// PCG random number generator
// Returns float in [0, 1)
float Random(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) / 4294967295.0f;
}

// Generate two random numbers at once (for efficiency)
float2 Random2(inout uint state) {
    return float2(Random(state), Random(state));
}

// Generate three random numbers
float3 Random3(inout uint state) {
    return float3(Random(state), Random(state), Random(state));
}

// ============================================
// Sampling Functions
// ============================================

// Sample uniformly on unit sphere
float3 SampleSphereUniform(inout uint rng) {
    float u1 = Random(rng);
    float u2 = Random(rng);

    float z = 1.0f - 2.0f * u1;
    float r = sqrt(max(0.0f, 1.0f - z * z));
    float phi = TWO_PI * u2;

    return float3(r * cos(phi), r * sin(phi), z);
}

// Sample uniformly on hemisphere (above z=0 plane)
float3 SampleHemisphereUniform(inout uint rng) {
    float u1 = Random(rng);
    float u2 = Random(rng);

    float z = u1;
    float r = sqrt(max(0.0f, 1.0f - z * z));
    float phi = TWO_PI * u2;

    return float3(r * cos(phi), r * sin(phi), z);
}

// Cosine-weighted hemisphere sampling (importance sampling for Lambertian BRDF)
// Returns direction in local space (z-up)
float3 SampleHemisphereCosine(inout uint rng) {
    float u1 = Random(rng);
    float u2 = Random(rng);

    float r = sqrt(u1);
    float phi = TWO_PI * u2;

    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0f, 1.0f - u1));

    return float3(x, y, z);
}

// Build orthonormal basis from normal (Frisvad method - fast, no branching issues)
void BuildOrthonormalBasis(float3 n, out float3 tangent, out float3 bitangent) {
    if (n.z < -0.9999999f) {
        tangent = float3(0.0f, -1.0f, 0.0f);
        bitangent = float3(-1.0f, 0.0f, 0.0f);
    } else {
        float a = 1.0f / (1.0f + n.z);
        float b = -n.x * n.y * a;
        tangent = float3(1.0f - n.x * n.x * a, b, -n.x);
        bitangent = float3(b, 1.0f - n.y * n.y * a, -n.y);
    }
}

// Transform local direction to world space using normal
float3 LocalToWorld(float3 localDir, float3 normal) {
    float3 tangent, bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);
    return localDir.x * tangent + localDir.y * bitangent + localDir.z * normal;
}

// Cosine-weighted hemisphere sampling in world space
float3 SampleHemisphereCosineWorld(float3 normal, inout uint rng) {
    float3 localDir = SampleHemisphereCosine(rng);
    return LocalToWorld(localDir, normal);
}

// ============================================
// Spherical Harmonics (L1 - 4 coefficients)
// ============================================

// Evaluate L1 SH basis functions
// Returns 4 coefficients: [L0, L1_y, L1_z, L1_x]
float4 EvaluateSHL1Basis(float3 dir) {
    // Normalization constants
    static const float c0 = 0.282095f;      // sqrt(1/(4*PI))
    static const float c1 = 0.488603f;      // sqrt(3/(4*PI))

    return float4(
        c0,             // L0: constant
        c1 * dir.y,     // L1,-1: y
        c1 * dir.z,     // L1,0: z
        c1 * dir.x      // L1,+1: x
    );
}

// Accumulate radiance to L1 SH coefficients
void AccumulateToSHL1(float3 dir, float3 radiance, inout float3 shCoeffs[4]) {
    float4 basis = EvaluateSHL1Basis(dir);

    shCoeffs[0] += radiance * basis.x;
    shCoeffs[1] += radiance * basis.y;
    shCoeffs[2] += radiance * basis.z;
    shCoeffs[3] += radiance * basis.w;
}

// Reconstruct irradiance from L1 SH coefficients
float3 ReconstructIrradianceSHL1(float3 shCoeffs[4], float3 normal) {
    float4 basis = EvaluateSHL1Basis(normal);

    // Irradiance reconstruction (with cosine lobe convolution)
    // A0 = PI, A1 = 2*PI/3
    static const float A0 = PI;
    static const float A1 = 2.094395f;  // 2*PI/3

    float3 irradiance = A0 * shCoeffs[0] * basis.x;
    irradiance += A1 * shCoeffs[1] * basis.y;
    irradiance += A1 * shCoeffs[2] * basis.z;
    irradiance += A1 * shCoeffs[3] * basis.w;

    return max(0.0f, irradiance);
}

// ============================================
// Spherical Harmonics (L2 - 9 coefficients)
// ============================================

// Evaluate L2 SH basis functions
void EvaluateSHL2Basis(float3 dir, out float basis[9]) {
    // L0
    basis[0] = 0.282095f;

    // L1
    basis[1] = 0.488603f * dir.y;
    basis[2] = 0.488603f * dir.z;
    basis[3] = 0.488603f * dir.x;

    // L2
    basis[4] = 1.092548f * dir.x * dir.y;
    basis[5] = 1.092548f * dir.y * dir.z;
    basis[6] = 0.315392f * (3.0f * dir.z * dir.z - 1.0f);
    basis[7] = 1.092548f * dir.x * dir.z;
    basis[8] = 0.546274f * (dir.x * dir.x - dir.y * dir.y);
}

// Accumulate radiance to L2 SH coefficients
void AccumulateToSHL2(float3 dir, float3 radiance, inout float3 shCoeffs[9]) {
    float basis[9];
    EvaluateSHL2Basis(dir, basis);

    [unroll]
    for (int i = 0; i < 9; i++) {
        shCoeffs[i] += radiance * basis[i];
    }
}

// Alias for L2 accumulation (used by LightmapBake.hlsl)
#define AccumulateToSH AccumulateToSHL2

// ============================================
// Utility Functions
// ============================================

// Linear interpolation
float3 Lerp3(float3 a, float3 b, float t) {
    return a + t * (b - a);
}

// Clamp to [0, 1]
float Saturate(float x) {
    return clamp(x, 0.0f, 1.0f);
}

// Luminance calculation (Rec. 709)
float Luminance(float3 color) {
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Safe normalize (returns zero vector if input is zero)
float3 SafeNormalize(float3 v) {
    float len = length(v);
    return len > 0.0001f ? v / len : float3(0, 0, 0);
}

// Offset ray origin to avoid self-intersection
float3 OffsetRayOrigin(float3 position, float3 normal) {
    // Offset along normal to avoid self-intersection
    return position + normal * 0.001f;
}

// Compute reflection direction
float3 Reflect(float3 incident, float3 normal) {
    return incident - 2.0f * dot(incident, normal) * normal;
}

#endif // LIGHTMAP_BAKE_COMMON_HLSL
