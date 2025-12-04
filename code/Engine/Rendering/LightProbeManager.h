#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

// Forward declarations
class CScene;

// ============================================
// CLightProbeManager - Light Probe System
// ============================================
// 管理场景中所有 Light Probe 的 SH 系数，并提供混合查询
//
// 设计：
// - 使用 StructuredBuffer 存储所有 Probe 数据（支持大量 Probe）
// - 距离权重混合最近的 4 个 Probe
// - Fallback 到全局 IBL（当没有 Probe 覆盖时）
//
// 与 Reflection Probe 的区别：
// - Reflection Probe: Cubemap (TextureCubeArray), 8 个, 镜面反射
// - Light Probe: SH 系数 (StructuredBuffer), 100+, 漫反射环境光
//
// Shader Slots：
// - t15: LightProbeBuffer (StructuredBuffer<LightProbeData>)
// - b5: CB_LightProbeParams (probe count, blend settings)
// ============================================
class CLightProbeManager
{
public:
    // ============================================
    // Constants
    // ============================================
    static const int MAX_PROBES = 128;           // 最大 Probe 数量
    static const int MAX_BLEND_PROBES = 4;       // 最多混合 4 个 Probe
    static const int SH_COEFF_COUNT = 9;         // L2 球谐系数数量

    // ============================================
    // GPU Data Structures (与 Shader 对应)
    // ============================================

    // 单个 Light Probe 的 GPU 数据
    struct LightProbeData
    {
        DirectX::XMFLOAT3 position;   // Probe 位置
        float radius;                 // 影响半径

        // L2 球谐系数（9 个系数，每个是 RGB 向量）
        // shCoeffs[0]: L0
        // shCoeffs[1-3]: L1 (m=-1, m=0, m=1)
        // shCoeffs[4-8]: L2 (m=-2, m=-1, m=0, m=1, m=2)
        DirectX::XMFLOAT3 shCoeffs[9]; // 9 bands × RGB
    };

    // 常量缓冲区（传递全局参数）
    struct CB_LightProbeParams
    {
        int probeCount;               // 当前场景中的 Probe 数量
        float blendFalloff;           // 权重衰减指数（默认 2.0）
        DirectX::XMFLOAT2 _pad;
    };

    // ============================================
    // Public Interface
    // ============================================
    CLightProbeManager() = default;
    ~CLightProbeManager() = default;

    // 初始化（创建 StructuredBuffer 和 CB）
    bool Initialize();

    // 清理资源
    void Shutdown();

    // 从场景加载所有 Light Probe
    void LoadProbesFromScene(CScene& scene);

    // 绑定资源到 Shader（每帧调用一次）
    // slot 15: LightProbeBuffer (StructuredBuffer)
    // slot 5 (CB): CB_LightProbeParams
    void Bind(ID3D11DeviceContext* context);

    // 获取 Probe 数量（用于调试）
    int GetProbeCount() const { return m_probeCount; }

    // CPU-side probe blending for debugging/preview
    // Returns blended SH coefficients (9 bands × RGB)
    void BlendProbesForPosition(
        const DirectX::XMFLOAT3& worldPos,
        DirectX::XMFLOAT3 outShCoeffs[9]
    ) const;

    // 获取 SRV（用于调试）
    ID3D11ShaderResourceView* GetProbeBufferSRV() const { return m_probeBufferSRV.Get(); }

private:
    // ============================================
    // Internal Methods
    // ============================================

    // 创建 StructuredBuffer
    bool createStructuredBuffer();

    // 创建常量缓冲区
    bool createConstantBuffer();

    // 更新 StructuredBuffer 数据
    void updateProbeBuffer();

    // 更新常量缓冲区
    void updateConstantBuffer();

    // ============================================
    // Data
    // ============================================

    // StructuredBuffer 资源
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_probeBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_probeBufferSRV;

    // 常量缓冲区
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbParams;

    // CPU 侧 Probe 数据
    std::vector<LightProbeData> m_probeData;
    int m_probeCount = 0;

    // 混合参数
    CB_LightProbeParams m_params{};

    bool m_initialized = false;
};
