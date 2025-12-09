#include "RayTracer.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "Core/MaterialAsset.h"
#include "Core/MaterialManager.h"
#include "Core/FFLog.h"
#include <algorithm>
#include <cfloat>

using namespace DirectX;

// ============================================
// 初始化
// ============================================

bool CRayTracer::Initialize(CScene& scene)
{
    if (m_initialized) {
        CFFLog::Warning("[RayTracer] Already initialized, rebuilding...");
        Shutdown();
    }

    // 提取场景物体
    extractObjectsFromScene(scene);

    if (m_objects.empty()) {
        CFFLog::Warning("[RayTracer] No objects to trace!");
        m_initialized = true;
        return true;
    }

    // 构建 BVH
    buildBVH();

    m_initialized = true;
    CFFLog::Info("[RayTracer] Initialized: %d objects, %d BVH nodes",
                 (int)m_objects.size(), (int)m_bvhNodes.size());

    return true;
}

void CRayTracer::Shutdown()
{
    m_objects.clear();
    m_objectIndices.clear();
    m_bvhNodes.clear();
    m_rootNode = -1;
    m_initialized = false;
}

void CRayTracer::Rebuild(CScene& scene)
{
    Shutdown();
    Initialize(scene);
}

// ============================================
// 场景物体提取
// ============================================

void CRayTracer::extractObjectsFromScene(CScene& scene)
{
    m_objects.clear();
    auto& world = scene.GetWorld();

    for (size_t i = 0; i < world.Count(); i++)
    {
        auto* obj = world.Get(i);
        if (!obj) continue;

        auto* transform = obj->GetComponent<STransform>();
        auto* meshRenderer = obj->GetComponent<SMeshRenderer>();

        if (!transform || !meshRenderer) continue;

        // 获取局部空间 AABB
        XMFLOAT3 localMin, localMax;
        if (!meshRenderer->GetLocalBounds(localMin, localMax)) {
            continue;
        }

        // 将局部 AABB 的 8 个顶点变换到世界空间
        XMMATRIX worldMatrix = transform->WorldMatrix();

        XMFLOAT3 localCorners[8] = {
            {localMin.x, localMin.y, localMin.z},
            {localMax.x, localMin.y, localMin.z},
            {localMin.x, localMax.y, localMin.z},
            {localMax.x, localMax.y, localMin.z},
            {localMin.x, localMin.y, localMax.z},
            {localMax.x, localMin.y, localMax.z},
            {localMin.x, localMax.y, localMax.z},
            {localMax.x, localMax.y, localMax.z}
        };

        // 计算世界空间 AABB
        XMFLOAT3 worldMin = {FLT_MAX, FLT_MAX, FLT_MAX};
        XMFLOAT3 worldMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

        for (int c = 0; c < 8; c++) {
            XMVECTOR localPt = XMLoadFloat3(&localCorners[c]);
            XMVECTOR worldPt = XMVector3Transform(localPt, worldMatrix);
            XMFLOAT3 wp;
            XMStoreFloat3(&wp, worldPt);

            worldMin.x = std::min(worldMin.x, wp.x);
            worldMin.y = std::min(worldMin.y, wp.y);
            worldMin.z = std::min(worldMin.z, wp.z);
            worldMax.x = std::max(worldMax.x, wp.x);
            worldMax.y = std::max(worldMax.y, wp.y);
            worldMax.z = std::max(worldMax.z, wp.z);
        }

        // 获取材质 albedo
        XMFLOAT3 albedo = {0.5f, 0.5f, 0.5f};  // 默认灰色
        if (!meshRenderer->materialPath.empty()) {
            CMaterialAsset* material = CMaterialManager::Instance().Load(meshRenderer->materialPath);
            if (material) {
                albedo = material->albedo;
            }
        }

        // 创建 AABB 物体
        SAABBObject aabbObj;
        aabbObj.boundsMin = worldMin;
        aabbObj.boundsMax = worldMax;
        aabbObj.albedo = albedo;
        aabbObj.sceneObjectIndex = (int)i;

        m_objects.push_back(aabbObj);
    }
}

// ============================================
// BVH 构建
// ============================================

void CRayTracer::buildBVH()
{
    if (m_objects.empty()) {
        m_rootNode = -1;
        return;
    }

    // 初始化索引数组
    m_objectIndices.resize(m_objects.size());
    for (size_t i = 0; i < m_objects.size(); i++) {
        m_objectIndices[i] = (int)i;
    }

    m_bvhNodes.clear();
    m_bvhNodes.reserve(m_objects.size() * 2);  // 预估节点数量

    // 递归构建
    m_rootNode = buildBVHRecursive(0, (int)m_objects.size(), 0);
}

int CRayTracer::buildBVHRecursive(int start, int end, int depth)
{
    int count = end - start;

    // 创建节点
    SBVHNode node;
    computeBounds(start, end, node.boundsMin, node.boundsMax);

    // 叶子节点条件：物体太少或深度太深
    if (count <= MIN_OBJECTS_PER_LEAF || depth >= MAX_BVH_DEPTH) {
        node.objectStart = start;
        node.objectCount = count;
        node.leftChild = -1;
        node.rightChild = -1;

        int nodeIndex = (int)m_bvhNodes.size();
        m_bvhNodes.push_back(node);
        return nodeIndex;
    }

    // 选择分割轴（最长轴）
    float extentX = node.boundsMax.x - node.boundsMin.x;
    float extentY = node.boundsMax.y - node.boundsMin.y;
    float extentZ = node.boundsMax.z - node.boundsMin.z;

    int axis = 0;
    if (extentY > extentX && extentY > extentZ) axis = 1;
    else if (extentZ > extentX && extentZ > extentY) axis = 2;

    // 按中心点排序
    std::sort(m_objectIndices.begin() + start, m_objectIndices.begin() + end,
        [this, axis](int a, int b) {
            XMFLOAT3 centerA = getObjectCenter(a);
            XMFLOAT3 centerB = getObjectCenter(b);
            float va = (axis == 0) ? centerA.x : (axis == 1) ? centerA.y : centerA.z;
            float vb = (axis == 0) ? centerB.x : (axis == 1) ? centerB.y : centerB.z;
            return va < vb;
        });

    // 中点分割
    int mid = start + count / 2;

    // 递归构建子节点
    int nodeIndex = (int)m_bvhNodes.size();
    m_bvhNodes.push_back(node);  // 先占位

    int leftChild = buildBVHRecursive(start, mid, depth + 1);
    int rightChild = buildBVHRecursive(mid, end, depth + 1);

    // 更新节点
    m_bvhNodes[nodeIndex].leftChild = leftChild;
    m_bvhNodes[nodeIndex].rightChild = rightChild;
    m_bvhNodes[nodeIndex].objectStart = 0;
    m_bvhNodes[nodeIndex].objectCount = 0;

    return nodeIndex;
}

void CRayTracer::computeBounds(int start, int end,
                               XMFLOAT3& outMin, XMFLOAT3& outMax) const
{
    outMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    outMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (int i = start; i < end; i++) {
        int objIdx = m_objectIndices[i];
        const auto& obj = m_objects[objIdx];

        outMin.x = std::min(outMin.x, obj.boundsMin.x);
        outMin.y = std::min(outMin.y, obj.boundsMin.y);
        outMin.z = std::min(outMin.z, obj.boundsMin.z);
        outMax.x = std::max(outMax.x, obj.boundsMax.x);
        outMax.y = std::max(outMax.y, obj.boundsMax.y);
        outMax.z = std::max(outMax.z, obj.boundsMax.z);
    }
}

XMFLOAT3 CRayTracer::getObjectCenter(int index) const
{
    const auto& obj = m_objects[index];
    return {
        (obj.boundsMin.x + obj.boundsMax.x) * 0.5f,
        (obj.boundsMin.y + obj.boundsMax.y) * 0.5f,
        (obj.boundsMin.z + obj.boundsMax.z) * 0.5f
    };
}

// ============================================
// 射线相交测试
// ============================================

bool CRayTracer::rayAABBIntersect(const SRay& ray,
                                  const XMFLOAT3& boundsMin,
                                  const XMFLOAT3& boundsMax,
                                  float& tNear, float& tFar) const
{
    // Slab method
    float tmin = ray.tMin;
    float tmax = ray.tMax;

    // X slab
    float invDirX = (ray.direction.x != 0.0f) ? 1.0f / ray.direction.x : FLT_MAX;
    float t1 = (boundsMin.x - ray.origin.x) * invDirX;
    float t2 = (boundsMax.x - ray.origin.x) * invDirX;
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;

    // Y slab
    float invDirY = (ray.direction.y != 0.0f) ? 1.0f / ray.direction.y : FLT_MAX;
    t1 = (boundsMin.y - ray.origin.y) * invDirY;
    t2 = (boundsMax.y - ray.origin.y) * invDirY;
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;

    // Z slab
    float invDirZ = (ray.direction.z != 0.0f) ? 1.0f / ray.direction.z : FLT_MAX;
    t1 = (boundsMin.z - ray.origin.z) * invDirZ;
    t2 = (boundsMax.z - ray.origin.z) * invDirZ;
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;

    tNear = tmin;
    tFar = tmax;
    return true;
}

bool CRayTracer::rayAABBIntersectWithNormal(const SRay& ray,
                                            const XMFLOAT3& boundsMin,
                                            const XMFLOAT3& boundsMax,
                                            float& tHit,
                                            XMFLOAT3& outNormal) const
{
    float tNear, tFar;
    if (!rayAABBIntersect(ray, boundsMin, boundsMax, tNear, tFar)) {
        return false;
    }

    tHit = tNear;

    // 计算命中点
    XMFLOAT3 hitPos = {
        ray.origin.x + ray.direction.x * tHit,
        ray.origin.y + ray.direction.y * tHit,
        ray.origin.z + ray.direction.z * tHit
    };

    // 确定命中的是哪个面（找最近的面）
    float epsilon = 0.001f;
    outNormal = {0, 0, 0};

    if (std::abs(hitPos.x - boundsMin.x) < epsilon) outNormal = {-1, 0, 0};
    else if (std::abs(hitPos.x - boundsMax.x) < epsilon) outNormal = {1, 0, 0};
    else if (std::abs(hitPos.y - boundsMin.y) < epsilon) outNormal = {0, -1, 0};
    else if (std::abs(hitPos.y - boundsMax.y) < epsilon) outNormal = {0, 1, 0};
    else if (std::abs(hitPos.z - boundsMin.z) < epsilon) outNormal = {0, 0, -1};
    else if (std::abs(hitPos.z - boundsMax.z) < epsilon) outNormal = {0, 0, 1};
    else {
        // 备用：使用射线方向的反向
        outNormal = {-ray.direction.x, -ray.direction.y, -ray.direction.z};
        // 归一化
        XMVECTOR n = XMLoadFloat3(&outNormal);
        n = XMVector3Normalize(n);
        XMStoreFloat3(&outNormal, n);
    }

    return true;
}

// ============================================
// BVH 遍历
// ============================================

void CRayTracer::traverseBVH(const SRay& ray, SRayHit& closestHit) const
{
    if (m_rootNode < 0 || m_bvhNodes.empty()) {
        return;
    }

    traverseBVHRecursive(m_rootNode, ray, closestHit);
}

void CRayTracer::traverseBVHRecursive(int nodeIndex, const SRay& ray, SRayHit& closestHit) const
{
    if (nodeIndex < 0 || nodeIndex >= (int)m_bvhNodes.size()) {
        return;
    }

    const SBVHNode& node = m_bvhNodes[nodeIndex];

    // 先测试节点的 AABB
    float tNear, tFar;
    if (!rayAABBIntersect(ray, node.boundsMin, node.boundsMax, tNear, tFar)) {
        return;  // 不相交，跳过整个子树
    }

    // 如果节点 AABB 的最近点已经比当前命中更远，跳过
    if (tNear > closestHit.distance) {
        return;
    }

    if (node.IsLeaf()) {
        // 叶子节点：测试所有物体
        for (int i = node.objectStart; i < node.objectStart + node.objectCount; i++) {
            int objIdx = m_objectIndices[i];
            const auto& obj = m_objects[objIdx];

            float tHit;
            XMFLOAT3 hitNormal;
            if (rayAABBIntersectWithNormal(ray, obj.boundsMin, obj.boundsMax, tHit, hitNormal)) {
                if (tHit < closestHit.distance && tHit >= ray.tMin) {
                    closestHit.valid = true;
                    closestHit.distance = tHit;
                    closestHit.position = {
                        ray.origin.x + ray.direction.x * tHit,
                        ray.origin.y + ray.direction.y * tHit,
                        ray.origin.z + ray.direction.z * tHit
                    };
                    closestHit.normal = hitNormal;
                    closestHit.albedo = obj.albedo;
                    closestHit.objectIndex = objIdx;
                }
            }
        }
    } else {
        // 内部节点：递归遍历子节点
        // 优化：先遍历更近的子节点
        traverseBVHRecursive(node.leftChild, ray, closestHit);
        traverseBVHRecursive(node.rightChild, ray, closestHit);
    }
}

// ============================================
// 公共查询接口
// ============================================

SRayHit CRayTracer::TraceRay(const SRay& ray) const
{
    SRayHit hit;
    hit.valid = false;
    hit.distance = FLT_MAX;

    if (!m_initialized || m_objects.empty()) {
        return hit;
    }

    traverseBVH(ray, hit);
    return hit;
}

SRayHit CRayTracer::TraceRay(const XMFLOAT3& origin, const XMFLOAT3& direction) const
{
    SRay ray;
    ray.origin = origin;
    ray.direction = direction;
    ray.tMin = 0.001f;
    ray.tMax = FLT_MAX;
    return TraceRay(ray);
}

bool CRayTracer::TraceVisibility(const XMFLOAT3& from, const XMFLOAT3& to) const
{
    XMFLOAT3 dir = {
        to.x - from.x,
        to.y - from.y,
        to.z - from.z
    };

    // 计算距离
    XMVECTOR dirVec = XMLoadFloat3(&dir);
    float dist = XMVectorGetX(XMVector3Length(dirVec));

    if (dist < 0.001f) {
        return true;  // 同一点，认为可见
    }

    // 归一化
    dirVec = XMVector3Normalize(dirVec);
    XMStoreFloat3(&dir, dirVec);

    SRay ray;
    ray.origin = from;
    ray.direction = dir;
    ray.tMin = 0.001f;
    ray.tMax = dist - 0.001f;  // 不包括终点

    SRayHit hit = TraceRay(ray);
    return !hit.valid;  // 没有命中 = 可见
}

bool CRayTracer::TraceShadowRay(const XMFLOAT3& origin,
                                const XMFLOAT3& lightDir,
                                float maxDistance) const
{
    SRay ray;
    ray.origin = origin;
    ray.direction = lightDir;
    ray.tMin = 0.001f;
    ray.tMax = maxDistance;

    SRayHit hit = TraceRay(ray);
    return hit.valid;  // 命中 = 在阴影中
}
