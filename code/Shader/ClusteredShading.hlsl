// Clustered Shading - Pixel Shader Integration
// Provides functions to query cluster data and apply point lights

#ifndef CLUSTERED_SHADING_HLSL
#define CLUSTERED_SHADING_HLSL

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

// Get cluster Z slice from view-space depth
uint GetSliceFromDepth(float viewZ) {
    // Logarithmic depth slicing
    // slice = log(viewZ / nearZ) / log(farZ / nearZ) * DEPTH_SLICES
    float t = log(viewZ / g_clusterNearZ) / log(g_clusterFarZ / g_clusterNearZ);
    uint slice = (uint)(t * (float)DEPTH_SLICES);
    return clamp(slice, 0u, DEPTH_SLICES - 1u);
}

// Get cluster index from screen position and depth
uint GetClusterIndex(float2 screenPos, float viewZ) {
    // Screen position is in pixels [0, screenWidth/Height]
    uint clusterX = (uint)(screenPos.x) / TILE_SIZE;
    uint clusterY = (uint)(screenPos.y) / TILE_SIZE;
    uint clusterZ = GetSliceFromDepth(abs(viewZ));  // abs because viewZ is negative

    // Clamp to valid range
    clusterX = clamp(clusterX, 0u, g_clusterNumX - 1u);
    clusterY = clamp(clusterY, 0u, g_clusterNumY - 1u);

    uint clusterIdx = clusterX + clusterY * g_clusterNumX + clusterZ * g_clusterNumX * g_clusterNumY;
    return clusterIdx;
}

// PBR Point Light BRDF (Cook-Torrance)
// Matches directional light implementation for consistency
float3 CalculatePointLight(
    GpuPointLight light,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Light direction and distance
    float3 L = light.position - worldPos;
    float distance = length(L);
    L = normalize(L);

    // Range attenuation (smooth falloff)
    float attenuation = saturate(1.0 - (distance / light.range));
    attenuation = attenuation * attenuation;  // Smoother falloff

    // Physical inverse-square falloff
    float distanceAttenuation = 1.0 / max(distance * distance, 0.01);

    // Combined attenuation
    float finalAttenuation = attenuation * distanceAttenuation;

    // Cook-Torrance BRDF
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Roughness remapping (like MainPass)
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;

    // GGX Normal Distribution
    float denom = NdotH * NdotH * (alphaSquared - 1.0) + 1.0;
    float D = alphaSquared / (3.14159265 * denom * denom);

    // Schlick-GGX Geometry
    float k = alpha / 2.0;
    float G1_V = NdotV / (NdotV * (1.0 - k) + k);
    float G1_L = NdotL / (NdotL * (1.0 - k) + k);
    float G = G1_V * G1_L;

    // Fresnel (Schlick approximation)
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    // Specular BRDF
    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Diffuse BRDF (Lambertian)
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / 3.14159265;

    // Final radiance
    float3 radiance = light.color * light.intensity * finalAttenuation;
    return (diffuse + specular) * radiance * NdotL;
}

// Apply all point lights in the current cluster
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

    for (uint i = 0; i < cluster.count; i++) {
        uint lightIdx = g_compactLightList[cluster.offset + i];
        GpuPointLight light = g_pointLights[lightIdx];

        lighting += CalculatePointLight(light, worldPos, N, V, albedo, metallic, roughness);
    }

    return lighting;
}

#endif // CLUSTERED_SHADING_HLSL
