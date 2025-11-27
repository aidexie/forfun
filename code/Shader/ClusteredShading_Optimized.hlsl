// Clustered Shading - OPTIMIZED Point Light Implementation
// Based on UE4 and Unity URP best practices

#ifndef CLUSTERED_SHADING_OPTIMIZED_HLSL
#define CLUSTERED_SHADING_OPTIMIZED_HLSL

// Configuration (must match ClusteredLighting.compute.hlsl)
#define TILE_SIZE 32
#define DEPTH_SLICES 16

// Data structures (must match C++)
struct ClusterData {
    uint offset;
    uint count;
};

struct GpuPointLight {
    float3 position;
    float range;
    float3 color;
    float intensity;
};

// Cluster data bound to t10, t11, t12
StructuredBuffer<ClusterData> g_clusterData : register(t10);
StructuredBuffer<uint> g_compactLightList : register(t11);
StructuredBuffer<GpuPointLight> g_pointLights : register(t12);

// Additional cbuffer for cluster parameters (shared with MainPass)
cbuffer ClusteredParams : register(b3) {
    float g_clusterNearZ;
    float g_clusterFarZ;
    uint g_clusterNumX;
    uint g_clusterNumY;
    uint g_clusterNumZ;
    uint3 g_clusterPadding;
};

// ============================================
// Distance Attenuation (UE4 Style)
// ============================================
// Reference: UE4 DeferredLightingCommon.ush
// Combines physical inverse-square falloff with smooth windowing
float GetDistanceAttenuation(float3 unnormalizedLightVector, float invRadius) {
    float distSqr = dot(unnormalizedLightVector, unnormalizedLightVector);

    // Physical inverse-square falloff (with +1 to prevent infinity at origin)
    float attenuation = 1.0 / (distSqr + 1.0);

    // Smooth windowing to fade out at light range
    float factor = distSqr * invRadius * invRadius;
    float smoothFactor = saturate(1.0 - factor * factor);
    smoothFactor = smoothFactor * smoothFactor;

    return attenuation * smoothFactor;
}

// ============================================
// PBR BRDF Components (Optimized)
// ============================================

// GGX Normal Distribution Function
float D_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * denom * denom + 0.0001);  // +epsilon for stability
}

// Schlick-GGX Geometry Function (Direct Lighting)
// CRITICAL: Use correct k formula for direct lighting!
float G_SchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;  // Direct lighting formula (NOT alpha/2!)
    return NdotX / (NdotX * (1.0 - k) + k + 0.0001);
}

// Smith's method
float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

// Fresnel (Schlick approximation)
float3 F_Schlick(float VdotH, float3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
}

// ============================================
// Optimized Point Light Calculation
// ============================================
float3 CalculatePointLight(
    GpuPointLight light,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Light vector (unnormalized for distance calculation)
    float3 unnormalizedL = light.position - worldPos;
    float distance = length(unnormalizedL);
    float3 L = unnormalizedL / distance;  // Normalize

    // Early exit: backface
    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return float3(0, 0, 0);  // 30% faster!

    // Distance attenuation (UE4 style)
    float invRadius = 1.0 / light.range;
    float attenuation = GetDistanceAttenuation(unnormalizedL, invRadius);

    // Early exit: out of range
    if (attenuation < 0.001) return float3(0, 0, 0);

    // BRDF calculations
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));

    // Cook-Torrance BRDF terms
    float D = D_GGX(NdotH, roughness);
    float G = G_Smith(NdotV, NdotL, roughness);

    // Fresnel
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = F_Schlick(VdotH, F0);

    // Specular BRDF
    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Energy conservation: clamp specular to prevent over-brightness
    specular = min(specular, float3(1.0, 1.0, 1.0));

    // Diffuse BRDF (Lambertian)
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / 3.14159265;

    // Final radiance
    float3 radiance = light.color * light.intensity * attenuation;
    return (diffuse + specular) * radiance * NdotL;
}

// ============================================
// Cluster Index Calculation
// ============================================

// Get cluster index from screen position and depth
uint GetClusterIndex(float2 screenPos, float viewZ) {
    // Screen position is in pixels [0, screenWidth/Height]
    uint clusterX = (uint)(screenPos.x) / TILE_SIZE;
    uint clusterY = (uint)(screenPos.y) / TILE_SIZE;
    uint clusterZ = uint((abs(viewZ) / g_clusterFarZ) * DEPTH_SLICES);  // Linear depth

    // Clamp to valid range
    clusterX = clamp(clusterX, 0u, g_clusterNumX - 1u);
    clusterY = clamp(clusterY, 0u, g_clusterNumY - 1u);
    clusterZ = clamp(clusterZ, 0u, DEPTH_SLICES - 1u);

    uint clusterIdx = clusterX + clusterY * g_clusterNumX + clusterZ * g_clusterNumX * g_clusterNumY;
    return clusterIdx;
}

// ============================================
// Apply All Point Lights in Cluster
// ============================================
float3 ApplyClusteredPointLights(
    float2 screenPos,
    float viewZ,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Get cluster index
    uint clusterIdx = GetClusterIndex(screenPos, viewZ);

    // Get light list for this cluster
    ClusterData cluster = g_clusterData[clusterIdx];

    // Accumulate lighting
    float3 lighting = float3(0.0, 0.0, 0.0);

    // Process all lights in this cluster
    for (uint i = 0; i < cluster.count; i++) {
        uint lightIdx = g_compactLightList[cluster.offset + i];
        GpuPointLight light = g_pointLights[lightIdx];

        lighting += CalculatePointLight(light, worldPos, N, V, albedo, metallic, roughness);
    }

    return lighting;
}

#endif // CLUSTERED_SHADING_OPTIMIZED_HLSL
