#pragma once
#include "RHI/RHIPointers.h"
#include "IPerFrameContributor.h"
#include <DirectXMath.h>
#include <string>
#include <vector>
#include <memory>

// Forward declarations
namespace RHI {
    class ICommandList;
    class IDescriptorSet;
}

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
// - t5: BRDF LUT (2D texture, 512x512)
// ============================================
class CReflectionProbeManager : public IPerFrameContributor
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

    // 从场景加载局部 Probe (index 1-7)
    // 全局 IBL (index 0) 由 Initialize() 设置默认值，或通过 LoadGlobalProbe() 更新
    void LoadLocalProbesFromScene(CScene& scene);

    // 绑定资源到 Shader（每帧调用一次）
    // slot 3: IrradianceArray
    // slot 4: PrefilteredArray
    // slot 4 (CB): CB_Probes
    void Bind(RHI::ICommandList* cmdList);

    // IPerFrameContributor implementation
    void PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) override;

    // 加载/重载全局 Probe（index 0）
    bool LoadGlobalProbe(const std::string& irrPath, const std::string& prefPath);

    // 加载 BRDF LUT（全局唯一，在 Initialize 后调用一次）
    bool LoadBrdfLut(const std::string& brdfLutPath);

    // 获取 Probe 数量（用于调试）
    int GetProbeCount() const { return m_probeCount; }

    // CPU-side probe selection for per-object rendering
    // Returns probe index (0 = global fallback, 1-7 = local probes)
    int SelectProbeForPosition(const DirectX::XMFLOAT3& worldPos) const;

    // 获取纹理（用于调试和渲染）
    RHI::ITexture* GetIrradianceArrayTexture() const { return m_irradianceArray.get(); }
    RHI::ITexture* GetPrefilteredArrayTexture() const { return m_prefilteredArray.get(); }
    RHI::ITexture* GetBrdfLutTexture() const { return m_brdfLutTexture.get(); }

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
        RHI::ITexture* srcCubemap,
        RHI::ITexture* dstArray,
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

    // 创建默认纯色 fallback cubemap 并填充到指定 slice
    void fillSliceWithSolidColor(int sliceIndex, float r, float g, float b);

    // 更新常量缓冲区
    void updateConstantBuffer();

    // ============================================
    // Data
    // ============================================

    // TextureCubeArray 资源 (RHI)
    RHI::TexturePtr m_irradianceArray;
    RHI::TexturePtr m_prefilteredArray;

    // BRDF LUT (2D texture, shared across all probes)
    RHI::TexturePtr m_brdfLutTexture;

    // 常量缓冲区
    RHI::BufferPtr m_cbProbes;

    // Probe 数据
    CB_Probes m_probeData{};
    int m_probeCount = 0;

    bool m_initialized = false;
};
