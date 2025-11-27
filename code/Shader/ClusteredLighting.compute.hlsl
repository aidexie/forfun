// Clustered Lighting Compute Shaders
// Two entry points:
// 1. CSBuildClusterGrid - Build view-space AABB for each cluster
// 2. CSCullLights - Cull lights into clusters using Sphere-AABB intersection

// Configuration (must match C++ ClusteredConfig)
#define TILE_SIZE 32
#define DEPTH_SLICES 16
#define MAX_LIGHTS_PER_CLUSTER 100

// ============================================================================
// Data Structures (must match C++)
// ============================================================================

struct ClusterAABB {
    float4 minPoint;  // xyz = min, w = unused
    float4 maxPoint;  // xyz = max, w = unused
};

struct ClusterData {
    uint offset;  // Offset in compact light index list
    uint count;   // Number of lights in this cluster
};

struct GpuPointLight {
    float3 position;  // World space
    float range;
    float3 color;
    float intensity;
};

// ============================================================================
// CSBuildClusterGrid - Build cluster AABBs
// ============================================================================

cbuffer ClusterCB : register(b0) {
    float4x4 g_inverseProjection;
    float g_nearZ;
    float g_farZ;
    uint g_numClustersX;
    uint g_numClustersY;
    uint g_numClustersZ;
    uint g_screenWidth;
    uint g_screenHeight;
    uint g_padding;
};

RWStructuredBuffer<ClusterAABB> g_clusterAABBs : register(u0);

// Logarithmic depth slice calculation
// Returns view-space Z for a given slice index
float GetDepthFromSlice(uint sliceIdx) {
    float t = (float)sliceIdx / (float)DEPTH_SLICES;
    // Logarithmic interpolation: Z = near * (far/near)^t
    return g_nearZ * pow(g_farZ / g_nearZ, t);
    // return t*g_farZ;
}

// Unproject NDC point to view space
float3 UnprojectNDC(float2 ndc, float viewZ) {
    // NDC: [-1, 1] (bottom-left to top-right in DX)
    // ViewZ is negative in view space (camera looks down -Z)
    float4 clipSpace = float4(ndc.x, ndc.y, 1.0, 1.0);  // Z=1 for far plane
    float4 viewSpace = mul(clipSpace, g_inverseProjection);

    // Scale by desired view-space Z
    // viewSpace.z is the depth at far plane, we want to scale to viewZ
    float3 result = viewSpace.xyz / viewSpace.w;
    result *= (viewZ / result.z);  // Scale to desired depth

    return result;
}

[numthreads(8, 8, 1)]
void CSBuildClusterGrid(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint3 clusterID = dispatchThreadID;

    // Bounds check
    if (clusterID.x >= g_numClustersX ||
        clusterID.y >= g_numClustersY ||
        clusterID.z >= g_numClustersZ) {
        return;
    }

    // Calculate tile boundaries in screen space (pixels)
    uint tileMinX = clusterID.x * TILE_SIZE;
    uint tileMaxX = min(tileMinX + TILE_SIZE, g_screenWidth);
    uint tileMinY = clusterID.y * TILE_SIZE;
    uint tileMaxY = min(tileMinY + TILE_SIZE, g_screenHeight);

    // Convert to NDC space [-1, 1]
    // DX11: Top-left (0,0) -> NDC (-1, 1), Bottom-right (W,H) -> NDC (1, -1)
    float2 ndcMin = float2(
        (float)tileMinX / (float)g_screenWidth * 2.0 - 1.0,
        1.0 - (float)tileMaxY / (float)g_screenHeight * 2.0  // Y flipped
    );
    float2 ndcMax = float2(
        (float)tileMaxX / (float)g_screenWidth * 2.0 - 1.0,
        1.0 - (float)tileMinY / (float)g_screenHeight * 2.0  // Y flipped
    );

    // Get depth range for this slice (logarithmic)
    float nearZ = GetDepthFromSlice(clusterID.z);
    float farZ = GetDepthFromSlice(clusterID.z + 1);

    // Build 8 corners of the cluster frustum in view space
    float3 corners[8];
    corners[0] = UnprojectNDC(float2(ndcMin.x, ndcMin.y), nearZ);
    corners[1] = UnprojectNDC(float2(ndcMax.x, ndcMin.y), nearZ);
    corners[2] = UnprojectNDC(float2(ndcMin.x, ndcMax.y), nearZ);
    corners[3] = UnprojectNDC(float2(ndcMax.x, ndcMax.y), nearZ);
    corners[4] = UnprojectNDC(float2(ndcMin.x, ndcMin.y), farZ);
    corners[5] = UnprojectNDC(float2(ndcMax.x, ndcMin.y), farZ);
    corners[6] = UnprojectNDC(float2(ndcMin.x, ndcMax.y), farZ);
    corners[7] = UnprojectNDC(float2(ndcMax.x, ndcMax.y), farZ);

    // Compute AABB from 8 corners
    float3 aabbMin = corners[0];
    float3 aabbMax = corners[0];

    [unroll]
    for (int i = 1; i < 8; i++) {
        aabbMin = min(aabbMin, corners[i]);
        aabbMax = max(aabbMax, corners[i]);
    }

    // Write to output buffer
    uint clusterIdx = clusterID.x +
                      clusterID.y * g_numClustersX +
                      clusterID.z * g_numClustersX * g_numClustersY;

    ClusterAABB aabb;
    aabb.minPoint = float4(aabbMin, 0.0);
    aabb.maxPoint = float4(aabbMax, 0.0);
    g_clusterAABBs[clusterIdx] = aabb;
}

// ============================================================================
// CSCullLights - Cull lights into clusters
// ============================================================================

cbuffer LightCullingCB : register(b0) {
    float4x4 g_view;
    uint g_numLights;
    uint g_numClustersX_Cull;
    uint g_numClustersY_Cull;
    uint g_numClustersZ_Cull;
};

StructuredBuffer<GpuPointLight> g_pointLights : register(t0);
StructuredBuffer<ClusterAABB> g_clusterAABBs_Read : register(t1);  // Read-only in culling

RWStructuredBuffer<ClusterData> g_clusterData : register(u0);
RWStructuredBuffer<uint> g_compactLightList : register(u1);
RWStructuredBuffer<uint> g_globalCounter : register(u2);  // Atomic counter for light list

// Sphere-AABB intersection test
bool SphereIntersectsAABB(float3 sphereCenter, float sphereRadius, float3 aabbMin, float3 aabbMax) {
    // Find closest point on AABB to sphere center
    float3 closestPoint = clamp(sphereCenter, aabbMin, aabbMax);

    // Check if distance from sphere center to closest point is <= radius
    float distanceSquared = dot(closestPoint - sphereCenter, closestPoint - sphereCenter);
    return distanceSquared <= (sphereRadius * sphereRadius);
}

[numthreads(8, 8, 1)]
void CSCullLights(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint3 clusterID = dispatchThreadID;

    // Bounds check
    if (clusterID.x >= g_numClustersX_Cull ||
        clusterID.y >= g_numClustersY_Cull ||
        clusterID.z >= g_numClustersZ_Cull) {
        return;
    }

    uint clusterIdx = clusterID.x +
                      clusterID.y * g_numClustersX_Cull +
                      clusterID.z * g_numClustersX_Cull * g_numClustersY_Cull;

    // Get cluster AABB (view space)
    ClusterAABB aabb = g_clusterAABBs_Read[clusterIdx];
    float3 aabbMin = aabb.minPoint.xyz;
    float3 aabbMax = aabb.maxPoint.xyz;

    // Temporary storage for light indices in this cluster
    uint lightIndices[MAX_LIGHTS_PER_CLUSTER];
    uint lightCount = 0;

    // Test all lights against this cluster
    for (uint i = 0; i < g_numLights; i++) {
        GpuPointLight light = g_pointLights[i];

        // Transform light position to view space
        float4 viewPos = mul(float4(light.position, 1.0), g_view);
        float3 lightPosView = viewPos.xyz;

        // Sphere-AABB intersection test
        if (SphereIntersectsAABB(lightPosView, light.range, aabbMin, aabbMax)) {
            if (lightCount < MAX_LIGHTS_PER_CLUSTER) {
                lightIndices[lightCount++] = i;
            }
            // If exceeds MAX_LIGHTS_PER_CLUSTER, silently drop (this is okay for now)
        }
    }

    // Allocate space in compact light list
    uint offset = 0;
    if (lightCount > 0) {
        InterlockedAdd(g_globalCounter[0], lightCount, offset);

        // Write light indices to compact list
        for (uint j = 0; j < lightCount; j++) {
            g_compactLightList[offset + j] = lightIndices[j];
        }
    }

    // Write cluster data
    ClusterData data;
    data.offset = offset;
    data.count = lightCount;
    g_clusterData[clusterIdx] = data;
}
