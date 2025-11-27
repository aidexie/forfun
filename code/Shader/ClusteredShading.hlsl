// Clustered Shading - Pixel Shader Integration
// Provides functions to query cluster data and apply point lights

#ifndef CLUSTERED_SHADING_HLSL
#define CLUSTERED_SHADING_HLSL

#include "Common.hlsl"  // Shared BRDF functions and point light calculation

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

// ============================================
// Point Light Calculation (Wrapper)
// ============================================
// Converts GpuPointLight to PointLightInput and uses shared BRDF from Common.hlsl
float3 CalculatePointLight(
    GpuPointLight light,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Convert to Common.hlsl format
    PointLightInput lightInput;
    lightInput.position = light.position;
    lightInput.range = light.range;
    lightInput.color = light.color;
    lightInput.intensity = light.intensity;

    // Use shared PBR calculation from Common.hlsl
    return CalculatePointLightPBR(lightInput, worldPos, N, V, albedo, metallic, roughness);
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

#endif // CLUSTERED_SHADING_HLSL
