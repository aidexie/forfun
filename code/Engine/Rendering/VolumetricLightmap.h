#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <string>
#include <cmath>
#include <algorithm>

// Forward declarations
class CScene;
class CPathTraceBaker;
struct SPathTraceConfig;

// ============================================
// Volumetric Lightmap Constants
// ============================================
static const int VL_BRICK_SIZE = 4;                    // 每个 Brick 4×4×4 体素
static const int VL_BRICK_VOXEL_COUNT = 64;            // 4^3 = 64
static const int VL_SH_COEFF_COUNT = 9;                // L2 球谐系数数量
static const int VL_MAX_LEVEL = 8;                     // 最大细分级别限制

// ============================================
// SBrick - 单个 Brick 数据
// ============================================
struct SBrick
{
    // 八叉树位置（在当前 Level 的整数坐标）
    int treeX = 0;
    int treeY = 0;
    int treeZ = 0;

    // 细分级别（0 = 最粗，越大越精细）
    int level = 0;

    // 在 Atlas 纹理中的位置（Brick 坐标，非体素坐标）
    int atlasX = 0;
    int atlasY = 0;
    int atlasZ = 0;

    // 世界空间 AABB
    DirectX::XMFLOAT3 worldMin = {0, 0, 0};
    DirectX::XMFLOAT3 worldMax = {0, 0, 0};

    // SH 数据（4×4×4 = 64 个体素，每个 9 个 RGB 系数）
    // shData[voxelIndex][coeffIndex] = RGB
    std::array<std::array<DirectX::XMFLOAT3, VL_SH_COEFF_COUNT>, VL_BRICK_VOXEL_COUNT> shData;

    // Validity data for leak prevention
    // validity[voxelIndex] = true if the probe at this voxel is valid (not inside geometry)
    std::array<bool, VL_BRICK_VOXEL_COUNT> validity;

    // 辅助：体素局部坐标 (x,y,z) → 线性索引
    static int VoxelIndex(int x, int y, int z) {
        return x + y * VL_BRICK_SIZE + z * VL_BRICK_SIZE * VL_BRICK_SIZE;
    }

    // 辅助：线性索引 → 体素局部坐标
    static void IndexToVoxel(int index, int& x, int& y, int& z) {
        x = index % VL_BRICK_SIZE;
        y = (index / VL_BRICK_SIZE) % VL_BRICK_SIZE;
        z = index / (VL_BRICK_SIZE * VL_BRICK_SIZE);
    }

    // 初始化 SH 数据为零，validity 为 true
    void ClearSHData() {
        for (auto& voxel : shData) {
            for (auto& coeff : voxel) {
                coeff = {0, 0, 0};
            }
        }
        validity.fill(true);  // Default to valid
    }

    SBrick() { ClearSHData(); }
};

// ============================================
// SOctreeNode - 八叉树节点
// ============================================
struct SOctreeNode
{
    // AABB 边界（世界空间）
    DirectX::XMFLOAT3 boundsMin = {0, 0, 0};
    DirectX::XMFLOAT3 boundsMax = {0, 0, 0};

    // 子节点索引（-1 = 无子节点）
    // 顺序：[0]=-X-Y-Z, [1]=+X-Y-Z, [2]=-X+Y-Z, [3]=+X+Y-Z,
    //       [4]=-X-Y+Z, [5]=+X-Y+Z, [6]=-X+Y+Z, [7]=+X+Y+Z
    int children[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

    // 如果是叶子节点，指向 Brick 索引（-1 = 非叶子）
    int brickIndex = -1;

    // 当前细分级别
    int level = 0;

    // 查询方法
    bool IsLeaf() const { return brickIndex >= 0; }
    bool HasChildren() const { return children[0] >= 0; }
};

// ============================================
// GPU 数据结构
// ============================================

// Indirection Entry（打包为 uint32）
// 格式：[brickIndex: 16bit][level: 8bit][padding: 8bit]
struct SIndirectionEntry
{
    uint16_t brickIndex;    // 指向哪个 Brick（0xFFFF = 空/无效）
    uint8_t  level;         // 细分级别
    uint8_t  padding;

    uint32_t Pack() const {
        return (uint32_t)brickIndex | ((uint32_t)level << 16) | ((uint32_t)padding << 24);
    }

    static SIndirectionEntry Unpack(uint32_t packed) {
        SIndirectionEntry e;
        e.brickIndex = (uint16_t)(packed & 0xFFFF);
        e.level = (uint8_t)((packed >> 16) & 0xFF);
        e.padding = (uint8_t)((packed >> 24) & 0xFF);
        return e;
    }

    static SIndirectionEntry Invalid() {
        SIndirectionEntry e;
        e.brickIndex = 0xFFFF;
        e.level = 0;
        e.padding = 0;
        return e;
    }
};

// Brick Info（传给 GPU 供 Shader 查询）
struct SBrickInfo
{
    DirectX::XMFLOAT3 worldMin;
    float _pad0;
    DirectX::XMFLOAT3 worldMax;
    float _pad1;
    DirectX::XMFLOAT3 atlasOffset;  // Atlas 中的偏移（Brick 坐标 × BRICK_SIZE）
    float _pad2;
};

// Constant Buffer
struct CB_VolumetricLightmap
{
    DirectX::XMFLOAT3 volumeMin;
    float _pad0;
    DirectX::XMFLOAT3 volumeMax;
    float _pad1;
    DirectX::XMFLOAT3 volumeInvSize;    // 1.0 / (max - min)
    float _pad2;

    DirectX::XMFLOAT3 indirectionInvSize;  // 1.0 / indirectionResolution
    float _pad3;
    DirectX::XMFLOAT3 brickAtlasInvSize;   // 1.0 / brickAtlasSize
    float _pad4;

    int indirectionResolution;          // Indirection 纹理分辨率（每个维度）
    int brickAtlasSize;                 // Atlas 纹理尺寸（每个维度）
    int maxLevel;                       // 最大细分级别
    int enabled;                        // 是否启用 (0/1)

    int brickCount;                     // 实际 Brick 数量
    int _pad5[3];
};

// ============================================
// CVolumetricLightmap - 主管理类
// ============================================
class CVolumetricLightmap
{
public:
    // ============================================
    // 用户配置（只需配置这些）
    // ============================================
    struct Config
    {
        // 体积范围（世界坐标）
        DirectX::XMFLOAT3 volumeMin = {-50.0f, -10.0f, -50.0f};
        DirectX::XMFLOAT3 volumeMax = {50.0f, 30.0f, 50.0f};

        // 最小 Brick 的世界尺寸（决定最大精度）
        // 例如：2.0f 表示最精细的 Brick 覆盖 2m × 2m × 2m
        float minBrickWorldSize = 2.0f;
    };

    // ============================================
    // 派生参数（自动计算）
    // ============================================
    struct DerivedParams
    {
        int maxLevel = 0;                 // 从 volumeSize / minBrickSize 计算
        int indirectionResolution = 1;    // = 2^maxLevel（每个维度）
        int actualBrickCount = 0;         // 八叉树构建后确定
        int brickAtlasSize = 0;           // 根据 actualBrickCount 计算
        float rootBrickSize = 0;          // 根节点 Brick 尺寸（最大边）
    };

    // ============================================
    // 生命周期
    // ============================================
    CVolumetricLightmap() = default;
    ~CVolumetricLightmap() = default;

    bool Initialize(const Config& config);
    void Shutdown();

    // ============================================
    // 烘焙流程
    // ============================================

    // Step 1: 构建八叉树（分析场景几何，决定细分）
    void BuildOctree(CScene& scene);

    // Step 2: 烘焙所有 Brick 的 SH（耗时操作）
    void BakeAllBricks(CScene& scene);

    // ============================================
    // GPU 资源
    // ============================================

    // Step 3: 创建 GPU 纹理（在 Bake 完成后调用）
    bool CreateGPUResources();

    // 上传数据到 GPU
    void UploadToGPU();

    // 绑定到 Shader（每帧调用）
    void Bind(ID3D11DeviceContext* context);

    // 解绑
    void Unbind(ID3D11DeviceContext* context);

    // ============================================
    // 序列化
    // ============================================
    bool SaveToFile(const std::string& path);
    bool LoadFromFile(const std::string& path);

    // ============================================
    // 状态查询
    // ============================================
    bool IsInitialized() const { return m_initialized; }
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool HasBakedData() const { return !m_bricks.empty(); }

    int GetBrickCount() const { return (int)m_bricks.size(); }
    int GetOctreeNodeCount() const { return (int)m_octreeNodes.size(); }
    const Config& GetConfig() const { return m_config; }
    Config& GetConfigMutable() { return m_config; }
    const DerivedParams& GetDerivedParams() const { return m_derived; }

    // 获取 Brick 数据（用于调试）
    const std::vector<SBrick>& GetBricks() const { return m_bricks; }
    const std::vector<SOctreeNode>& GetOctreeNodes() const { return m_octreeNodes; }

    // ============================================
    // 调试可视化
    // ============================================
    bool IsDebugDrawEnabled() const { return m_debugDrawEnabled; }
    void SetDebugDrawEnabled(bool enabled) { m_debugDrawEnabled = enabled; }

    // 绘制八叉树到 DebugLinePass
    void DrawOctreeDebug(class CDebugLinePass& linePass) const;

private:
    // ============================================
    // 派生参数计算
    // ============================================
    void computeDerivedParams();
    void computeAtlasSize();

    // ============================================
    // 八叉树构建
    // ============================================
    void buildOctreeRecursive(int nodeIndex, int level, CScene& scene);
    bool shouldSubdivide(const DirectX::XMFLOAT3& boundsMin,
                         const DirectX::XMFLOAT3& boundsMax,
                         int currentLevel, CScene& scene);
    bool checkGeometryInBounds(const DirectX::XMFLOAT3& boundsMin,
                               const DirectX::XMFLOAT3& boundsMax,
                               CScene& scene);

    // ============================================
    // Brick 管理
    // ============================================
    int createBrick(const DirectX::XMFLOAT3& boundsMin,
                    const DirectX::XMFLOAT3& boundsMax,
                    int level);
    bool allocateBrickInAtlas(SBrick& brick);
    void bakeBrick(SBrick& brick, CScene& scene, CPathTraceBaker& baker);

    // ============================================
    // Probe Dilation (leak prevention)
    // ============================================
    void dilateInvalidProbes();
    DirectX::XMFLOAT3 getVoxelWorldPosition(const SBrick& brick, int voxelX, int voxelY, int voxelZ) const;
    int findNearestValidVoxel(int brickIdx, int voxelIdx, int searchRadius) const;

    // ============================================
    // GPU 数据构建
    // ============================================
    void buildIndirectionData();
    int findBrickAtPosition(const DirectX::XMFLOAT3& worldPos);
    void packSHToAtlas();

    // ============================================
    // 工具函数
    // ============================================
    static int nextPowerOf2(int n);
    static DirectX::XMFLOAT3 getChildBoundsMin(const DirectX::XMFLOAT3& parentMin,
                                                const DirectX::XMFLOAT3& parentMax,
                                                int octant);
    static DirectX::XMFLOAT3 getChildBoundsMax(const DirectX::XMFLOAT3& parentMin,
                                                const DirectX::XMFLOAT3& parentMax,
                                                int octant);

private:
    // ============================================
    // 配置和状态
    // ============================================
    Config m_config;
    DerivedParams m_derived;
    bool m_initialized = false;
    bool m_enabled = false;
    bool m_gpuResourcesCreated = false;
    bool m_debugDrawEnabled = false;

    // ============================================
    // 八叉树数据
    // ============================================
    std::vector<SOctreeNode> m_octreeNodes;
    int m_rootNodeIndex = -1;

    // ============================================
    // Brick 数据
    // ============================================
    std::vector<SBrick> m_bricks;

    // Atlas 分配状态（当前分配位置）
    int m_atlasNextX = 0;
    int m_atlasNextY = 0;
    int m_atlasNextZ = 0;
    int m_atlasBricksPerSide = 0;  // Atlas 每边能放多少 Brick

    // ============================================
    // CPU 侧纹理数据（烘焙后填充）
    // ============================================
    std::vector<uint32_t> m_indirectionData;        // Packed SIndirectionEntry
    std::vector<DirectX::XMFLOAT4> m_brickAtlasSH0; // SH Atlas 纹理 0
    std::vector<DirectX::XMFLOAT4> m_brickAtlasSH1; // SH Atlas 纹理 1
    std::vector<DirectX::XMFLOAT4> m_brickAtlasSH2; // SH Atlas 纹理 2
    std::vector<SBrickInfo> m_brickInfoData;        // Brick 信息 Buffer

    // ============================================
    // GPU 资源
    // ============================================
    Microsoft::WRL::ComPtr<ID3D11Texture3D> m_indirectionTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_indirectionSRV;

    Microsoft::WRL::ComPtr<ID3D11Texture3D> m_brickAtlasTexture[3];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_brickAtlasSRV[3];

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_brickInfoBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_brickInfoSRV;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;  // s3: trilinear sampler for atlas
};
