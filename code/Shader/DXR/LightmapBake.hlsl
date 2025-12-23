// ============================================
// DXR Lightmap Baking Shader (Per-Brick Dispatch)
// ============================================
// GPU-accelerated path tracing for volumetric lightmap baking.
// Dispatches 4x4x4 = 64 threads per brick, matching the CPU baker.
// Output is written to a structured buffer for easy CPU readback.
//
// IMPORTANT: DXR shaders require [unroll] for loops with TraceRay.
// Dynamic while loops cause GPU crashes due to divergent control flow.

#include "LightmapBakeCommon.hlsl"

// ============================================
// Constants
// ============================================

#define BRICK_SIZE 4
#define VOXELS_PER_BRICK 64  // 4^3
#define SH_COEFF_COUNT 9     // L2 spherical harmonics

// Fixed max iterations for shader compilation (DXR requirement)
static const uint MAX_SAMPLES = 16;
static const uint MAX_BOUNCES = 4;

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

// Output structure for one voxel (matches CPU SBrick layout)
struct SVoxelSHOutput {
    float3 sh[SH_COEFF_COUNT];  // L2 SH coefficients (RGB each)
    float validity;
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

cbuffer CB_BakeParams : register(b0) {
    float3 g_BrickWorldMin;
    float g_Padding0;
    float3 g_BrickWorldMax;
    float g_Padding1;

    uint g_SamplesPerVoxel;
    uint g_MaxBounces;
    uint g_FrameIndex;
    uint g_NumLights;

    float g_SkyIntensity;
    uint g_BrickIndex;
    uint g_TotalBricks;
    float g_Padding2;
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
RWStructuredBuffer<SVoxelSHOutput> g_OutputBuffer : register(u0);

// ============================================
// Helper Functions
// ============================================

float3 LocalVoxelToWorldPos(uint3 localIdx) {
    float3 brickSize = g_BrickWorldMax - g_BrickWorldMin;
    float3 voxelSize = brickSize / float(BRICK_SIZE);
    float3 localPos = (float3(localIdx) + 0.5f) * voxelSize;
    return g_BrickWorldMin + localPos;
}

uint LocalVoxelToIndex(uint3 localIdx) {
    return localIdx.x + localIdx.y * BRICK_SIZE + localIdx.z * BRICK_SIZE * BRICK_SIZE;
}

// ============================================
// Lighting Evaluation
// ============================================

float3 EvaluateDirectLighting(float3 hitPos, float3 normal, SMaterialData mat, inout uint rng) {
    float3 result = float3(0, 0, 0);

    SLightData light = g_Lights[0];

    float3 L;
    float lightDist;
    float attenuation;

    // Directional light
    L = -normalize(light.direction);
    lightDist = 10000.0f;
    attenuation = 1.0f;

    float NdotL = saturate(dot(normal, L));

    if (NdotL > 0.0f && attenuation > 0.0f) {
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
            result += light.color * light.intensity * attenuation * NdotL * brdf;
        }
    }

    return result;
}

// ============================================
// Ray Generation Shader (Per-Brick)
// ============================================

[shader("raygeneration")]
void RayGen() {
    uint3 localVoxelIdx = DispatchRaysIndex().xyz;

    if (any(localVoxelIdx >= uint3(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE))) {
        return;
    }

    uint outputIndex = LocalVoxelToIndex(localVoxelIdx);
    float3 worldPos = LocalVoxelToWorldPos(localVoxelIdx);

    // Initialize output
    SVoxelSHOutput output;
    [unroll]
    for (int i = 0; i < SH_COEFF_COUNT; i++) {
        output.sh[i] = float3(0, 0, 0);
    }
    output.validity = 1.0f;
    output.padding = float3(0, 0, 0);

    // Initialize RNG
    uint rngState = InitRNGWithSample(localVoxelIdx, g_FrameIndex, g_BrickIndex);

    // ============================================
    // Validity Check
    // ============================================
    float validitySum = 0.0f;
    float3 validityDirs[6] = {
        float3(1, 0, 0), float3(-1, 0, 0),
        float3(0, 1, 0), float3(0, -1, 0),
        float3(0, 0, 1), float3(0, 0, -1)
    };

    for (uint v = 0; v < 6; v++) {
        RayDesc validityRay;
        validityRay.Origin = worldPos;
        validityRay.Direction = validityDirs[v];
        validityRay.TMin = 0.0f;
        validityRay.TMax = 0.1f;

        SShadowPayload validityPayload;
        validityPayload.isVisible = true;

        TraceRay(
            g_Scene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF, 1, 0, 1,
            validityRay,
            validityPayload
        );

        validitySum += validityPayload.isVisible ? 1.0f : 0.0f;
    }

    float validity = validitySum / 6.0f;
    output.validity = validity;

    if (validity < 0.5f) {
        g_OutputBuffer[outputIndex] = output;
        return;
    }

    // ============================================
    // Path Tracing
    // ============================================
    float3 shAccum[SH_COEFF_COUNT];
    [unroll]
    for (int k = 0; k < SH_COEFF_COUNT; k++) {
        shAccum[k] = float3(0, 0, 0);
    }

    uint actualSamples = min(g_SamplesPerVoxel, MAX_SAMPLES);

    [unroll]
    for (uint s = 0; s < MAX_SAMPLES; s++) {
        if (s >= actualSamples) break;

        float3 dir = SampleSphereUniform(rngState);

        SRayPayload payload;
        payload.radiance = float3(0, 0, 0);
        payload.throughput = float3(1, 1, 1);
        payload.nextOrigin = worldPos;
        payload.nextDirection = dir;
        payload.rngState = rngState;
        payload.bounceCount = 0;
        payload.terminated = false;

        [unroll]
        for (uint bounce = 0; bounce < MAX_BOUNCES; bounce++) {
            if (payload.terminated || payload.bounceCount >= g_MaxBounces) break;

            RayDesc ray;
            ray.Origin = payload.nextOrigin;
            ray.Direction = payload.nextDirection;
            ray.TMin = 0.001f;
            ray.TMax = 10000.0f;

            TraceRay(
                g_Scene,
                RAY_FLAG_NONE,
                0xFF,
                0, 0, 0,
                ray,
                payload
            );
        }

        AccumulateToSH(dir, payload.radiance, shAccum);
        rngState = payload.rngState;
    }

    // Normalize SH coefficients
    float weight = 4.0f * PI / float(actualSamples);
    [unroll]
    for (int j = 0; j < SH_COEFF_COUNT; j++) {
        output.sh[j] = shAccum[j] * weight;
    }

    g_OutputBuffer[outputIndex] = output;
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
