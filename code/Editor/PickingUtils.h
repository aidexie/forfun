// Editor/PickingUtils.h
#pragma once
#include <DirectXMath.h>
#include <optional>

namespace PickingUtils {

// Ray structure for picking
struct Ray {
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction;  // Normalized
};

// Generate a ray from screen coordinates
// screenX, screenY: Mouse position in pixels (0,0 = top-left)
// viewportWidth, viewportHeight: Viewport size in pixels
// viewMatrix, projMatrix: Camera matrices
Ray GenerateRayFromScreen(
    float screenX, float screenY,
    float viewportWidth, float viewportHeight,
    DirectX::XMMATRIX viewMatrix,
    DirectX::XMMATRIX projMatrix
);

// Ray-AABB intersection test (slab method)
// Returns distance to intersection point if hit, empty otherwise
// aabbMin, aabbMax: Axis-aligned bounding box in world space
std::optional<float> RayAABBIntersect(
    const Ray& ray,
    const DirectX::XMFLOAT3& aabbMin,
    const DirectX::XMFLOAT3& aabbMax
);

// Transform local-space AABB to world-space AABB
// localMin, localMax: AABB in object's local space
// worldMatrix: Object's world transformation matrix
void TransformAABB(
    const DirectX::XMFLOAT3& localMin,
    const DirectX::XMFLOAT3& localMax,
    DirectX::XMMATRIX worldMatrix,
    DirectX::XMFLOAT3& outWorldMin,
    DirectX::XMFLOAT3& outWorldMax
);

} // namespace PickingUtils
