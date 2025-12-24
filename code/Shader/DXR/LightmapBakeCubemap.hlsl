// ============================================
// DXR Lightmap Baking Shader (Cubemap-Based)
// ============================================
// GPU-accelerated path tracing for volumetric lightmap baking.
// Uses cubemap-based ray dispatch: 32x32x6 = 6144 rays per voxel.
// This matches the CPU baker's sampling pattern for correctness.
//
// Algorithm:
// 1. Dispatch (32, 32, 6) threads per voxel
// 2. Each thread computes ray direction from cubemap UV
// 3. Trace ray and write radiance to cubemap UAV
// 4. CPU/Compute shader projects cubemap to SH
//
// Benefits:
// - Sample parity with CPU baker (6144 vs 16 samples)
// - Direct cubemap output for debugging
// - No [unroll] limitation (one ray per thread)
// - Better GPU utilization

#include "LightmapBakeCommon.hlsl"

// ============================================
// Constants
// ============================================

#define CUBEMAP_RES 128
#define CUBEMAP_FACES 6
#define PIXELS_PER_FACE (CUBEMAP_RES * CUBEMAP_RES)
#define TOTAL_RAYS (PIXELS_PER_FACE * CUBEMAP_FACES)  // 98304

// ============================================
// Data Structures
// ============================================

struct SMaterialData {
    float3 albedo;
    float metallic;
    float roughness;
    float3 padding;
};

struct SLightData {
    uint type;          // 0=Directional, 1=Point, 2=Spot
    float3 padding0;
    float3 position;
    float padding1;
    float3 direction;
    float padding2;
    float3 color;
    float intensity;
    float range;
    float spotAngle;
    float2 padding3;
};

struct SInstanceData {
    uint materialIndex;
    float3 padding;
};

// Ray payload for path tracing
struct SRayPayload {
    float3 radiance;
    float3 throughput;
    float3 nextOrigin;
    float3 nextDirection;
    uint rngState;
    uint bounceCount;
    bool terminated;
};

// Shadow ray payload
struct SShadowPayload {
    bool isVisible;
};

// ============================================
// Constant Buffer
// ============================================

cbuffer CB_CubemapBakeParams : register(b0) {
    float3 g_VoxelWorldPos;   // World position of current voxel
    float g_Padding0;

    uint g_MaxBounces;
    uint g_FrameIndex;        // For RNG seeding
    uint g_NumLights;
    float g_SkyIntensity;

    uint g_VoxelIndex;        // For debugging/RNG seeding
    uint3 g_Padding1;
};

// ============================================
// Resource Bindings
// ============================================

RaytracingAccelerationStructure g_Scene : register(t0);
TextureCube<float4> g_Skybox : register(t1);
SamplerState g_LinearSampler : register(s0);
StructuredBuffer<SMaterialData> g_Materials : register(t2);
StructuredBuffer<SLightData> g_Lights : register(t3);
StructuredBuffer<SInstanceData> g_Instances : register(t4);

// Output: Cubemap radiance (32x32x6 = 6144 float4 values)
// Layout: [face * 1024 + y * 32 + x]
RWStructuredBuffer<float4> g_CubemapOutput : register(u0);

// ============================================
// Cubemap Direction Calculation
// ============================================

// Convert cubemap face + UV to world direction
// Face order: +X, -X, +Y, -Y, +Z, -Z
float3 CubemapUVToDirection(uint face, float u, float v) {
    // Map UV from [0,1] to [-1,1]
    float s = u * 2.0f - 1.0f;
    float t = v * 2.0f - 1.0f;

    float3 dir;
    switch (face) {
        case 0: dir = float3( 1.0f,    -t,    -s); break;  // +X
        case 1: dir = float3(-1.0f,    -t,     s); break;  // -X
        case 2: dir = float3(    s,  1.0f,     t); break;  // +Y
        case 3: dir = float3(    s, -1.0f,    -t); break;  // -Y
        case 4: dir = float3(    s,    -t,  1.0f); break;  // +Z
        case 5: dir = float3(   -s,    -t, -1.0f); break;  // -Z
        default: dir = float3(0, 1, 0); break;
    }

    // Normalize and ensure valid direction (avoid NaN from zero-length vector)
    float len = length(dir);
    if (len < 0.001f) {
        dir = float3(0, 1, 0);  // Default up direction
    } else {
        dir = dir / len;
    }

    return dir;
}

// ============================================
// Lighting Evaluation
// ============================================

float3 EvaluateDirectLighting(float3 hitPos, float3 normal, SMaterialData mat, inout uint rng) {
    float3 result = float3(0, 0, 0);
    if (g_NumLights == 0) return result;

    SLightData light = g_Lights[0];

    // Directional light
    float3 L = -normalize(light.direction);
    float lightDist = 10000.0f;

    float NdotL = saturate(dot(normal, L));

    if (NdotL > 0.0f) {
        // Shadow ray
        RayDesc shadowRay;
        shadowRay.Origin = OffsetRayOrigin(hitPos, normal);
        shadowRay.Direction = L;
        shadowRay.TMin = 0.001f;
        shadowRay.TMax = lightDist - 0.001f;

        SShadowPayload shadowPayload;
        shadowPayload.isVisible = true;

        TraceRay(
            g_Scene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF,
            1,      // Shadow hit group index
            0,
            1,      // Shadow miss shader index
            shadowRay,
            shadowPayload
        );

        if (shadowPayload.isVisible) {
            float3 brdf = mat.albedo * INV_PI;
            result += light.color * light.intensity * NdotL * brdf;
        }
    }

    return result;
}

// ============================================
// Ray Generation Shader (Cubemap-Based)
// ============================================

[shader("raygeneration")]
void RayGen() {
    // Thread indices: x=[0,127], y=[0,127], z=[0,5] (face)
    uint3 idx = DispatchRaysIndex().xyz;
    uint x = idx.x;
    uint y = idx.y;
    uint face = idx.z;

    // Bounds check
    if (x >= CUBEMAP_RES || y >= CUBEMAP_RES || face >= CUBEMAP_FACES) {
        return;
    }

    // Compute output index
    uint outputIndex = face * PIXELS_PER_FACE + y * CUBEMAP_RES + x;

    // Compute UV (pixel center)
    float u = (float(x) + 0.5f) / float(CUBEMAP_RES);
    float v = (float(y) + 0.5f) / float(CUBEMAP_RES);

    // Compute ray direction from cubemap UV
    float3 dir = CubemapUVToDirection(face, u, v);

    // Initialize RNG with unique seed per ray
    uint rngState = InitRNG(uint3(x, y, face), g_FrameIndex ^ g_VoxelIndex);

    // Initialize ray payload
    SRayPayload payload;
    payload.radiance = float3(0, 0, 0);
    payload.throughput = float3(1, 1, 1);
    payload.nextOrigin = g_VoxelWorldPos;
    payload.nextDirection = dir;
    payload.rngState = rngState;
    payload.bounceCount = 0;
    payload.terminated = false;

    // Path tracing loop
    for (uint bounce = 0; bounce < 1; bounce++) {
        if (payload.terminated || payload.bounceCount >= g_MaxBounces) {
            break;
        }

        RayDesc ray;
        ray.Origin = payload.nextOrigin;
        ray.Direction = payload.nextDirection;
        ray.TMin = 0.01f;
        ray.TMax = 10000.0f;

        TraceRay(
            g_Scene,
            RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            0xFF,
            0, 0, 0,
            ray,
            payload
        );
    }

    // Write radiance to cubemap output
    g_CubemapOutput[outputIndex] = float4(payload.radiance, 1.0f);
}

// ============================================
// Closest Hit Shader
// ============================================

[shader("closesthit")]
void ClosestHit(inout SRayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = InstanceID();

    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 normal = -normalize(WorldRayDirection());

    // Get material from buffer
    uint materialIndex = g_Instances[instanceID].materialIndex;
    SMaterialData mat = g_Materials[materialIndex];

    // Evaluate direct lighting
    float3 directLight = EvaluateDirectLighting(hitPos, normal, mat, payload.rngState);

    payload.radiance += payload.throughput * directLight;
    payload.bounceCount++;

    // Russian Roulette (after 2 bounces)
    if (payload.bounceCount >= 2) {
        float maxComponent = max(mat.albedo.r, max(mat.albedo.g, mat.albedo.b));
        float survivalProb = min(maxComponent, 0.95f);

        if (Random(payload.rngState) > survivalProb) {
            payload.terminated = true;
            return;
        }

        payload.throughput /= survivalProb;
    }

    // Sample next bounce direction
    float3 bounceDir = SampleHemisphereCosineWorld(normal, payload.rngState);

    payload.nextOrigin = OffsetRayOrigin(hitPos, normal);
    payload.nextDirection = bounceDir;
    payload.throughput *= mat.albedo;
}

// ============================================
// Miss Shader (Sky)
// ============================================

[shader("miss")]
void Miss(inout SRayPayload payload) {
    float3 dir = WorldRayDirection();
    float3 skyColor = g_Skybox.SampleLevel(g_LinearSampler, dir, 0).rgb;

    payload.radiance += payload.throughput * skyColor * g_SkyIntensity;
    payload.terminated = true;
}

// ============================================
// Shadow Miss Shader
// ============================================

[shader("miss")]
void ShadowMiss(inout SShadowPayload payload) {
    payload.isVisible = true;
}

// ============================================
// Shadow Any Hit Shader
// ============================================

[shader("anyhit")]
void ShadowAnyHit(inout SShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    payload.isVisible = false;
    AcceptHitAndEndSearch();
}
