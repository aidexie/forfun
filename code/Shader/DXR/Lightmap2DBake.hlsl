// ============================================
// DXR 2D Lightmap Baking Shader
// ============================================
// GPU-accelerated path tracing for 2D lightmap baking.
// Uses hemisphere sampling above surface normal (not cubemap).
//
// Dispatch Mode:
// - Dispatch (batchSize, samplesPerTexel, 1)
// - Each thread traces one ray for one sample of one texel
//
// Algorithm:
// 1. Load texel data (world pos, normal) from linearized buffer
// 2. Generate cosine-weighted hemisphere ray
// 3. Trace path with multiple bounces
// 4. Atomic add to accumulation buffer
//
// Key differences from Volumetric Lightmap:
// - Hemisphere sampling (not full sphere/cubemap)
// - Direct RGB output (not SH coefficients)
// - Batched texel processing (not brick-based)

#include "LightmapBakeCommon.hlsl"

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
    uint vertexBufferOffset;
    uint indexBufferOffset;
    uint padding;
};

struct SVertexPosition {
    float3 position;
    float padding;
};

struct STexelData {
    float3 worldPos;
    float validity;
    float3 normal;
    float padding;
    uint atlasX;
    uint atlasY;
    uint2 padding2;
};

struct SRayPayload {
    float3 radiance;
    float3 throughput;
    float3 nextOrigin;
    float3 nextDirection;
    uint rngState;
    uint bounceCount;
    bool terminated;
};

struct SShadowPayload {
    bool isVisible;
};

// ============================================
// Constant Buffer
// ============================================

cbuffer CB_Lightmap2DBakeParams : register(b0) {
    uint g_TotalTexels;
    uint g_SamplesPerTexel;
    uint g_MaxBounces;
    float g_SkyIntensity;

    uint g_AtlasWidth;
    uint g_AtlasHeight;
    uint g_BatchOffset;
    uint g_BatchSize;

    uint g_FrameIndex;
    uint g_NumLights;
    uint2 g_Padding;
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
StructuredBuffer<SVertexPosition> g_Vertices : register(t5);
StructuredBuffer<uint> g_Indices : register(t6);
StructuredBuffer<STexelData> g_TexelData : register(t7);

// Output: Accumulation buffer (float4 per atlas texel)
// xyz = accumulated radiance, w = sample count
RWStructuredBuffer<float4> g_Accumulation : register(u0);

// ============================================
// Geometric Normal Computation
// ============================================

float3 ComputeGeometricNormal(uint instanceID, uint primitiveID) {
    SInstanceData inst = g_Instances[instanceID];

    uint indexOffset = (inst.indexBufferOffset + primitiveID) * 3;

    uint i0 = g_Indices[indexOffset + 0];
    uint i1 = g_Indices[indexOffset + 1];
    uint i2 = g_Indices[indexOffset + 2];

    uint vertexOffset = inst.vertexBufferOffset;

    float3 v0 = g_Vertices[vertexOffset + i0].position;
    float3 v1 = g_Vertices[vertexOffset + i1].position;
    float3 v2 = g_Vertices[vertexOffset + i2].position;

    float3 edge1 = v1 - v0;
    float3 edge2 = v2 - v0;

    return normalize(cross(edge1, edge2));
}

float3 LocalToWorldNormal(float3 localNormal) {
    float3x4 objectToWorld = ObjectToWorld3x4();

    float3x3 normalMatrix = float3x3(
        objectToWorld[0].xyz,
        objectToWorld[1].xyz,
        objectToWorld[2].xyz
    );

    return normalize(mul(localNormal, normalMatrix));
}

// ============================================
// Lighting Evaluation
// ============================================

float3 EvaluateDirectLighting(float3 hitPos, float3 normal, SMaterialData mat, inout uint rng) {
    float3 result = float3(0, 0, 0);
    if (g_NumLights == 0) return result;

    // Process first light (assumed directional for now)
    SLightData light = g_Lights[0];

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
        shadowPayload.isVisible = false;

        TraceRay(
            g_Scene,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            0xFF,
            1, 0, 1,  // Shadow hit group index, miss index
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
// Ray Generation Shader (2D Lightmap)
// ============================================

[shader("raygeneration")]
void RayGen() {
    // Thread indices: x = texel index in batch, y = sample index
    uint3 idx = DispatchRaysIndex().xyz;
    uint texelIdxInBatch = idx.x;
    uint sampleIdx = idx.y;

    // Calculate global texel index
    uint globalTexelIdx = g_BatchOffset + texelIdxInBatch;

    // Bounds check
    if (globalTexelIdx >= g_TotalTexels || texelIdxInBatch >= g_BatchSize) {
        return;
    }

    // Load texel data
    STexelData texel = g_TexelData[globalTexelIdx];

    // Skip invalid texels
    if (texel.validity <= 0.5f) {
        return;
    }

    float3 worldPos = texel.worldPos;
    float3 normal = normalize(texel.normal);

    // Calculate atlas index for accumulation
    uint atlasIdx = texel.atlasY * g_AtlasWidth + texel.atlasX;

    // Initialize RNG with unique seed
    uint rngState = InitRNGWithSample(
        uint3(texel.atlasX, texel.atlasY, g_FrameIndex),
        g_FrameIndex,
        sampleIdx
    );

    // Generate cosine-weighted hemisphere direction
    float3 rayDir = SampleHemisphereCosineWorld(normal, rngState);

    // Initialize payload
    SRayPayload payload;
    payload.radiance = float3(0, 0, 0);
    payload.throughput = float3(1, 1, 1);
    payload.nextOrigin = OffsetRayOrigin(worldPos, normal);
    payload.nextDirection = rayDir;
    payload.rngState = rngState;
    payload.bounceCount = 0;
    payload.terminated = false;

    // Path tracing loop
    for (uint bounce = 0; bounce <= g_MaxBounces; bounce++) {
        if (payload.terminated) {
            break;
        }

        RayDesc ray;
        ray.Origin = payload.nextOrigin;
        ray.Direction = payload.nextDirection;
        ray.TMin = 0.001f;
        ray.TMax = 10000.0f;

        TraceRay(
            g_Scene,
            RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            0xFF,
            0, 0, 0,  // Primary hit group index
            ray,
            payload
        );
    }

    // Cosine-weighted sampling already accounts for PDF
    // For Lambertian: PDF = cos(theta) / PI, so weight = 1
    float3 contribution = payload.radiance;

    // Simple accumulation (race condition possible with parallel samples)
    // TODO: Implement proper atomic float accumulation
    float4 current = g_Accumulation[atlasIdx];
    g_Accumulation[atlasIdx] = float4(
        current.xyz + contribution,
        current.w + 1.0f
    );
}

// ============================================
// Closest Hit Shader
// ============================================

[shader("closesthit")]
void ClosestHit(inout SRayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = InstanceID();
    uint primitiveID = PrimitiveIndex();

    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    float3 localNormal = ComputeGeometricNormal(instanceID, primitiveID);
    float3 worldNormal = LocalToWorldNormal(localNormal);

    // Ensure normal faces the ray
    if (dot(worldNormal, WorldRayDirection()) > 0) {
        worldNormal = -worldNormal;
    }

    // Get material
    uint materialIndex = g_Instances[instanceID].materialIndex;
    SMaterialData mat = g_Materials[materialIndex];

    // Evaluate direct lighting
    float3 directLight = EvaluateDirectLighting(hitPos, worldNormal, mat, payload.rngState);
    payload.radiance += payload.throughput * directLight;

    payload.bounceCount++;

    // Russian Roulette after 2 bounces
    if (payload.bounceCount >= 2) {
        float maxComponent = max(mat.albedo.r, max(mat.albedo.g, mat.albedo.b));
        float survivalProb = min(maxComponent, 0.95f);

        if (Random(payload.rngState) > survivalProb) {
            payload.terminated = true;
            return;
        }

        payload.throughput /= survivalProb;
    }

    // Check max bounces
    if (payload.bounceCount >= g_MaxBounces) {
        payload.terminated = true;
        return;
    }

    // Sample next bounce direction
    float3 bounceDir = SampleHemisphereCosineWorld(worldNormal, payload.rngState);

    payload.nextOrigin = OffsetRayOrigin(hitPos, worldNormal);
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
