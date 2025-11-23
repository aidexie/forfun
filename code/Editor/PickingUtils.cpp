// Editor/PickingUtils.cpp
#include "PickingUtils.h"
#include <algorithm>
#include <limits>

using namespace DirectX;

namespace PickingUtils {

Ray GenerateRayFromScreen(
    float screenX, float screenY,
    float viewportWidth, float viewportHeight,
    XMMATRIX viewMatrix,
    XMMATRIX projMatrix
)
{
    // 1. Screen space to NDC (Normalized Device Coordinates)
    // DirectX viewport: (0,0) = top-left, Y increases downward
    // NDC: X [-1,1], Y [-1,1], origin at center
    float ndcX = (screenX / viewportWidth) * 2.0f - 1.0f;
    float ndcY = 1.0f - (screenY / viewportHeight) * 2.0f;  // Flip Y

    // 2. NDC to view space (inverse projection)
    XMMATRIX invProj = XMMatrixInverse(nullptr, projMatrix);

    // Create two points: one at near plane (Z=0), one at far plane (Z=1)
    XMVECTOR nearPoint = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    XMVECTOR farPoint = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

    // Transform to view space
    XMVECTOR nearView = XMVector3TransformCoord(nearPoint, invProj);
    XMVECTOR farView = XMVector3TransformCoord(farPoint, invProj);

    // 3. View space to world space (inverse view)
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
    XMVECTOR nearWorld = XMVector3TransformCoord(nearView, invView);
    XMVECTOR farWorld = XMVector3TransformCoord(farView, invView);

    // 4. Construct ray
    Ray ray;
    XMStoreFloat3(&ray.origin, nearWorld);

    XMVECTOR direction = XMVector3Normalize(farWorld - nearWorld);
    XMStoreFloat3(&ray.direction, direction);

    return ray;
}

std::optional<float> RayAABBIntersect(
    const Ray& ray,
    const XMFLOAT3& aabbMin,
    const XMFLOAT3& aabbMax
)
{
    // Slab method: test intersection with 3 pairs of parallel planes (X, Y, Z)
    // https://tavianator.com/2011/ray_box.html

    XMVECTOR rayOrigin = XMLoadFloat3(&ray.origin);
    XMVECTOR rayDir = XMLoadFloat3(&ray.direction);
    XMVECTOR boxMin = XMLoadFloat3(&aabbMin);
    XMVECTOR boxMax = XMLoadFloat3(&aabbMax);

    // Calculate inverse direction to avoid division in loop
    XMVECTOR invDir = XMVectorReciprocal(rayDir);

    // Calculate intersections with slab planes
    XMVECTOR t1 = XMVectorMultiply(XMVectorSubtract(boxMin, rayOrigin), invDir);
    XMVECTOR t2 = XMVectorMultiply(XMVectorSubtract(boxMax, rayOrigin), invDir);

    // Find tMin and tMax for each axis
    XMVECTOR tMin = XMVectorMin(t1, t2);
    XMVECTOR tMax = XMVectorMax(t1, t2);

    // Find the largest tMin and smallest tMax
    float tNear = XMVectorGetX(tMin);
    tNear = std::max(tNear, XMVectorGetY(tMin));
    tNear = std::max(tNear, XMVectorGetZ(tMin));

    float tFar = XMVectorGetX(tMax);
    tFar = std::min(tFar, XMVectorGetY(tMax));
    tFar = std::min(tFar, XMVectorGetZ(tMax));

    // Check for intersection
    // Ray misses if tNear > tFar (no overlap between slabs)
    // Ray misses if tFar < 0 (box is behind ray)
    if (tNear > tFar || tFar < 0.0f) {
        return std::nullopt;  // No intersection
    }

    // Return the nearest intersection distance
    // If tNear < 0, ray origin is inside box, return tFar
    float hitDistance = (tNear < 0.0f) ? tFar : tNear;
    return hitDistance;
}

void TransformAABB(
    const XMFLOAT3& localMin,
    const XMFLOAT3& localMax,
    XMMATRIX worldMatrix,
    XMFLOAT3& outWorldMin,
    XMFLOAT3& outWorldMax
)
{
    // Transform all 8 corners of the AABB to world space
    // Then compute the new AABB that encloses all transformed corners

    XMFLOAT3 corners[8] = {
        { localMin.x, localMin.y, localMin.z },
        { localMax.x, localMin.y, localMin.z },
        { localMin.x, localMax.y, localMin.z },
        { localMax.x, localMax.y, localMin.z },
        { localMin.x, localMin.y, localMax.z },
        { localMax.x, localMin.y, localMax.z },
        { localMin.x, localMax.y, localMax.z },
        { localMax.x, localMax.y, localMax.z }
    };

    // Initialize with extreme values
    XMFLOAT3 worldMin(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 worldMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // Transform each corner and update world bounds
    for (int i = 0; i < 8; ++i) {
        XMVECTOR corner = XMLoadFloat3(&corners[i]);
        XMVECTOR worldCorner = XMVector3TransformCoord(corner, worldMatrix);

        XMFLOAT3 wc;
        XMStoreFloat3(&wc, worldCorner);

        worldMin.x = std::min(worldMin.x, wc.x);
        worldMin.y = std::min(worldMin.y, wc.y);
        worldMin.z = std::min(worldMin.z, wc.z);

        worldMax.x = std::max(worldMax.x, wc.x);
        worldMax.y = std::max(worldMax.y, wc.y);
        worldMax.z = std::max(worldMax.z, wc.z);
    }

    outWorldMin = worldMin;
    outWorldMax = worldMax;
}

} // namespace PickingUtils
