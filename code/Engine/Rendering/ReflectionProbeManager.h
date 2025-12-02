#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

// Forward declarations
class CScene;

// ============================================
// CReflectionProbeManager - TextureCubeArray Based Probe System
// ============================================
// 使用 TextureCubeArray 统一管理所有 Reflection Probe
//
// 设计：
// - 最多 8 个 Probe（index 0 = 全局 IBL，1-7 = 局部 Probe）
// - 统一分辨率：Irradiance 32x32，Prefiltered 128x128
// - GPU 侧选择 Probe（Shader 中根据世界坐标计算）
// - 零状态切换（一次绑定，所有物体共享）
//
// Texture Slots：
// - t3: IrradianceArray (TextureCubeArray, 32x32, 8 slices)
// - t4: PrefilteredArray (TextureCubeArray, 128x128, 8 slices)
// - t5: BRDF LUT (不变)
// ============================================
class CReflectionProbeManager
{
public:
    // ============================================
    // Constants
    // ============================================
    static const int MAX_PROBES = 8;              // 最大 Probe 数量（包括全局 IBL）
    static const int IRRADIANCE_SIZE = 32;        // Irradiance cubemap 分辨率
    static const int PREFILTERED_SIZE = 128;      // Prefiltered cubemap 分辨率
    static const int PREFILTERED_MIP_COUNT = 7;   // Prefiltered mip 数量

    // ============================================
    // Probe Info (传给 Shader)
    // ============================================
    struct SProbeInfo
    {
        DirectX::XMFLOAT3 position;
        float radius;
    };

    // ============================================
    // CB_Probes 结构（与 Shader 对应）
    // ============================================
    struct CB_Probes
    {
        SProbeInfo probes[MAX_PROBES];
        int probeCount;
        DirectX::XMFLOAT3 _pad;
    };

    // ============================================
    // Public Interface
    // ============================================
    CReflectionProbeManager() = default;
    ~CReflectionProbeManager() = default;

    // 初始化（创建 TextureCubeArray 和 CB）
    bool Initialize();

    // 清理资源
    void Shutdown();

    // 从场景加载 Probe（全局 IBL + 局部 Probe）
    // globalIrradiancePath/globalPrefilteredPath: 全局 IBL 的 KTX2 路径
    void LoadProbesFromScene(
        CScene& scene,
        const std::string& globalIrradiancePath,
        const std::string& globalPrefilteredPath
    );

    // 绑定资源到 Shader（每帧调用一次）
    // slot 3: IrradianceArray
    // slot 4: PrefilteredArray
    // slot 4 (CB): CB_Probes
    void Bind(ID3D11DeviceContext* context);

    // 加载/重载全局 Probe（index 0）
    bool LoadGlobalProbe(const std::string& irrPath, const std::string& prefPath);

    // 热重载单个 Probe（用于 Bake 后更新）
    bool ReloadProbe(int probeIndex, const std::string& irrPath, const std::string& prefPath);

    // 获取 Probe 数量（用于调试）
    int GetProbeCount() const { return m_probeCount; }

    // CPU-side probe selection for per-object rendering
    // Returns probe index (0 = global fallback, 1-7 = local probes)
    int SelectProbeForPosition(const DirectX::XMFLOAT3& worldPos) const;

    // 获取 SRV（用于调试）
    ID3D11ShaderResourceView* GetIrradianceArraySRV() const { return m_irradianceArraySRV.Get(); }
    ID3D11ShaderResourceView* GetPrefilteredArraySRV() const { return m_prefilteredArraySRV.Get(); }

private:
    // ============================================
    // Internal Methods
    // ============================================

    // 创建 TextureCubeArray
    bool createCubeArrays();

    // 创建常量缓冲区
    bool createConstantBuffer();

    // 将单个 Cubemap 拷贝到 Array 的指定 slice
    bool copyCubemapToArray(
        ID3D11Texture2D* srcCubemap,
        ID3D11Texture2D* dstArray,
        int sliceIndex,
        int expectedSize,
        int mipCount
    );

    // 加载 KTX2 Cubemap 并拷贝到 Array
    bool loadAndCopyToArray(
        const std::string& ktx2Path,
        int sliceIndex,
        bool isIrradiance  // true = irradiance, false = prefiltered
    );

    // ============================================
    // Data
    // ============================================

    // TextureCubeArray 资源
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_irradianceArray;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_prefilteredArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_irradianceArraySRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_prefilteredArraySRV;

    // 常量缓冲区
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbProbes;

    // Probe 数据
    CB_Probes m_probeData{};
    int m_probeCount = 0;

    bool m_initialized = false;
};
