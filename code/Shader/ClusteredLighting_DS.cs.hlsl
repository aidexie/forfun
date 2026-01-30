// ============================================
// ClusteredLighting_DS.cs.hlsl - Clustered Lighting (SM 5.1)
// ============================================
// Descriptor Set version using unified compute layout (space1)
//
// Entry Points:
//   CSBuildClusterGrid - Build view-space AABB for each cluster
//   CSCullLights       - Cull lights into clusters using Sphere-AABB intersection
// ============================================

// Configuration (must match C++ ClusteredConfig)
#define TILE_SIZE 32
#define DEPTH_SLICES 16
#define MAX_LIGHTS_PER_CLUSTER 100

// ============================================
// Data Structures (must match C++)
// ============================================

struct ClusterAABB {
    float4 minPoint;  // xyz = min, w = unused
    float4 maxPoint;  // xyz = max, w = unused
};

struct ClusterData {
    uint offset;  // Offset in compact light index list
    uint count;   // Number of lights in this cluster
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

// ============================================
// Set 1: PerPass (space1) - Unified Compute Layout
// ============================================

// Constant Buffer (b0, space1)
// This is a union-style CB - different passes interpret it differently
// The volatile CBV system handles this by uploading the correct data each dispatch
cbuffer CB_Clustered : register(b0, space1) {
    // For CSBuildClusterGrid (SClusterCB):
    float4x4 gInverseProjection;  // or gView for CullLights
    float gNearZ;                 // or asuint = gNumLights for CullLights
    float gFarZ;
    uint gNumClustersX;
    uint gNumClustersY;
    uint gNumClustersZ;
    uint gScreenWidth;
    uint gScreenHeight;
    uint gPadding;
};

// Structured Buffer SRVs (t0-t7, space1)
StructuredBuffer<GpuLight> gLights : register(t0, space1);
StructuredBuffer<ClusterAABB> gClusterAABBs_Read : register(t1, space1);

// UAVs (u0-u3, space1)
// Note: u0 is used by both passes but for different buffers
// BuildClusterGrid writes to ClusterAABB buffer
// CullLights writes to ClusterData buffer
// This works because they're dispatched separately with different bindings
RWStructuredBuffer<ClusterAABB> gClusterAABBs_Write : register(u0, space1);
RWStructuredBuffer<ClusterData> gClusterData : register(u0, space1);
RWStructuredBuffer<uint> gCompactLightList : register(u1, space1);
RWStructuredBuffer<uint> gGlobalCounter : register(u2, space1);

// Samplers (s0-s3, space1) - not used but declared for layout compatibility
SamplerState gPointSampler : register(s0, space1);
SamplerState gLinearSampler : register(s1, space1);

// ============================================
// CSBuildClusterGrid - Build cluster AABBs
// ============================================

// Logarithmic depth slice calculation
// Returns view-space Z for a given slice index
float GetDepthFromSlice(uint sliceIdx) {
    float t = (float)sliceIdx / (float)DEPTH_SLICES;
    // Logarithmic interpolation: Z = near * (far/near)^t
    return gNearZ * pow(gFarZ / gNearZ, t);
}

// Unproject NDC point to view space
float3 UnprojectNDC(float2 ndc, float viewZ) {
    // NDC: [-1, 1] (bottom-left to top-right in DX)
    // ViewZ is negative in view space (camera looks down -Z)
    float4 clipSpace = float4(ndc.x, ndc.y, 1.0, 1.0);  // Z=1 for far plane
    float4 viewSpace = mul(clipSpace, gInverseProjection);

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
    if (clusterID.x >= gNumClustersX ||
        clusterID.y >= gNumClustersY ||
        clusterID.z >= gNumClustersZ) {
        return;
    }

    // Calculate tile boundaries in screen space (pixels)
    uint tileMinX = clusterID.x * TILE_SIZE;
    uint tileMaxX = min(tileMinX + TILE_SIZE, gScreenWidth);
    uint tileMinY = clusterID.y * TILE_SIZE;
    uint tileMaxY = min(tileMinY + TILE_SIZE, gScreenHeight);

    // Convert to NDC space [-1, 1]
    // DX11: Top-left (0,0) -> NDC (-1, 1), Bottom-right (W,H) -> NDC (1, -1)
    float2 ndcMin = float2(
        (float)tileMinX / (float)gScreenWidth * 2.0 - 1.0,
        1.0 - (float)tileMaxY / (float)gScreenHeight * 2.0  // Y flipped
    );
    float2 ndcMax = float2(
        (float)tileMaxX / (float)gScreenWidth * 2.0 - 1.0,
        1.0 - (float)tileMinY / (float)gScreenHeight * 2.0  // Y flipped
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
                      clusterID.y * gNumClustersX +
                      clusterID.z * gNumClustersX * gNumClustersY;

    ClusterAABB aabb;
    aabb.minPoint = float4(aabbMin, 0.0);
    aabb.maxPoint = float4(aabbMax, 0.0);
    gClusterAABBs_Write[clusterIdx] = aabb;
}

// ============================================
// CSCullLights - Cull lights into clusters
// ============================================

// For CullLights, we reinterpret the CB:
// gInverseProjection -> view matrix
// gNearZ -> numLights (as uint via asuint)
#define gView gInverseProjection
#define gNumLights asuint(gNearZ)

// Sphere-AABB intersection test (for Point lights)
bool SphereIntersectsAABB(float3 sphereCenter, float sphereRadius, float3 aabbMin, float3 aabbMax) {
    // Find closest point on AABB to sphere center
    float3 closestPoint = clamp(sphereCenter, aabbMin, aabbMax);

    // Check if distance from sphere center to closest point is <= radius
    float distanceSquared = dot(closestPoint - sphereCenter, closestPoint - sphereCenter);
    return distanceSquared <= (sphereRadius * sphereRadius);
}

// Cone-AABB intersection test (for Spot lights)
// Conservative approach: Use bounding sphere that contains the entire cone
bool ConeIntersectsAABB(float3 coneApex, float3 coneDir, float coneRange,
                        float cosOuterAngle, float3 aabbMin, float3 aabbMax) {
    // Calculate cone base radius
    float outerAngle = acos(cosOuterAngle);
    float baseRadius = coneRange * tan(outerAngle);

    // Bounding sphere center: midpoint of cone axis
    float3 sphereCenter = coneApex + coneDir * (coneRange * 0.5);

    // Bounding sphere radius: distance from center to cone base edge
    float halfRange = coneRange * 0.5;
    float sphereRadius = sqrt(halfRange * halfRange + baseRadius * baseRadius);

    // Use existing sphere-AABB test
    return SphereIntersectsAABB(sphereCenter, sphereRadius, aabbMin, aabbMax);
}

[numthreads(8, 8, 1)]
void CSCullLights(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint3 clusterID = dispatchThreadID;

    // Bounds check
    if (clusterID.x >= gNumClustersX ||
        clusterID.y >= gNumClustersY ||
        clusterID.z >= gNumClustersZ) {
        return;
    }

    uint clusterIdx = clusterID.x +
                      clusterID.y * gNumClustersX +
                      clusterID.z * gNumClustersX * gNumClustersY;

    // Get cluster AABB (view space)
    ClusterAABB aabb = gClusterAABBs_Read[clusterIdx];
    float3 aabbMin = aabb.minPoint.xyz;
    float3 aabbMax = aabb.maxPoint.xyz;

    // Temporary storage for light indices in this cluster
    uint lightIndices[MAX_LIGHTS_PER_CLUSTER];
    uint lightCount = 0;

    // Test all lights against this cluster
    for (uint i = 0; i < gNumLights; i++) {
        GpuLight light = gLights[i];

        // Transform light position to view space
        float4 viewPos = mul(float4(light.position, 1.0), gView);
        float3 lightPosView = viewPos.xyz;

        bool intersects = false;

        // Choose intersection test based on light type
        if (light.type == LIGHT_TYPE_POINT) {
            // Point light: Sphere-AABB test
            intersects = SphereIntersectsAABB(lightPosView, light.range, aabbMin, aabbMax);
        }
        else if (light.type == LIGHT_TYPE_SPOT) {
            // Spot light: Cone-AABB test
            // Transform direction to view space
            float3 lightDirView = mul(float4(light.direction, 0.0), gView).xyz;
            intersects = ConeIntersectsAABB(lightPosView, lightDirView, light.range,
                                           light.outerConeAngle, aabbMin, aabbMax);
        }

        if (intersects) {
            if (lightCount < MAX_LIGHTS_PER_CLUSTER) {
                lightIndices[lightCount++] = i;
            }
            // If exceeds MAX_LIGHTS_PER_CLUSTER, silently drop
        }
    }

    // Allocate space in compact light list
    uint offset = 0;
    if (lightCount > 0) {
        InterlockedAdd(gGlobalCounter[0], lightCount, offset);

        // Write light indices to compact list
        for (uint j = 0; j < lightCount; j++) {
            gCompactLightList[offset + j] = lightIndices[j];
        }
    }

    // Write cluster data
    ClusterData data;
    data.offset = offset;
    data.count = lightCount;
    gClusterData[clusterIdx] = data;
}
