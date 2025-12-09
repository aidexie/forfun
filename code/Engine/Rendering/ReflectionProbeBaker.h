#pragma once
#include <DirectXMath.h>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>

// Forward declarations
class CScene;
class CForwardRenderPipeline;
class CIBLGenerator;

// ============================================
// CReflectionProbeBaker - Reflection Probe Baking
// ============================================
// 负责烘焙 Reflection Probe 的 Cubemap 和 IBL maps
//
// 工作流程：
// 1. 创建 Cubemap render target (resolution × resolution, 6 faces)
// 2. 从 Probe 位置渲染 6 个方向 (±X, ±Y, ±Z)
// 3. 保存 Environment Cubemap 为 KTX2
// 4. 使用 IBLGenerator 生成 Irradiance + Pre-filtered maps
// 5. 保存 IBL maps 为 KTX2
// 6. 创建/更新 .ffasset 文件
//
// 使用示例：
// CReflectionProbeBaker baker;
// baker.Initialize();
// baker.BakeProbe(position, 256, scene, "probes/living_room/living_room.ffasset");
// baker.Shutdown();
// ============================================
class CReflectionProbeBaker
{
public:
    CReflectionProbeBaker();
    ~CReflectionProbeBaker();

    // ============================================
    // Initialization
    // ============================================

    bool Initialize();
    void Shutdown();

    // ============================================
    // Baking
    // ============================================

    // 烘焙单个 Reflection Probe
    //
    // position: Probe 在世界空间的位置
    // resolution: Cubemap 分辨率 (128/256/512)
    // scene: 要渲染的场景
    // outputAssetPath: 输出的 .ffasset 路径（相对于 E:/forfun/assets/）
    //                  例如: "probes/living_room/living_room.ffasset"
    //
    // 输出文件：
    // - living_room.ffasset (metadata)
    // - env.ktx2 (environment cubemap)
    // - irradiance.ktx2 (diffuse irradiance)
    // - prefiltered.ktx2 (specular pre-filtered)
    //
    // 返回: 成功返回 true，失败返回 false
    bool BakeProbe(
        const DirectX::XMFLOAT3& position,
        int resolution,
        CScene& scene,
        const std::string& outputAssetPath
    );

private:
    // ============================================
    // Cubemap Rendering
    // ============================================

    // 渲染 6 个面到 Cubemap（使用共享的 CCubemapRenderer）
    void renderToCubemap(
        const DirectX::XMFLOAT3& position,
        int resolution,
        CScene& scene,
        ID3D11Texture2D* outputCubemap
    );

    // ============================================
    // IBL Generation
    // ============================================

    // 生成 IBL maps 并保存
    // envCubemap: Environment cubemap 纹理
    // basePath: 基础路径（如 "E:/forfun/assets/probes/living_room"）
    // 生成文件：
    //   - basePath/irradiance.ktx2
    //   - basePath/prefiltered.ktx2
    void generateAndSaveIBL(
        ID3D11Texture2D* envCubemap,
        const std::string& basePath
    );

    // ============================================
    // File Saving
    // ============================================

    // 保存 Cubemap 为 KTX2 文件
    // cubemap: Cubemap 纹理
    // outputPath: 输出路径（完整路径）
    bool saveCubemapAsKTX2(
        ID3D11Texture2D* cubemap,
        const std::string& outputPath
    );

    // 创建/更新 .ffasset 文件
    // assetPath: .ffasset 完整路径
    // resolution: Cubemap 分辨率
    bool createAssetFile(
        const std::string& assetPath,
        int resolution
    );

    // ============================================
    // Helpers
    // ============================================

    // 创建 Cubemap render target 和 depth buffer
    // resolution: Cubemap 分辨率
    // 返回: 成功返回 true，失败返回 false
    bool createCubemapRenderTarget(int resolution);

    // ============================================
    // Members
    // ============================================

    CForwardRenderPipeline* m_pipeline;   // Rendering pipeline (owned)
    CIBLGenerator* m_iblGenerator;        // IBL generator (owned)

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_cubemapRT;      // Cubemap render target
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthBuffer;    // Depth buffer for cubemap rendering

    bool m_initialized = false;
};
