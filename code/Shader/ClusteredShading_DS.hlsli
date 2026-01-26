// ClusteredShading_DS.hlsli
// Descriptor Set version - uses PerFrameSlots.h slot assignments
// CB: b1 (space0), SRVs: t4-t6 (space0)

#ifndef CLUSTERED_SHADING_DS_HLSLI
#define CLUSTERED_SHADING_DS_HLSLI

#include "Common.hlsl"  // Shared BRDF functions and point light calculation

// Configuration (must match ClusteredLighting.compute.hlsl)
#define TILE_SIZE 32
#define DEPTH_SLICES 16

// Data structures (must match C++)
struct ClusterData {
    uint offset;
    uint count;
};

// Light types (must match C++ ELightType)
#define LIGHT_TYPE_POINT 0
#define LIGHT_TYPE_SPOT  1

// Unified GPU light structure (supports both Point and Spot lights)
struct GpuLight {
    float3 position;     // World space position (all types)
    float range;         // Maximum light radius (all types)
    float3 color;        // Linear RGB (all types)
    float intensity;     // Luminous intensity (all types)

    // Spot light specific (unused for point lights)
    float3 direction;    // World space direction (normalized)
    float innerConeAngle;// cos(innerAngle) - precomputed
    float outerConeAngle;// cos(outerAngle) - precomputed
    uint type;           // LIGHT_TYPE_POINT or LIGHT_TYPE_SPOT
    float2 padding;      // Align to 16 bytes
};

// Cluster data bound to t4, t5, t6 (space0) - matches PerFrameSlots.h
StructuredBuffer<ClusterData> g_clusterData : register(t4, space0);
StructuredBuffer<uint> g_compactLightList : register(t5, space0);
StructuredBuffer<GpuLight> g_lights : register(t6, space0);  // All lights (Point + Spot)

// Cluster parameters CB at b1 (space0) - matches PerFrameSlots::CB::Clustered
cbuffer ClusteredParams : register(b1, space0) {
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
// Converts GpuLight to PointLightInput and uses shared BRDF from Common.hlsl
float3 CalculatePointLight(
    GpuLight light,
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
// Spot Light Calculation (Wrapper)
// ============================================
// Converts GpuLight to SpotLightInput and uses shared BRDF from Common.hlsl
float3 CalculateSpotLight(
    GpuLight light,
    float3 worldPos,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness
) {
    // Convert to Common.hlsl format
    SpotLightInput lightInput;
    lightInput.position = light.position;
    lightInput.range = light.range;
    lightInput.color = light.color;
    lightInput.intensity = light.intensity;
    lightInput.direction = light.direction;
    lightInput.innerConeAngle = light.innerConeAngle;
    lightInput.outerConeAngle = light.outerConeAngle;

    // Use shared PBR calculation from Common.hlsl
    return CalculateSpotLightPBR(lightInput, worldPos, N, V, albedo, metallic, roughness);
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
        GpuLight light = g_lights[lightIdx];

        // Dispatch to correct calculation based on light type
        if (light.type == LIGHT_TYPE_POINT) {
            lighting += CalculatePointLight(light, worldPos, N, V, albedo, metallic, roughness);
        }
        else if (light.type == LIGHT_TYPE_SPOT) {
            lighting += CalculateSpotLight(light, worldPos, N, V, albedo, metallic, roughness);
        }
    }

    return lighting;
}

#endif // CLUSTERED_SHADING_DS_HLSLI
