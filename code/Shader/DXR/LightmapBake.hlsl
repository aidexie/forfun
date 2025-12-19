// ============================================
// DXR Lightmap Baking Shader
// ============================================
// GPU-accelerated path tracing for volumetric lightmap baking.
// Uses DXR to trace rays through the scene and accumulate
// spherical harmonics coefficients for indirect lighting.

#include "LightmapBakeCommon.hlsl"

// ============================================
// Resource Bindings
// ============================================

// Acceleration structure
RaytracingAccelerationStructure g_Scene : register(t0);

// Environment map (skybox)
TextureCube<float4> g_Skybox : register(t1);
SamplerState g_LinearSampler : register(s0);

// Scene data buffers
StructuredBuffer<SMaterialData> g_Materials : register(t2);
StructuredBuffer<SLightData> g_Lights : register(t3);
StructuredBuffer<SInstanceData> g_Instances : register(t4);

// Output: SH coefficients (L1 = 4 coefficients, packed into 3 textures)
// SH0: (L0.rgb, L1_y.r)
// SH1: (L1_y.gb, L1_z.rg)
// SH2: (L1_z.b, L1_x.rgb)
RWTexture3D<float4> g_OutputSH0 : register(u0);
RWTexture3D<float4> g_OutputSH1 : register(u1);
RWTexture3D<float4> g_OutputSH2 : register(u2);

// Validity mask output (1 = valid, 0 = inside geometry)
RWTexture3D<float> g_OutputValidity : register(u3);

// ============================================
// Constant Buffers
// ============================================

cbuffer CB_BakeParams : register(b0) {
    float3 g_VolumeMin;
    float g_Padding0;
    float3 g_VolumeMax;
    float g_Padding1;
    uint3 g_VoxelGridSize;
    uint g_SamplesPerVoxel;
    uint g_MaxBounces;
    uint g_FrameIndex;
    uint g_NumLights;
    float g_SkyIntensity;
};

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
    float3 position;
    float3 direction;
    float3 color;
    float intensity;
    float range;
    float spotAngle;
    float padding;
};

struct SInstanceData {
    uint materialIndex;
    float3 padding;
};

// Ray payload for path tracing
struct SRayPayload {
    float3 radiance;        // Accumulated radiance
    float3 throughput;      // Path throughput (for Russian Roulette)
    float3 nextOrigin;      // Next ray origin
    float3 nextDirection;   // Next ray direction
    uint rngState;          // RNG state
    uint bounceCount;       // Current bounce count
    bool terminated;        // Path terminated flag
};

// Shadow ray payload (simplified)
struct SShadowPayload {
    bool isVisible;         // True if no hit (light is visible)
};

// ============================================
// Helper Functions
// ============================================

// Convert voxel index to world position (voxel center)
float3 VoxelIndexToWorldPos(uint3 voxelIdx) {
    float3 normalizedPos = (float3(voxelIdx) + 0.5f) / float3(g_VoxelGridSize);
    return g_VolumeMin + normalizedPos * (g_VolumeMax - g_VolumeMin);
}

// Check if a point is inside the volume
bool IsInsideVolume(float3 worldPos) {
    return all(worldPos >= g_VolumeMin) && all(worldPos <= g_VolumeMax);
}

// ============================================
// Lighting Evaluation
// ============================================

float3 EvaluateDirectLighting(float3 hitPos, float3 normal, SMaterialData mat, inout uint rng) {
    float3 result = float3(0, 0, 0);

    for (uint i = 0; i < g_NumLights; i++) {
        SLightData light = g_Lights[i];

        float3 L;           // Direction to light
        float lightDist;    // Distance to light
        float attenuation;  // Light attenuation

        if (light.type == 0) {
            // Directional light
            L = -normalize(light.direction);
            lightDist = 10000.0f;
            attenuation = 1.0f;
        }
        else if (light.type == 1) {
            // Point light
            float3 toLight = light.position - hitPos;
            lightDist = length(toLight);
            L = toLight / lightDist;

            // Distance attenuation
            float distRatio = lightDist / light.range;
            attenuation = saturate(1.0f - distRatio * distRatio);
            attenuation *= attenuation;
        }
        else {
            // Spot light
            float3 toLight = light.position - hitPos;
            lightDist = length(toLight);
            L = toLight / lightDist;

            // Distance attenuation
            float distRatio = lightDist / light.range;
            attenuation = saturate(1.0f - distRatio * distRatio);
            attenuation *= attenuation;

            // Cone attenuation
            float cosAngle = dot(-L, normalize(light.direction));
            float cosOuter = cos(radians(light.spotAngle));
            float cosInner = cos(radians(light.spotAngle * 0.8f));
            attenuation *= saturate((cosAngle - cosOuter) / (cosInner - cosOuter));
        }

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
                1,      // Shadow hit group
                0,
                1,      // Shadow miss shader
                shadowRay,
                shadowPayload
            );

            if (shadowPayload.isVisible) {
                // Lambertian BRDF: albedo / PI
                float3 brdf = mat.albedo * INV_PI;
                result += light.color * light.intensity * attenuation * NdotL * brdf;
            }
        }
    }

    return result;
}

// ============================================
// Ray Generation Shader
// ============================================

[shader("raygeneration")]
void RayGen() {
    // Get voxel index from dispatch ID
    uint3 voxelIdx = DispatchRaysIndex().xyz;

    // Bounds check
    if (any(voxelIdx >= g_VoxelGridSize)) {
        return;
    }

    // Compute world position (voxel center)
    float3 worldPos = VoxelIndexToWorldPos(voxelIdx);

    // Initialize SH accumulators (L1 = 4 coefficients)
    float3 shCoeffs[4];
    [unroll]
    for (int i = 0; i < 4; i++) {
        shCoeffs[i] = float3(0, 0, 0);
    }

    // Initialize RNG
    uint rngState = InitRNGWithSample(voxelIdx, g_FrameIndex, 0);

    // Validity check: cast short rays to detect if voxel is inside geometry
    float validitySum = 0.0f;
    const uint validityRays = 6;

    float3 validityDirs[6] = {
        float3(1, 0, 0), float3(-1, 0, 0),
        float3(0, 1, 0), float3(0, -1, 0),
        float3(0, 0, 1), float3(0, 0, -1)
    };

    for (uint v = 0; v < validityRays; v++) {
        RayDesc validityRay;
        validityRay.Origin = worldPos;
        validityRay.Direction = validityDirs[v];
        validityRay.TMin = 0.0f;
        validityRay.TMax = 0.1f;  // Short distance check

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

    float validity = validitySum / float(validityRays);

    // If mostly inside geometry, skip sampling
    if (validity < 0.5f) {
        g_OutputSH0[voxelIdx] = float4(0, 0, 0, 0);
        g_OutputSH1[voxelIdx] = float4(0, 0, 0, 0);
        g_OutputSH2[voxelIdx] = float4(0, 0, 0, 0);
        g_OutputValidity[voxelIdx] = 0.0f;
        return;
    }

    // Sample hemisphere for GI
    for (uint s = 0; s < g_SamplesPerVoxel; s++) {
        // Generate uniform sphere direction (full sphere for volumetric)
        float3 dir = SampleSphereUniform(rngState);

        // Initialize payload
        SRayPayload payload;
        payload.radiance = float3(0, 0, 0);
        payload.throughput = float3(1, 1, 1);
        payload.nextOrigin = worldPos;
        payload.nextDirection = dir;
        payload.rngState = rngState;
        payload.bounceCount = 0;
        payload.terminated = false;

        // Path trace
        while (!payload.terminated && payload.bounceCount < g_MaxBounces) {
            RayDesc ray;
            ray.Origin = payload.nextOrigin;
            ray.Direction = payload.nextDirection;
            ray.TMin = 0.001f;
            ray.TMax = 10000.0f;

            TraceRay(
                g_Scene,
                RAY_FLAG_NONE,
                0xFF,
                0,      // Primary hit group
                0,
                0,      // Primary miss shader
                ray,
                payload
            );
        }

        // Accumulate to SH
        AccumulateToSHL1(dir, payload.radiance, shCoeffs);

        // Update RNG state
        rngState = payload.rngState;
    }

    // Normalize SH coefficients
    float weight = 4.0f * PI / float(g_SamplesPerVoxel);
    [unroll]
    for (int j = 0; j < 4; j++) {
        shCoeffs[j] *= weight;
    }

    // Pack and store output
    // SH0: (L0.rgb, L1_y.r)
    g_OutputSH0[voxelIdx] = float4(shCoeffs[0], shCoeffs[1].r);
    // SH1: (L1_y.gb, L1_z.rg)
    g_OutputSH1[voxelIdx] = float4(shCoeffs[1].gb, shCoeffs[2].rg);
    // SH2: (L1_z.b, L1_x.rgb)
    g_OutputSH2[voxelIdx] = float4(shCoeffs[2].b, shCoeffs[3]);

    // Store validity
    g_OutputValidity[voxelIdx] = validity;
}

// ============================================
// Closest Hit Shader (Primary Rays)
// ============================================

[shader("closesthit")]
void ClosestHit(inout SRayPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    // Get instance and primitive info
    uint instanceID = InstanceID();

    // Compute hit position
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // Compute barycentric coordinates
    float3 barycentrics = float3(
        1.0f - attr.barycentrics.x - attr.barycentrics.y,
        attr.barycentrics.x,
        attr.barycentrics.y
    );

    // Get geometric normal (assuming CCW winding)
    // In a full implementation, you would interpolate vertex normals here
    // For now, use geometric normal from ray direction
    float3 normal = -WorldRayDirection();  // Placeholder - should use actual geometry normal

    // Get material
    uint materialIndex = g_Instances[instanceID].materialIndex;
    SMaterialData mat = g_Materials[materialIndex];

    // Evaluate direct lighting
    float3 directLight = EvaluateDirectLighting(hitPos, normal, mat, payload.rngState);

    // Accumulate radiance
    payload.radiance += payload.throughput * directLight;

    // Prepare next bounce
    payload.bounceCount++;

    // Russian Roulette for path termination (after 2 bounces)
    if (payload.bounceCount >= 2) {
        float maxComponent = max(mat.albedo.r, max(mat.albedo.g, mat.albedo.b));
        float survivalProb = min(maxComponent, 0.95f);

        if (Random(payload.rngState) > survivalProb) {
            payload.terminated = true;
            return;
        }

        payload.throughput /= survivalProb;
    }

    // Sample next direction (cosine-weighted for diffuse)
    float3 bounceDir = SampleHemisphereCosineWorld(normal, payload.rngState);

    payload.nextOrigin = OffsetRayOrigin(hitPos, normal);
    payload.nextDirection = bounceDir;

    // Update throughput (albedo for Lambertian, PI cancels with cosine PDF)
    payload.throughput *= mat.albedo;
}

// ============================================
// Miss Shader (Primary Rays - Sky)
// ============================================

[shader("miss")]
void Miss(inout SRayPayload payload) {
    // Sample skybox
    float3 dir = WorldRayDirection();
    float3 skyColor = g_Skybox.SampleLevel(g_LinearSampler, dir, 0).rgb;

    // Accumulate sky contribution
    payload.radiance += payload.throughput * skyColor * g_SkyIntensity;
    payload.terminated = true;
}

// ============================================
// Shadow Miss Shader
// ============================================

[shader("miss")]
void ShadowMiss(inout SShadowPayload payload) {
    // No hit means light is visible
    payload.isVisible = true;
}

// ============================================
// Shadow Any Hit Shader (for alpha testing if needed)
// ============================================

[shader("anyhit")]
void ShadowAnyHit(inout SShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    // For opaque geometry, any hit means shadowed
    payload.isVisible = false;
    AcceptHitAndEndSearch();
}
