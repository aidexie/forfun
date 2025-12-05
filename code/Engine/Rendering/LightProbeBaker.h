#pragma once
#include <DirectXMath.h>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
// Forward declarations
class CScene;
class CForwardRenderPipeline;
struct SLightProbe;

// ============================================
// CLightProbeBaker - Light Probe Baking
// ============================================
// 负责烘焙 Light Probe 的 SH 系数
//
// 工作流程：
// 1. 创建低分辨率 Cubemap render target (32×32, 6 faces)
// 2. 从 Probe 位置渲染 6 个方向 (±X, ±Y, ±Z)
// 3. 读取 Cubemap 像素数据到 CPU
// 4. 使用 SphericalHarmonics::ProjectCubemapToSH() 投影到 L2 球谐系数
// 5. 将 SH 系数写入 LightProbe 组件
//
// 与 ReflectionProbeBaker 的区别：
// - Resolution: 32×32 (vs 128/256)
// - Output: SH 系数 (vs IBL maps)
// - Storage: 组件内存 (vs KTX2 文件)
//
// 使用示例：
// CLightProbeBaker baker;
// baker.Initialize();
// baker.BakeProbe(probeComponent, scene);
// baker.Shutdown();
// ============================================
class CLightProbeBaker
{
public:
    CLightProbeBaker();
    ~CLightProbeBaker();

    // ============================================
    // Initialization
    // ============================================

    bool Initialize();
    void Shutdown();

    // ============================================
    // Baking
    // ============================================

    // 烘焙单个 Light Probe
    //
    // probe: Light Probe 组件（会更新其 shCoeffs 字段）
    // position: Probe 在世界空间的位置
    // scene: 要渲染的场景
    //
    // 返回: 成功返回 true，失败返回 false
    bool BakeProbe(
        SLightProbe& probe,
        const DirectX::XMFLOAT3& position,
        CScene& scene
    );

    // 烘焙场景中所有 Light Probe
    // scene: 场景（包含所有 LightProbe 组件）
    // 返回: 成功烘焙的 probe 数量
    int BakeAllProbes(CScene& scene);

private:
    // ============================================
    // Cubemap Rendering
    // ============================================

    // 渲染 6 个面到 Cubemap
    // position: Probe 位置
    // scene: 场景
    // outputCubemap: 输出的 Cubemap 纹理
    void renderToCubemap(
        const DirectX::XMFLOAT3& position,
        CScene& scene,
        ID3D11Texture2D* outputCubemap
    );

    // 渲染单个 Cubemap face
    // face: Face 索引 (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z)
    // position: Probe 位置
    // scene: 场景
    // faceRTV: 该 face 的 RTV
    // dsv: 深度缓冲
    void renderCubemapFace(
        int face,
        const DirectX::XMFLOAT3& position,
        CScene& scene,
        ID3D11RenderTargetView* faceRTV,
        ID3D11DepthStencilView* dsv
    );

    // ============================================
    // SH Projection
    // ============================================

    // 从 Cubemap 投影到 SH 系数
    // cubemap: Cubemap 纹理
    // outCoeffs: 输出的 SH 系数（9 个 XMFLOAT3）
    // 返回: 成功返回 true，失败返回 false
    bool projectCubemapToSH(
        ID3D11Texture2D* cubemap,
        DirectX::XMFLOAT3 outCoeffs[9]
    );

    // ============================================
    // Helpers
    // ============================================

    // 创建 Cubemap render target 和 depth buffer
    // 返回: 成功返回 true，失败返回 false
    bool createCubemapRenderTarget();

    // ============================================
    // Members
    // ============================================

    static const int CUBEMAP_RESOLUTION = 32;  // Light Probe 使用低分辨率

    std::unique_ptr<CForwardRenderPipeline> m_pipeline;   // Rendering pipeline (owned)

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_cubemapRT;      // Cubemap render target
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthBuffer;    // Depth buffer for cubemap rendering
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture; // Staging texture for CPU readback

    bool m_initialized = false;
};
