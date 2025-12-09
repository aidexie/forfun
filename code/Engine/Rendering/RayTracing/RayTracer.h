#pragma once
#include <DirectXMath.h>
#include <vector>
#include <string>

class CScene;

// ============================================
// Ray Tracing 数据结构
// ============================================

struct SRay
{
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction;
    float tMin = 0.001f;   // 避免自相交
    float tMax = FLT_MAX;
};

struct SRayHit
{
    bool valid = false;
    float distance = FLT_MAX;
    DirectX::XMFLOAT3 position = {0, 0, 0};
    DirectX::XMFLOAT3 normal = {0, 1, 0};    // AABB 面法线
    DirectX::XMFLOAT3 albedo = {0.5f, 0.5f, 0.5f};  // 物体 albedo（简化版从材质获取）
    int objectIndex = -1;
};

// ============================================
// AABB 物体（简化版几何体）
// ============================================
struct SAABBObject
{
    DirectX::XMFLOAT3 boundsMin;
    DirectX::XMFLOAT3 boundsMax;
    DirectX::XMFLOAT3 albedo = {0.5f, 0.5f, 0.5f};  // 漫反射颜色
    int sceneObjectIndex = -1;  // 对应场景中的 GameObject 索引
};

// ============================================
// BVH 节点
// ============================================
struct SBVHNode
{
    DirectX::XMFLOAT3 boundsMin;
    DirectX::XMFLOAT3 boundsMax;

    int leftChild = -1;     // 左子节点索引（-1 = 叶子）
    int rightChild = -1;    // 右子节点索引

    // 叶子节点数据
    int objectStart = 0;    // 物体起始索引
    int objectCount = 0;    // 物体数量

    bool IsLeaf() const { return leftChild < 0; }
};

// ============================================
// CRayTracer - CPU 射线追踪器
// ============================================
// 使用 AABB 简化版几何体进行射线追踪
// 后续可升级为三角形精度
// ============================================
class CRayTracer
{
public:
    CRayTracer() = default;
    ~CRayTracer() = default;

    // ============================================
    // 初始化
    // ============================================

    // 从场景构建 BVH
    // 提取所有 MeshRenderer 的世界空间 AABB
    bool Initialize(CScene& scene);
    void Shutdown();

    // 重建 BVH（场景变化后调用）
    void Rebuild(CScene& scene);

    // ============================================
    // 射线查询
    // ============================================

    // 追踪射线，返回最近命中
    SRayHit TraceRay(const SRay& ray) const;

    // 追踪射线（简化接口）
    SRayHit TraceRay(const DirectX::XMFLOAT3& origin,
                     const DirectX::XMFLOAT3& direction) const;

    // 可见性测试（从 from 到 to 是否有遮挡）
    bool TraceVisibility(const DirectX::XMFLOAT3& from,
                         const DirectX::XMFLOAT3& to) const;

    // 阴影射线（从点向光源方向，检测是否被遮挡）
    bool TraceShadowRay(const DirectX::XMFLOAT3& origin,
                        const DirectX::XMFLOAT3& lightDir,
                        float maxDistance) const;

    // ============================================
    // 状态查询
    // ============================================
    bool IsInitialized() const { return m_initialized; }
    int GetObjectCount() const { return (int)m_objects.size(); }
    int GetBVHNodeCount() const { return (int)m_bvhNodes.size(); }

private:
    // ============================================
    // BVH 构建
    // ============================================
    void extractObjectsFromScene(CScene& scene);
    void buildBVH();
    int buildBVHRecursive(int start, int end, int depth);

    // 计算 AABB 并集
    void computeBounds(int start, int end,
                       DirectX::XMFLOAT3& outMin,
                       DirectX::XMFLOAT3& outMax) const;

    // 计算 AABB 中心
    DirectX::XMFLOAT3 getObjectCenter(int index) const;

    // ============================================
    // 射线相交测试
    // ============================================

    // Ray-AABB 相交（slab method）
    bool rayAABBIntersect(const SRay& ray,
                          const DirectX::XMFLOAT3& boundsMin,
                          const DirectX::XMFLOAT3& boundsMax,
                          float& tNear, float& tFar) const;

    // Ray-AABB 相交并计算命中法线
    bool rayAABBIntersectWithNormal(const SRay& ray,
                                    const DirectX::XMFLOAT3& boundsMin,
                                    const DirectX::XMFLOAT3& boundsMax,
                                    float& tHit,
                                    DirectX::XMFLOAT3& outNormal) const;

    // BVH 遍历
    void traverseBVH(const SRay& ray, SRayHit& closestHit) const;
    void traverseBVHRecursive(int nodeIndex, const SRay& ray, SRayHit& closestHit) const;

private:
    bool m_initialized = false;

    // 场景物体（AABB）
    std::vector<SAABBObject> m_objects;
    std::vector<int> m_objectIndices;  // 用于 BVH 构建时重排序

    // BVH
    std::vector<SBVHNode> m_bvhNodes;
    int m_rootNode = -1;

    // 配置
    static const int MAX_BVH_DEPTH = 20;
    static const int MIN_OBJECTS_PER_LEAF = 2;
};
