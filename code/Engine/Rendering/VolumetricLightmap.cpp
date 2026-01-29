#include "VolumetricLightmap.h"
#include "RayTracing/PathTraceBaker.h"
#include "RayTracing/DXRCubemapBaker.h"
#include "Engine/Scene.h"
#include "Engine/GameObject.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/MeshRenderer.h"
#include "RHI/RHIManager.h"
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIDescriptors.h"
#include "RHI/IDescriptorSet.h"
#include "RHI/PerFrameSlots.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include "Core/TextureManager.h"
#include <DirectXPackedVector.h>
#include <cmath>
#include <cfloat>
#include <fstream>
#include <chrono>

using namespace DirectX;
using namespace DirectX::PackedVector;

// ============================================
// 生命周期
// ============================================

CVolumetricLightmap::CVolumetricLightmap() 
{
    // 创建 sampler（不依赖烘焙数据，在禁用时也需要绑定以避免 D3D11 warning）
    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (renderContext)
    {
        RHI::SamplerDesc samplerDesc;
        samplerDesc.filter = RHI::EFilter::MinMagMipLinear;
        samplerDesc.addressU = RHI::ETextureAddressMode::Clamp;
        samplerDesc.addressV = RHI::ETextureAddressMode::Clamp;
        samplerDesc.addressW = RHI::ETextureAddressMode::Clamp;
        m_sampler.reset(renderContext->CreateSampler(samplerDesc));
    }
}

CVolumetricLightmap::~CVolumetricLightmap() = default;

bool CVolumetricLightmap::Initialize(const Config& config)
{
    m_config = config;

    // 计算派生参数
    computeDerivedParams();


    CFFLog::Info("[VolumetricLightmap] Initialized:");
    CFFLog::Info("  Volume: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)",
        m_config.volumeMin.x, m_config.volumeMin.y, m_config.volumeMin.z,
        m_config.volumeMax.x, m_config.volumeMax.y, m_config.volumeMax.z);
    CFFLog::Info("  Min Brick World Size: %.2f m", m_config.minBrickWorldSize);
    CFFLog::Info("  Derived MaxLevel: %d", m_derived.maxLevel);
    CFFLog::Info("  Derived Indirection Resolution: %d^3", m_derived.indirectionResolution);
    CFFLog::Info("  Root Brick Size: %.2f m", m_derived.rootBrickSize);

    m_initialized = true;
    if (!m_dxrBaker)
    {
        m_dxrBaker = std::make_unique<CDXRCubemapBaker>();
    }

    if (!m_dxrBaker->IsReady())
    {
        if (!m_dxrBaker->Initialize())
        {
            CFFLog::Error("[VolumetricLightmap] Failed to initialize DXR baker");
        }
    }
    return true;
}

void CVolumetricLightmap::Shutdown()
{
    m_octreeNodes.clear();
    m_bricks.clear();
    m_indirectionData.clear();
    m_brickAtlasSH0.clear();
    m_brickAtlasSH1.clear();
    m_brickAtlasSH2.clear();
    m_brickInfoData.clear();

    m_indirectionTexture.reset();
    for (int i = 0; i < 3; i++) {
        m_brickAtlasTexture[i].reset();
    }
    m_constantBuffer.reset();
    m_brickInfoBuffer.reset();
    m_sampler.reset();

    m_initialized = false;
    m_enabled = false;
    m_gpuResourcesCreated = false;
}

// ============================================
// 派生参数计算
// ============================================

void CVolumetricLightmap::computeDerivedParams()
{
    // 计算体积尺寸
    float volumeSizeX = m_config.volumeMax.x - m_config.volumeMin.x;
    float volumeSizeY = m_config.volumeMax.y - m_config.volumeMin.y;
    float volumeSizeZ = m_config.volumeMax.z - m_config.volumeMin.z;
    float maxVolumeSize = std::max({volumeSizeX, volumeSizeY, volumeSizeZ});

    // 使用最大边作为根节点 Brick 尺寸（立方体化）
    m_derived.rootBrickSize = maxVolumeSize;

    // 计算需要多少级细分才能达到 minBrickWorldSize
    // rootBrickSize / 2^maxLevel = minBrickWorldSize
    // maxLevel = log2(rootBrickSize / minBrickWorldSize)
    if (m_config.minBrickWorldSize > 0) {
        float ratio = m_derived.rootBrickSize / m_config.minBrickWorldSize;
        m_derived.maxLevel = (int)std::ceil(std::log2(ratio));
        m_derived.maxLevel = std::clamp(m_derived.maxLevel, 0, VL_MAX_LEVEL);
    } else {
        m_derived.maxLevel = 0;
    }

    // Indirection 分辨率 = 2^maxLevel
    m_derived.indirectionResolution = 1 << m_derived.maxLevel;

    // Atlas Size 在八叉树构建后计算（需要知道实际 Brick 数量）
    m_derived.actualBrickCount = 0;
    m_derived.brickAtlasSize = 0;
}

void CVolumetricLightmap::computeAtlasSize()
{
    int brickCount = m_derived.actualBrickCount;
    if (brickCount == 0) {
        m_derived.brickAtlasSize = VL_BRICK_SIZE;  // 最小 1 个 Brick
        m_atlasBricksPerSide = 1;
        return;
    }

    // 计算能容纳这么多 Brick 的最小立方体边长（以 Brick 为单位）
    int bricksNeeded = brickCount;
    int bricksPerSide = (int)std::ceil(std::cbrt((double)bricksNeeded));

    // 确保至少能放下所有 Brick
    while (bricksPerSide * bricksPerSide * bricksPerSide < brickCount) {
        bricksPerSide++;
    }

    m_atlasBricksPerSide = bricksPerSide;

    // Atlas 尺寸 = Brick 数量 × Brick 体素尺寸
    int atlasSize = bricksPerSide * VL_BRICK_SIZE;

    // 向上取整到 2 的幂次（GPU 纹理优化，可选）
    // atlasSize = nextPowerOf2(atlasSize);

    m_derived.brickAtlasSize = atlasSize;

    CFFLog::Info("[VolumetricLightmap] Atlas computed:");
    CFFLog::Info("  Brick Count: %d", brickCount);
    CFFLog::Info("  Bricks Per Side: %d", m_atlasBricksPerSide);
    CFFLog::Info("  Atlas Size: %d^3 (%d voxels)", atlasSize, atlasSize * atlasSize * atlasSize);
    CFFLog::Info("  Atlas Utilization: %.1f%%",
        100.0f * brickCount / (bricksPerSide * bricksPerSide * bricksPerSide));
}

// ============================================
// 八叉树构建
// ============================================

void CVolumetricLightmap::BuildOctree(CScene& scene)
{
    if (!m_initialized) {
        CFFLog::Error("[VolumetricLightmap] Not initialized!");
        return;
    }

    // 清空现有数据
    m_octreeNodes.clear();
    m_bricks.clear();
    m_atlasNextX = 0;
    m_atlasNextY = 0;
    m_atlasNextZ = 0;

    // 创建根节点
    SOctreeNode root;
    root.boundsMin = m_config.volumeMin;
    root.boundsMax = m_config.volumeMax;
    root.level = 0;
    m_octreeNodes.push_back(root);
    m_rootNodeIndex = 0;

    CFFLog::Info("[VolumetricLightmap] Building octree...");

    // 递归构建
    buildOctreeRecursive(0, 0, scene);

    // 更新派生参数
    m_derived.actualBrickCount = (int)m_bricks.size();

    // 计算 Atlas 尺寸
    computeAtlasSize();

    // 为每个 Brick 分配 Atlas 位置
    m_atlasNextX = 0;
    m_atlasNextY = 0;
    m_atlasNextZ = 0;
    for (auto& brick : m_bricks) {
        allocateBrickInAtlas(brick);
    }

    CFFLog::Info("[VolumetricLightmap] Octree built:");
    CFFLog::Info("  Octree Nodes: %d", (int)m_octreeNodes.size());
    CFFLog::Info("  Leaf Bricks: %d", (int)m_bricks.size());
}

void CVolumetricLightmap::buildOctreeRecursive(int nodeIndex, int level, CScene& scene)
{
    SOctreeNode& node = m_octreeNodes[nodeIndex];
    const XMFLOAT3 boundsMin = node.boundsMin;
    const XMFLOAT3 boundsMax = node.boundsMax;

    // 判断是否需要继续细分
    if (shouldSubdivide(boundsMin, boundsMax, level, scene) && level < m_derived.maxLevel)
    {
        // 计算中心点
        XMFLOAT3 center = {
            (boundsMin.x + boundsMax.x) * 0.5f,
            (boundsMin.y + boundsMax.y) * 0.5f,
            (boundsMin.z + boundsMax.z) * 0.5f
        };

        // 创建 8 个子节点
        for (int octant = 0; octant < 8; octant++)
        {
            // 计算子节点边界
            XMFLOAT3 childMin = getChildBoundsMin(boundsMin, boundsMax, octant);
            XMFLOAT3 childMax = getChildBoundsMax(boundsMin, boundsMax, octant);

            // 创建子节点
            SOctreeNode child;
            child.boundsMin = childMin;
            child.boundsMax = childMax;
            child.level = level + 1;

            int childIndex = (int)m_octreeNodes.size();
            m_octreeNodes.push_back(child);

            // 必须在 push_back 之后更新父节点引用（因为 vector 可能重新分配）
            m_octreeNodes[nodeIndex].children[octant] = childIndex;

            // 递归构建子节点
            buildOctreeRecursive(childIndex, level + 1, scene);
        }
    }
    else
    {
        // 叶子节点：创建 Brick
        int brickIndex = createBrick(boundsMin, boundsMax, level);
        m_octreeNodes[nodeIndex].brickIndex = brickIndex;
    }
}

bool CVolumetricLightmap::shouldSubdivide(
    const XMFLOAT3& boundsMin,
    const XMFLOAT3& boundsMax,
    int currentLevel,
    CScene& scene)
{
    // 条件 1：检查当前 Brick 尺寸是否大于最小尺寸
    float brickSizeX = boundsMax.x - boundsMin.x;
    float brickSizeY = boundsMax.y - boundsMin.y;
    float brickSizeZ = boundsMax.z - boundsMin.z;
    float minBrickSize = std::min({brickSizeX, brickSizeY, brickSizeZ});

    // 如果细分后会小于最小尺寸，不细分
    if (minBrickSize / 2.0f < m_config.minBrickWorldSize) {
        return false;
    }

    // 条件 2：检查区域内是否有几何体
    bool hasGeometry = checkGeometryInBounds(boundsMin, boundsMax, scene);

    // 如果没有几何体，也创建一个 Brick（用于空间连续性）
    // 但不继续细分
    if (!hasGeometry) {
        return false;
    }

    // 有几何体，继续细分
    return true;
}

bool CVolumetricLightmap::checkGeometryInBounds(
    const XMFLOAT3& boundsMin,
    const XMFLOAT3& boundsMax,
    CScene& scene)
{
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
            // 如果无法获取 bounds，使用物体位置作为点进行检测
            const auto& pos = transform->position;
            if (pos.x >= boundsMin.x && pos.x <= boundsMax.x &&
                pos.y >= boundsMin.y && pos.y <= boundsMax.y &&
                pos.z >= boundsMin.z && pos.z <= boundsMax.z)
            {
                return true;
            }
            continue;
        }

        // 将局部 AABB 的 8 个顶点变换到世界空间，计算世界空间 AABB
        XMMATRIX worldMatrix = transform->WorldMatrix();

        // 局部 AABB 的 8 个顶点
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

        // 变换到世界空间并计算世界 AABB
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

        // AABB-AABB 相交检测
        bool intersects =
            worldMin.x <= boundsMax.x && worldMax.x >= boundsMin.x &&
            worldMin.y <= boundsMax.y && worldMax.y >= boundsMin.y &&
            worldMin.z <= boundsMax.z && worldMax.z >= boundsMin.z;

        if (intersects) {
            return true;
        }
    }

    return false;
}

// ============================================
// Brick 管理
// ============================================

int CVolumetricLightmap::createBrick(
    const XMFLOAT3& boundsMin,
    const XMFLOAT3& boundsMax,
    int level)
{
    SBrick brick;
    brick.worldMin = boundsMin;
    brick.worldMax = boundsMax;
    brick.level = level;

    // 计算在八叉树中的位置
    float cellSize = m_derived.rootBrickSize / (float)(1 << level);
    if (cellSize > 0) {
        brick.treeX = (int)((boundsMin.x - m_config.volumeMin.x) / cellSize);
        brick.treeY = (int)((boundsMin.y - m_config.volumeMin.y) / cellSize);
        brick.treeZ = (int)((boundsMin.z - m_config.volumeMin.z) / cellSize);
    }

    int brickIndex = (int)m_bricks.size();
    m_bricks.push_back(brick);

    return brickIndex;
}

bool CVolumetricLightmap::allocateBrickInAtlas(SBrick& brick)
{
    if (m_atlasBricksPerSide == 0) {
        CFFLog::Error("[VolumetricLightmap] Atlas not computed!");
        return false;
    }

    // 分配位置
    brick.atlasX = m_atlasNextX;
    brick.atlasY = m_atlasNextY;
    brick.atlasZ = m_atlasNextZ;

    // 移动到下一个位置
    m_atlasNextX++;
    if (m_atlasNextX >= m_atlasBricksPerSide) {
        m_atlasNextX = 0;
        m_atlasNextY++;
        if (m_atlasNextY >= m_atlasBricksPerSide) {
            m_atlasNextY = 0;
            m_atlasNextZ++;
            if (m_atlasNextZ >= m_atlasBricksPerSide) {
                CFFLog::Error("[VolumetricLightmap] Atlas overflow!");
                return false;
            }
        }
    }

    return true;
}

// ============================================
// 烘焙
// ============================================

bool CVolumetricLightmap::IsDXRBakingAvailable() const
{
    auto* ctx = RHI::CRHIManager::Instance().GetRenderContext();
    return ctx && ctx->SupportsRaytracing();
}

void CVolumetricLightmap::BakeAllBricks(CScene& scene, const SLightmapBakeConfig& config)
{
    if (m_bricks.empty()) {
        CFFLog::Warning("[VolumetricLightmap] No bricks to bake! Call BuildOctree first.");
        return;
    }
    // Determine which backend to use
    ELightmapBakeBackend backend = config.backend;

    // Auto-fallback if DXR requested but not available
    if (backend == ELightmapBakeBackend::GPU_DXR && !IsDXRBakingAvailable()) {
        CFFLog::Warning("[VolumetricLightmap] DXR not supported on this device, falling back to CPU");
        backend = ELightmapBakeBackend::CPU;
    }

    // Dispatch to appropriate backend
    if (backend == ELightmapBakeBackend::GPU_DXR) {
        bakeWithGPU(scene, config);
    } else {
        bakeWithCPU(scene, config);
    }

    // Apply dilation to fill invalid probes with data from nearby valid probes
    //dilateInvalidProbes();
}

void CVolumetricLightmap::bakeWithCPU(CScene& scene, const SLightmapBakeConfig& config)
{
    if (m_bricks.empty()) {
        CFFLog::Warning("[VolumetricLightmap] No bricks to bake! Call BuildOctree first.");
        return;
    }
    // 创建 Path Trace Baker
    SPathTraceConfig ptConfig;
    ptConfig.samplesPerVoxel = config.cpuSamplesPerVoxel;
    ptConfig.maxBounces = config.cpuMaxBounces;
    ptConfig.useRussianRoulette = true;

    CPathTraceBaker baker;
    if (!baker.Initialize(scene, ptConfig)) {
        CFFLog::Error("[VolumetricLightmap] Failed to initialize PathTraceBaker!");
        return;
    }

    int totalBricks = (int)m_bricks.size();
    int totalVoxels = totalBricks * VL_BRICK_VOXEL_COUNT;

    // 计算合适的进度打印间隔
    int progressInterval = std::max(1, totalBricks / 20);

    CFFLog::Info("[VolumetricLightmap] ========================================");
    CFFLog::Info("[VolumetricLightmap] Starting CPU Path Trace bake...");
    CFFLog::Info("[VolumetricLightmap]   Total Bricks: %d", totalBricks);
    CFFLog::Info("[VolumetricLightmap]   Total Voxels: %d (%d per brick)", totalVoxels, VL_BRICK_VOXEL_COUNT);
    CFFLog::Info("[VolumetricLightmap]   Samples per voxel: %d", ptConfig.samplesPerVoxel);
    CFFLog::Info("[VolumetricLightmap]   Max bounces: %d", ptConfig.maxBounces);
    CFFLog::Info("[VolumetricLightmap]   Volume: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)",
                 m_config.volumeMin.x, m_config.volumeMin.y, m_config.volumeMin.z,
                 m_config.volumeMax.x, m_config.volumeMax.y, m_config.volumeMax.z);
    CFFLog::Info("[VolumetricLightmap] ========================================");

    auto startTime = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < m_bricks.size(); i++)
    {
        bakeBrick(m_bricks[i], scene, baker);

        // Progress callback
        if (config.progressCallback) {
            float progress = static_cast<float>(i + 1) / static_cast<float>(totalBricks);
            config.progressCallback(progress);
        }

        // 进度日志
        bool shouldPrint = (i + 1) % progressInterval == 0 || i == m_bricks.size() - 1;
        if (shouldPrint)
        {
            auto now = std::chrono::high_resolution_clock::now();
            float elapsedSec = std::chrono::duration<float>(now - startTime).count();
            float progressPercent = 100.0f * (i + 1) / totalBricks;

            float estimatedTotalTime = (elapsedSec / (i + 1)) * totalBricks;
            float remainingSec = estimatedTotalTime - elapsedSec;

            CFFLog::Info("[VolumetricLightmap] Progress: %d/%d bricks (%.1f%%) | Elapsed: %.1fs | ETA: %.1fs",
                         (int)(i + 1), totalBricks, progressPercent, elapsedSec, remainingSec);
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float totalElapsedSec = std::chrono::duration<float>(endTime - startTime).count();

    baker.Shutdown();

    CFFLog::Info("[VolumetricLightmap] ========================================");
    CFFLog::Info("[VolumetricLightmap] CPU Path Trace bake complete!");
    CFFLog::Info("[VolumetricLightmap]   Bricks baked: %d", totalBricks);
    CFFLog::Info("[VolumetricLightmap]   Voxels baked: %d", totalVoxels);
    CFFLog::Info("[VolumetricLightmap]   Total time: %.2f seconds", totalElapsedSec);
    CFFLog::Info("[VolumetricLightmap]   Avg per brick: %.3f ms", (totalElapsedSec * 1000.0f) / totalBricks);
    CFFLog::Info("[VolumetricLightmap]   Avg per voxel: %.3f ms", (totalElapsedSec * 1000.0f) / totalVoxels);
    CFFLog::Info("[VolumetricLightmap] ========================================");
}

void CVolumetricLightmap::bakeWithGPU(CScene& scene, const SLightmapBakeConfig& config)
{
    CFFLog::Info("[VolumetricLightmap] ========================================");
    CFFLog::Info("[VolumetricLightmap] Starting GPU DXR cubemap bake...");
    CFFLog::Info("[VolumetricLightmap]   Cubemap resolution: %dx%dx6", CUBEMAP_BAKE_RES, CUBEMAP_BAKE_RES);
    CFFLog::Info("[VolumetricLightmap]   Rays per voxel: %d", CUBEMAP_TOTAL_PIXELS);
    CFFLog::Info("[VolumetricLightmap]   Max bounces: %d", config.gpuMaxBounces);
    CFFLog::Info("[VolumetricLightmap] ========================================");

    // Lazy initialize DXR baker
    if (!m_dxrBaker) {
        m_dxrBaker = std::make_unique<CDXRCubemapBaker>();
    }

    if (!m_dxrBaker->IsReady()) {
        if (!m_dxrBaker->Initialize()) {
            CFFLog::Error("[VolumetricLightmap] Failed to initialize DXR baker");
            return;
        }
    }

    // Configure DXR cubemap bake
    SDXRCubemapBakeConfig dxrConfig;
    dxrConfig.cubemapResolution = CUBEMAP_BAKE_RES;
    dxrConfig.maxBounces = config.gpuMaxBounces;
    dxrConfig.skyIntensity = config.gpuSkyIntensity;
    dxrConfig.progressCallback = config.progressCallback;

    // Run DXR bake
    if (!m_dxrBaker->BakeVolumetricLightmap(*this, scene, dxrConfig)) {
        CFFLog::Error("[VolumetricLightmap] DXR bake failed");
        return;
    }

    // Phase 2: Dispatch bake for all voxels
    CFFLog::Info("[VolumetricLightmap] Rays per voxel: %u (32x32x6)",
                 dxrConfig.cubemapResolution * dxrConfig.cubemapResolution * 6);

    bool success = m_dxrBaker->DispatchBakeAllVoxels(*this, dxrConfig);

    if (!success) {
        CFFLog::Error("[VolumetricLightmap] GPU bake dispatch failed");
        return;
    }
    CFFLog::Info("[VolumetricLightmap] ========================================");
    CFFLog::Info("[VolumetricLightmap] GPU DXR cubemap bake complete!");
    CFFLog::Info("[VolumetricLightmap] ========================================");
}

void CVolumetricLightmap::bakeBrick(SBrick& brick, CScene& scene, CPathTraceBaker& baker)
{
    XMFLOAT3 brickSize = {
        brick.worldMax.x - brick.worldMin.x,
        brick.worldMax.y - brick.worldMin.y,
        brick.worldMax.z - brick.worldMin.z
    };

    // 对每个体素位置烘焙 SH
    // 使用 Overlap Baking：voxel[0] brick 边缘，voxel[3] 在另一边缘
    // 这样相邻 Brick 的边缘体素采样同一个世界位置，实现 C0 连续性
    for (int z = 0; z < VL_BRICK_SIZE; z++)
    for (int y = 0; y < VL_BRICK_SIZE; y++)
    for (int x = 0; x < VL_BRICK_SIZE; x++)
    {
        // Overlap Baking: 体素位置从边缘到边缘
        // voxel[0] -> t=0.0 (brick.worldMin)
        // voxel[3] -> t=1.0 (brick.worldMax)
        // 中间体素均匀分布
        float tx = (float)x / (VL_BRICK_SIZE - 1);  // 0, 1/3, 2/3, 1
        float ty = (float)y / (VL_BRICK_SIZE - 1);
        float tz = (float)z / (VL_BRICK_SIZE - 1);

        XMFLOAT3 voxelPos = {
            brick.worldMin.x + tx * brickSize.x,
            brick.worldMin.y + ty * brickSize.y,
            brick.worldMin.z + tz * brickSize.z
        };

        // 使用 Path Tracing 烘焙 SH，并获取 validity
        SBakeResult result = baker.BakeVoxelWithValidity(voxelPos, scene);

        // 存储 SH 系数和 validity
        int voxelIndex = SBrick::VoxelIndex(x, y, z);
        for (int c = 0; c < VL_SH_COEFF_COUNT; c++) {
            brick.shData[voxelIndex][c] = result.sh[c];
        }
        brick.validity[voxelIndex] = result.isValid;
    }
}

// ============================================
// Probe Dilation (leak prevention)
// ============================================

XMFLOAT3 CVolumetricLightmap::getVoxelWorldPosition(const SBrick& brick, int voxelX, int voxelY, int voxelZ) const
{
    XMFLOAT3 brickSize = {
        brick.worldMax.x - brick.worldMin.x,
        brick.worldMax.y - brick.worldMin.y,
        brick.worldMax.z - brick.worldMin.z
    };

    float tx = (float)voxelX / (VL_BRICK_SIZE - 1);
    float ty = (float)voxelY / (VL_BRICK_SIZE - 1);
    float tz = (float)voxelZ / (VL_BRICK_SIZE - 1);

    return {
        brick.worldMin.x + tx * brickSize.x,
        brick.worldMin.y + ty * brickSize.y,
        brick.worldMin.z + tz * brickSize.z
    };
}

int CVolumetricLightmap::findNearestValidVoxel(int brickIdx, int voxelIdx, int searchRadius) const
{
    const SBrick& brick = m_bricks[brickIdx];
    int vx, vy, vz;
    SBrick::IndexToVoxel(voxelIdx, vx, vy, vz);
    XMFLOAT3 invalidPos = getVoxelWorldPosition(brick, vx, vy, vz);

    float bestDistSq = FLT_MAX;
    int bestBrick = -1;
    int bestVoxel = -1;

    // Search within the same brick first
    for (int z = 0; z < VL_BRICK_SIZE; z++)
    for (int y = 0; y < VL_BRICK_SIZE; y++)
    for (int x = 0; x < VL_BRICK_SIZE; x++)
    {
        int idx = SBrick::VoxelIndex(x, y, z);
        if (!brick.validity[idx]) continue;  // Skip invalid

        XMFLOAT3 pos = getVoxelWorldPosition(brick, x, y, z);
        float dx = pos.x - invalidPos.x;
        float dy = pos.y - invalidPos.y;
        float dz = pos.z - invalidPos.z;
        float distSq = dx*dx + dy*dy + dz*dz;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestBrick = brickIdx;
            bestVoxel = idx;
        }
    }

    // If found in same brick, return
    if (bestVoxel >= 0) {
        return bestBrick * VL_BRICK_VOXEL_COUNT + bestVoxel;
    }

    // Search neighboring bricks
    for (size_t bi = 0; bi < m_bricks.size(); bi++)
    {
        if ((int)bi == brickIdx) continue;

        const SBrick& other = m_bricks[bi];

        // Quick distance check using brick centers
        float centerX = (other.worldMin.x + other.worldMax.x) * 0.5f;
        float centerY = (other.worldMin.y + other.worldMax.y) * 0.5f;
        float centerZ = (other.worldMin.z + other.worldMax.z) * 0.5f;
        float dcx = centerX - invalidPos.x;
        float dcy = centerY - invalidPos.y;
        float dcz = centerZ - invalidPos.z;
        float centerDistSq = dcx*dcx + dcy*dcy + dcz*dcz;

        // Skip if too far (heuristic: max brick diagonal ~ sqrt(3) * brickSize)
        float brickSize = other.worldMax.x - other.worldMin.x;
        float maxSearchDist = brickSize * (float)searchRadius * 2.0f;
        if (centerDistSq > maxSearchDist * maxSearchDist) continue;

        for (int z = 0; z < VL_BRICK_SIZE; z++)
        for (int y = 0; y < VL_BRICK_SIZE; y++)
        for (int x = 0; x < VL_BRICK_SIZE; x++)
        {
            int idx = SBrick::VoxelIndex(x, y, z);
            if (!other.validity[idx]) continue;

            XMFLOAT3 pos = getVoxelWorldPosition(other, x, y, z);
            float dx = pos.x - invalidPos.x;
            float dy = pos.y - invalidPos.y;
            float dz = pos.z - invalidPos.z;
            float distSq = dx*dx + dy*dy + dz*dz;

            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestBrick = (int)bi;
                bestVoxel = idx;
            }
        }
    }

    if (bestVoxel >= 0) {
        return bestBrick * VL_BRICK_VOXEL_COUNT + bestVoxel;
    }

    return -1;  // No valid voxel found
}

void CVolumetricLightmap::dilateInvalidProbes()
{
    CFFLog::Info("[VolumetricLightmap] Starting probe dilation...");

    int totalInvalid = 0;
    int totalDilated = 0;

    // Count invalid probes
    for (const auto& brick : m_bricks) {
        for (int i = 0; i < VL_BRICK_VOXEL_COUNT; i++) {
            if (!brick.validity[i]) totalInvalid++;
        }
    }

    CFFLog::Info("[VolumetricLightmap]   Invalid probes before dilation: %d", totalInvalid);

    if (totalInvalid == 0) {
        CFFLog::Info("[VolumetricLightmap]   No invalid probes, dilation skipped.");
        return;
    }

    const int searchRadius = 3;  // Search within 3 brick-widths

    // For each brick and each invalid voxel, find nearest valid and copy SH
    for (size_t bi = 0; bi < m_bricks.size(); bi++)
    {
        SBrick& brick = m_bricks[bi];

        for (int vi = 0; vi < VL_BRICK_VOXEL_COUNT; vi++)
        {
            if (brick.validity[vi]) continue;  // Skip valid probes

            // Find nearest valid voxel
            int packedResult = findNearestValidVoxel((int)bi, vi, searchRadius);

            if (packedResult >= 0) {
                int srcBrick = packedResult / VL_BRICK_VOXEL_COUNT;
                int srcVoxel = packedResult % VL_BRICK_VOXEL_COUNT;

                // Copy SH data from valid source
                const SBrick& srcBrickRef = m_bricks[srcBrick];
                for (int c = 0; c < VL_SH_COEFF_COUNT; c++) {
                    brick.shData[vi][c] = srcBrickRef.shData[srcVoxel][c];
                }

                // Mark as valid after dilation (so it can be used by GPU)
                brick.validity[vi] = true;
                totalDilated++;
            }
        }
    }

    CFFLog::Info("[VolumetricLightmap]   Probes dilated: %d / %d", totalDilated, totalInvalid);
    CFFLog::Info("[VolumetricLightmap] Probe dilation complete!");
}


// ============================================
// GPU 数据构建
// ============================================

void CVolumetricLightmap::buildIndirectionData()
{
    int res = m_derived.indirectionResolution;
    m_indirectionData.resize(res * res * res);

    XMFLOAT3 volumeSize = {
        m_config.volumeMax.x - m_config.volumeMin.x,
        m_config.volumeMax.y - m_config.volumeMin.y,
        m_config.volumeMax.z - m_config.volumeMin.z
    };

    // 对每个 Indirection 体素，查找对应的 Brick
    for (int z = 0; z < res; z++)
    for (int y = 0; y < res; y++)
    for (int x = 0; x < res; x++)
    {
        // 计算世界坐标（体素中心）
        float tx = (x + 0.5f) / res;
        float ty = (y + 0.5f) / res;
        float tz = (z + 0.5f) / res;

        XMFLOAT3 worldPos = {
            m_config.volumeMin.x + tx * volumeSize.x,
            m_config.volumeMin.y + ty * volumeSize.y,
            m_config.volumeMin.z + tz * volumeSize.z
        };

        // 查找包含该点的 Brick
        int brickIndex = findBrickAtPosition(worldPos);

        // 打包数据
        SIndirectionEntry entry;
        if (brickIndex >= 0) {
            entry.brickIndex = (uint16_t)brickIndex;
            entry.level = (uint8_t)m_bricks[brickIndex].level;
        } else {
            CFFLog::Error("[VolumetricLightmap] brickIndex error!");
            entry = SIndirectionEntry::Invalid();
        }
        entry.padding = 0;

        int idx = x + y * res + z * res * res;
        m_indirectionData[idx] = entry.Pack();
    }
}

int CVolumetricLightmap::findBrickAtPosition(const XMFLOAT3& worldPos)
{
    // 从根节点开始遍历八叉树
    if (m_rootNodeIndex < 0 || m_octreeNodes.empty()) {
        return -1;
    }

    int nodeIndex = m_rootNodeIndex;

    while (nodeIndex >= 0 && nodeIndex < (int)m_octreeNodes.size())
    {
        const SOctreeNode& node = m_octreeNodes[nodeIndex];

        // 如果是叶子节点，返回 Brick 索引
        if (node.IsLeaf()) {
            return node.brickIndex;
        }

        // 如果没有子节点（不应该发生），返回 -1
        if (!node.HasChildren()) {
            return -1;
        }

        // 计算应该进入哪个子节点
        XMFLOAT3 center = {
            (node.boundsMin.x + node.boundsMax.x) * 0.5f,
            (node.boundsMin.y + node.boundsMax.y) * 0.5f,
            (node.boundsMin.z + node.boundsMax.z) * 0.5f
        };

        int octant = 0;
        if (worldPos.x >= center.x) octant |= 1;
        if (worldPos.y >= center.y) octant |= 2;
        if (worldPos.z >= center.z) octant |= 4;

        nodeIndex = node.children[octant];
    }

    return -1;
}

void CVolumetricLightmap::packSHToAtlas()
{
    if (m_derived.brickAtlasSize == 0) {
        CFFLog::Error("[VolumetricLightmap] Atlas size is 0!");
        return;
    }

    int atlasSize = m_derived.brickAtlasSize;
    int totalVoxels = atlasSize * atlasSize * atlasSize;

    // 初始化 Atlas 数据（全零）
    m_brickAtlasSH0.resize(totalVoxels, {0, 0, 0, 0});
    m_brickAtlasSH1.resize(totalVoxels, {0, 0, 0, 0});
    m_brickAtlasSH2.resize(totalVoxels, {0, 0, 0, 0});

    // 构建 Brick Info
    m_brickInfoData.resize(m_bricks.size());

    // 打包每个 Brick 的 SH 数据到 Atlas
    for (size_t bi = 0; bi < m_bricks.size(); bi++)
    {
        const SBrick& brick = m_bricks[bi];

        // Brick 在 Atlas 中的起始体素坐标
        int atlasBaseX = brick.atlasX * VL_BRICK_SIZE;
        int atlasBaseY = brick.atlasY * VL_BRICK_SIZE;
        int atlasBaseZ = brick.atlasZ * VL_BRICK_SIZE;

        // 填充 Brick Info
        m_brickInfoData[bi].worldMin = brick.worldMin;
        m_brickInfoData[bi].worldMax = brick.worldMax;
        m_brickInfoData[bi].atlasOffset = {
            (float)atlasBaseX,
            (float)atlasBaseY,
            (float)atlasBaseZ
        };

        // 填充 SH 数据
        for (int vz = 0; vz < VL_BRICK_SIZE; vz++)
        for (int vy = 0; vy < VL_BRICK_SIZE; vy++)
        for (int vx = 0; vx < VL_BRICK_SIZE; vx++)
        {
            int voxelIndex = SBrick::VoxelIndex(vx, vy, vz);
            const auto& sh = brick.shData[voxelIndex];

            // Atlas 中的坐标
            int ax = atlasBaseX + vx;
            int ay = atlasBaseY + vy;
            int az = atlasBaseZ + vz;
            int atlasIdx = ax + ay * atlasSize + az * atlasSize * atlasSize;

            // 打包 SH 系数到 3 张纹理
            // SH0: L0 (RGB) + L1[0].R
            // SH1: L1[0].GB + L1[1].RG
            // SH2: L1[1].B + L1[2].RGB
            // 注意：这是简化的 L1 打包，完整版需要 L2

            // 完整 L2 打包方案：
            // SH0: sh[0].RGB, sh[1].R
            // SH1: sh[1].GB, sh[2].RG
            // SH2: sh[2].B, sh[3].RGB
            // ... 需要更多纹理或更紧凑的打包

            // 简化版：只打包前 4 个系数（L0 + L1 的部分）
            m_brickAtlasSH0[atlasIdx] = {sh[0].x, sh[0].y, sh[0].z, sh[1].x};
            m_brickAtlasSH1[atlasIdx] = {sh[1].y, sh[1].z, sh[2].x, sh[2].y};
            m_brickAtlasSH2[atlasIdx] = {sh[2].z, sh[3].x, sh[3].y, sh[3].z};
        }
    }
}

// ============================================
// GPU 资源创建
// ============================================

bool CVolumetricLightmap::CreateGPUResources()
{
    if (m_bricks.empty()) {
        CFFLog::Warning("[VolumetricLightmap] No bricks! Cannot create GPU resources.");
        return false;
    }

    auto* renderContext = RHI::CRHIManager::Instance().GetRenderContext();
    if (!renderContext) {
        CFFLog::Error("[VolumetricLightmap] No render context!");
        return false;
    }

    CFFLog::Info("[VolumetricLightmap] Creating GPU resources...");

    // 构建 CPU 侧数据
    buildIndirectionData();
    packSHToAtlas();

    // ============================================
    // 1. 创建 Indirection Texture (3D)
    // ============================================
    {
        int res = m_derived.indirectionResolution;

        RHI::TextureDesc texDesc;
        texDesc.width = res;
        texDesc.height = res;
        texDesc.depth = res;
        texDesc.mipLevels = 1;
        texDesc.format = RHI::ETextureFormat::R32_UINT;
        texDesc.usage = RHI::ETextureUsage::ShaderResource;
        texDesc.dimension = RHI::ETextureDimension::Tex3D;
        texDesc.debugName = "VolumetricLightmap_Indirection";

        m_indirectionTexture.reset(renderContext->CreateTexture(texDesc, m_indirectionData.data()));
        if (!m_indirectionTexture) {
            CFFLog::Error("[VolumetricLightmap] Failed to create Indirection texture!");
            return false;
        }

        CFFLog::Info("  Indirection Texture: %dx%dx%d (R32_UINT)", res, res, res);
    }

    // ============================================
    // 2. 创建 Brick Atlas Textures (3D)
    // ============================================
    {
        int atlasSize = m_derived.brickAtlasSize;

        const std::vector<XMFLOAT4>* atlasData[3] = {
            &m_brickAtlasSH0,
            &m_brickAtlasSH1,
            &m_brickAtlasSH2
        };

        for (int i = 0; i < 3; i++)
        {
            // 转换为 half float
            std::vector<uint16_t> halfData(atlasSize * atlasSize * atlasSize * 4);
            for (size_t j = 0; j < atlasData[i]->size(); j++) {
                const XMFLOAT4& v = (*atlasData[i])[j];
                XMHALF4 h;
                XMStoreHalf4(&h, XMLoadFloat4(&v));
                halfData[j * 4 + 0] = h.x;
                halfData[j * 4 + 1] = h.y;
                halfData[j * 4 + 2] = h.z;
                halfData[j * 4 + 3] = h.w;
            }

            RHI::TextureDesc texDesc;
            texDesc.width = atlasSize;
            texDesc.height = atlasSize;
            texDesc.depth = atlasSize;
            texDesc.mipLevels = 1;
            texDesc.format = RHI::ETextureFormat::R16G16B16A16_FLOAT;
            texDesc.usage = RHI::ETextureUsage::ShaderResource;
            texDesc.dimension = RHI::ETextureDimension::Tex3D;
            texDesc.debugName = "VolumetricLightmap_BrickAtlas";

            m_brickAtlasTexture[i].reset(renderContext->CreateTexture(texDesc, halfData.data()));
            if (!m_brickAtlasTexture[i]) {
                CFFLog::Error("[VolumetricLightmap] Failed to create Atlas texture %d!", i);
                return false;
            }
        }

        CFFLog::Info("  Atlas Textures: %dx%dx%d x3 (R16G16B16A16_FLOAT)", atlasSize, atlasSize, atlasSize);
    }

    // ============================================
    // 3. 创建 Constant Buffer
    // ============================================
    {
        RHI::BufferDesc bufDesc;
        bufDesc.size = sizeof(CB_VolumetricLightmap);
        bufDesc.usage = RHI::EBufferUsage::Constant;
        bufDesc.cpuAccess = RHI::ECPUAccess::Write;
        bufDesc.debugName = "VolumetricLightmap_CB";

        m_constantBuffer.reset(renderContext->CreateBuffer(bufDesc, nullptr));
        if (!m_constantBuffer) {
            CFFLog::Error("[VolumetricLightmap] Failed to create constant buffer!");
            return false;
        }
    }

    // ============================================
    // 4. 创建 Brick Info Buffer (Structured Buffer)
    // ============================================
    {
        RHI::BufferDesc bufDesc;
        bufDesc.size = (uint32_t)(m_brickInfoData.size() * sizeof(SBrickInfo));
        bufDesc.usage = RHI::EBufferUsage::Structured | RHI::EBufferUsage::UnorderedAccess;
        bufDesc.cpuAccess = RHI::ECPUAccess::None;
        bufDesc.structureByteStride = sizeof(SBrickInfo);
        bufDesc.debugName = "VolumetricLightmap_BrickInfo";

        m_brickInfoBuffer.reset(renderContext->CreateBuffer(bufDesc, m_brickInfoData.data()));
        if (!m_brickInfoBuffer) {
            CFFLog::Error("[VolumetricLightmap] Failed to create Brick Info buffer!");
            return false;
        }

        CFFLog::Info("  Brick Info Buffer: %d entries", (int)m_brickInfoData.size());
    }

    // ============================================
    // 5. 创建 Sampler State (s3: trilinear for atlas)
    // ============================================
    {
        RHI::SamplerDesc samplerDesc;
        samplerDesc.filter = RHI::EFilter::MinMagMipLinear;
        samplerDesc.addressU = RHI::ETextureAddressMode::Clamp;
        samplerDesc.addressV = RHI::ETextureAddressMode::Clamp;
        samplerDesc.addressW = RHI::ETextureAddressMode::Clamp;

        m_sampler.reset(renderContext->CreateSampler(samplerDesc));
        if (!m_sampler) {
            CFFLog::Error("[VolumetricLightmap] Failed to create sampler state!");
            return false;
        }
    }

    m_gpuResourcesCreated = true;
    CFFLog::Info("[VolumetricLightmap] GPU resources created successfully!");

    return true;
}

void CVolumetricLightmap::UploadToGPU()
{
    // 目前 CreateGPUResources 已经上传了初始数据
    // 这个函数保留用于将来的动态更新
}

// ============================================
// Legacy Binding (DX11 compatibility)
// ============================================
#ifndef FF_LEGACY_BINDING_DISABLED

void CVolumetricLightmap::Bind(RHI::ICommandList* cmdList)
{
    static bool s_warnedLegacy = false;
    if (!s_warnedLegacy) {
        CFFLog::Warning("[VolumetricLightmap] Using legacy binding path. Consider migrating to descriptor sets.");
        s_warnedLegacy = true;
    }

    // 如果未启用或未烘焙，绑定禁用状态的 CB 和空 SRV
    if (!m_enabled || !m_gpuResourcesCreated || m_bricks.empty()) {
        // Must still bind CB with enabled=0 so shader can check the flag
        // Otherwise DX12 GPU validation will report uninitialized root argument
        CB_VolumetricLightmap cb = {};
        cb.enabled = 0;
        cb.brickCount = 0;
        cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 6, &cb, sizeof(CB_VolumetricLightmap));

        // 绑定空 SRV，避免 RTV/SRV 资源冲突
        cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 11, nullptr);
        cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 12, nullptr);
        cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 13, nullptr);
        cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 14, nullptr);
        cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Pixel, 15, nullptr);
        cmdList->SetSampler(RHI::EShaderStage::Pixel, 2, m_sampler.get());
        return;
    }

    // Update and bind CB to b6 using SetConstantBufferData for DX12 compatibility
    CB_VolumetricLightmap cb = {};
    cb.volumeMin = m_config.volumeMin;
    cb.volumeMax = m_config.volumeMax;

    float invSizeX = 1.0f / (m_config.volumeMax.x - m_config.volumeMin.x);
    float invSizeY = 1.0f / (m_config.volumeMax.y - m_config.volumeMin.y);
    float invSizeZ = 1.0f / (m_config.volumeMax.z - m_config.volumeMin.z);
    cb.volumeInvSize = {invSizeX, invSizeY, invSizeZ};

    float indirInv = 1.0f / m_derived.indirectionResolution;
    cb.indirectionInvSize = {indirInv, indirInv, indirInv};

    float atlasInv = 1.0f / m_derived.brickAtlasSize;
    cb.brickAtlasInvSize = {atlasInv, atlasInv, atlasInv};

    cb.indirectionResolution = m_derived.indirectionResolution;
    cb.brickAtlasSize = m_derived.brickAtlasSize;
    cb.maxLevel = m_derived.maxLevel;
    cb.enabled = 1;
    cb.brickCount = (int)m_bricks.size();

    cmdList->SetConstantBufferData(RHI::EShaderStage::Pixel, 6, &cb, sizeof(CB_VolumetricLightmap));

    // 绑定纹理资源 (t11-t15)
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 11, m_indirectionTexture.get());
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 12, m_brickAtlasTexture[0].get());
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 13, m_brickAtlasTexture[1].get());
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 14, m_brickAtlasTexture[2].get());
    cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Pixel, 15, m_brickInfoBuffer.get());

    // 绑定采样器到 s2
    cmdList->SetSampler(RHI::EShaderStage::Pixel, 2, m_sampler.get());
}

void CVolumetricLightmap::Unbind(RHI::ICommandList* cmdList)
{
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 11, nullptr);
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 12, nullptr);
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 13, nullptr);
    cmdList->SetShaderResource(RHI::EShaderStage::Pixel, 14, nullptr);
    cmdList->SetShaderResourceBuffer(RHI::EShaderStage::Pixel, 15, nullptr);
    // Note: SetConstantBufferData uses per-frame ring buffer, no need to unbind
}

#else // FF_LEGACY_BINDING_DISABLED

void CVolumetricLightmap::Bind(RHI::ICommandList* /*cmdList*/)
{
    CFFLog::Warning("[VolumetricLightmap] Legacy Bind() called but FF_LEGACY_BINDING_DISABLED is defined. Use PopulatePerFrameSet() instead.");
}

void CVolumetricLightmap::Unbind(RHI::ICommandList* /*cmdList*/)
{
    // No-op when legacy binding is disabled
}

#endif // FF_LEGACY_BINDING_DISABLED

void CVolumetricLightmap::PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet)
{
    using namespace PerFrameSlots;

    if (!perFrameSet) return;

    // Build CB data
    CB_VolumetricLightmap cb = {};

    if (m_enabled && m_gpuResourcesCreated && !m_bricks.empty()) {
        cb.volumeMin = m_config.volumeMin;
        cb.volumeMax = m_config.volumeMax;

        float invSizeX = 1.0f / (m_config.volumeMax.x - m_config.volumeMin.x);
        float invSizeY = 1.0f / (m_config.volumeMax.y - m_config.volumeMin.y);
        float invSizeZ = 1.0f / (m_config.volumeMax.z - m_config.volumeMin.z);
        cb.volumeInvSize = {invSizeX, invSizeY, invSizeZ};

        float indirInv = 1.0f / m_derived.indirectionResolution;
        cb.indirectionInvSize = {indirInv, indirInv, indirInv};

        float atlasInv = 1.0f / m_derived.brickAtlasSize;
        cb.brickAtlasInvSize = {atlasInv, atlasInv, atlasInv};

        cb.indirectionResolution = m_derived.indirectionResolution;
        cb.brickAtlasSize = m_derived.brickAtlasSize;
        cb.maxLevel = m_derived.maxLevel;
        cb.enabled = 1;
        cb.brickCount = (int)m_bricks.size();

        // Bind textures to PerFrame set using PerFrameSlots constants
        perFrameSet->Bind({
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_SH_R, m_brickAtlasTexture[0].get()),
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_SH_G, m_brickAtlasTexture[1].get()),
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_SH_B, m_brickAtlasTexture[2].get()),
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_Octree, m_indirectionTexture.get()),
            RHI::BindingSetItem::VolatileCBV(CB::Volumetric, &cb, sizeof(cb))
        });
    } else {
        // Disabled state - still bind CB with enabled=0 and fallback textures
        cb.enabled = 0;
        cb.brickCount = 0;

        // Bind 3D black textures as fallback to ensure all slots are valid
        // VolumetricLightmap textures are Texture3D, so we must use a 3D fallback
        auto* blackTex3D = CTextureManager::Instance().GetDefaultBlack3D().get();
        perFrameSet->Bind({
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_SH_R, blackTex3D),
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_SH_G, blackTex3D),
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_SH_B, blackTex3D),
            RHI::BindingSetItem::Texture_SRV(Tex::Volumetric_Octree, blackTex3D),
            RHI::BindingSetItem::VolatileCBV(CB::Volumetric, &cb, sizeof(cb))
        });
    }
}

// ============================================
// 序列化
// ============================================

bool CVolumetricLightmap::SaveToFile(const std::string& path)
{
    // TODO: 实现二进制序列化
    CFFLog::Warning("[VolumetricLightmap] SaveToFile not implemented yet: %s", path.c_str());
    return false;
}

bool CVolumetricLightmap::LoadFromFile(const std::string& path)
{
    // TODO: 实现二进制反序列化
    CFFLog::Warning("[VolumetricLightmap] LoadFromFile not implemented yet: %s", path.c_str());
    return false;
}

// ============================================
// 工具函数
// ============================================

int CVolumetricLightmap::nextPowerOf2(int n)
{
    if (n <= 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

XMFLOAT3 CVolumetricLightmap::getChildBoundsMin(
    const XMFLOAT3& parentMin,
    const XMFLOAT3& parentMax,
    int octant)
{
    XMFLOAT3 center = {
        (parentMin.x + parentMax.x) * 0.5f,
        (parentMin.y + parentMax.y) * 0.5f,
        (parentMin.z + parentMax.z) * 0.5f
    };

    return {
        (octant & 1) ? center.x : parentMin.x,
        (octant & 2) ? center.y : parentMin.y,
        (octant & 4) ? center.z : parentMin.z
    };
}

XMFLOAT3 CVolumetricLightmap::getChildBoundsMax(
    const XMFLOAT3& parentMin,
    const XMFLOAT3& parentMax,
    int octant)
{
    XMFLOAT3 center = {
        (parentMin.x + parentMax.x) * 0.5f,
        (parentMin.y + parentMax.y) * 0.5f,
        (parentMin.z + parentMax.z) * 0.5f
    };

    return {
        (octant & 1) ? parentMax.x : center.x,
        (octant & 2) ? parentMax.y : center.y,
        (octant & 4) ? parentMax.z : center.z
    };
}

// ============================================
// 调试可视化
// ============================================

#include "DebugLinePass.h"

void CVolumetricLightmap::DrawOctreeDebug(CDebugLinePass& linePass) const
{
    if (!m_debugDrawEnabled || m_octreeNodes.empty()) {
        return;
    }

    // 定义不同级别的颜色
    const XMFLOAT4 levelColors[] = {
        {1.0f, 0.0f, 0.0f, 1.0f},  // Level 0: Red
        {1.0f, 0.5f, 0.0f, 1.0f},  // Level 1: Orange
        {1.0f, 1.0f, 0.0f, 1.0f},  // Level 2: Yellow
        {0.0f, 1.0f, 0.0f, 1.0f},  // Level 3: Green
        {0.0f, 1.0f, 1.0f, 1.0f},  // Level 4: Cyan
        {0.0f, 0.0f, 1.0f, 1.0f},  // Level 5: Blue
        {0.5f, 0.0f, 1.0f, 1.0f},  // Level 6: Purple
        {1.0f, 0.0f, 1.0f, 1.0f},  // Level 7: Magenta
    };
    const int numColors = sizeof(levelColors) / sizeof(levelColors[0]);

    // 遍历所有八叉树节点，只绘制叶子节点（有 Brick 的节点）
    for (const auto& node : m_octreeNodes)
    {
        // 只绘制叶子节点
        if (!node.IsLeaf()) {
            continue;
        }

        // 选择颜色（根据级别）
        int colorIndex = node.level % numColors;
        XMFLOAT4 color = levelColors[colorIndex];

        // 获取 AABB 边界
        const XMFLOAT3& minP = node.boundsMin;
        const XMFLOAT3& maxP = node.boundsMax;

        // 绘制 AABB 的 12 条边
        // 底面 4 条边
        linePass.AddLine({minP.x, minP.y, minP.z}, {maxP.x, minP.y, minP.z}, color);
        linePass.AddLine({maxP.x, minP.y, minP.z}, {maxP.x, minP.y, maxP.z}, color);
        linePass.AddLine({maxP.x, minP.y, maxP.z}, {minP.x, minP.y, maxP.z}, color);
        linePass.AddLine({minP.x, minP.y, maxP.z}, {minP.x, minP.y, minP.z}, color);

        // 顶面 4 条边
        linePass.AddLine({minP.x, maxP.y, minP.z}, {maxP.x, maxP.y, minP.z}, color);
        linePass.AddLine({maxP.x, maxP.y, minP.z}, {maxP.x, maxP.y, maxP.z}, color);
        linePass.AddLine({maxP.x, maxP.y, maxP.z}, {minP.x, maxP.y, maxP.z}, color);
        linePass.AddLine({minP.x, maxP.y, maxP.z}, {minP.x, maxP.y, minP.z}, color);

        // 垂直 4 条边
        linePass.AddLine({minP.x, minP.y, minP.z}, {minP.x, maxP.y, minP.z}, color);
        linePass.AddLine({maxP.x, minP.y, minP.z}, {maxP.x, maxP.y, minP.z}, color);
        linePass.AddLine({maxP.x, minP.y, maxP.z}, {maxP.x, maxP.y, maxP.z}, color);
        linePass.AddLine({minP.x, minP.y, maxP.z}, {minP.x, maxP.y, maxP.z}, color);
    }
}
