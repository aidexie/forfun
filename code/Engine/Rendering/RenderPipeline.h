#pragma once
#include "ShowFlags.h"
#include "RHI/RHIResources.h"

// Forward declarations
class CCamera;
class CScene;
class CDebugLinePass;
class CClusteredLightingPass;

// ============================================
// CRenderPipeline - 渲染流程基类
// ============================================
// 抽象基类，定义了统一的渲染接口
//
// 设计思想：
// - 借鉴 Unity 的 Scriptable Render Pipeline (SRP)
// - 每个渲染需求对应一个 Pipeline 实现
// - 通过 ShowFlags 控制渲染哪些内容
//
// 子类实现：
// - CForwardRenderPipeline: Forward+ 渲染流程（clustered lighting）
// - CDeferredRenderPipeline: True Deferred 渲染流程
// ============================================
class CRenderPipeline
{
public:
    // ============================================
    // RenderContext - 渲染上下文
    // ============================================
    // 包含渲染所需的所有参数
    struct RenderContext
    {
        // Camera (required)
        CCamera& camera;        // 渲染使用的相机

        // Scene (required)
        CScene& scene;          // 要渲染的场景

        // Viewport Size (required)
        unsigned int width;     // 渲染分辨率宽度
        unsigned int height;    // 渲染分辨率高度

        // Time (required)
        float deltaTime;        // Delta time (用于动画等)

        // Rendering Control (required)
        FShowFlags showFlags;   // 控制渲染哪些内容

        // ============================================
        // Final Output (optional)
        // ============================================
        // 如果非空，Pipeline 会在渲染结束后将结果复制到这里
        // 如果为空，使用 Pipeline 的 GetOffscreenSRV() 获取结果
        RHI::ITexture* finalOutputTexture = nullptr;
        uint32_t finalOutputArraySlice = 0;  // For cubemap faces or array textures
        uint32_t finalOutputMipLevel = 0;    // For mip chain

        // 指定需要哪种输出格式
        enum class EOutputFormat
        {
            LDR,  // Tone-mapped sRGB (用于屏幕显示)
            HDR   // Linear HDR (用于 Reflection Probe, IBL 等)
        };
        EOutputFormat outputFormat = EOutputFormat::LDR;
    };

    virtual ~CRenderPipeline() = default;

    // ============================================
    // 核心渲染接口（纯虚函数）
    // ============================================
    virtual void Render(const RenderContext& ctx) = 0;

    // ============================================
    // 初始化/清理（可选，子类可重写）
    // ============================================
    virtual bool Initialize() { return true; }
    virtual void Shutdown() {}

    // ============================================
    // 离屏纹理访问接口（用于 ImGui 显示和测试）
    // ============================================
    virtual void* GetOffscreenSRV() const = 0;
    virtual void* GetOffscreenTexture() const = 0;
    virtual RHI::ITexture* GetOffscreenTextureRHI() const = 0;
    virtual unsigned int GetOffscreenWidth() const = 0;
    virtual unsigned int GetOffscreenHeight() const = 0;

    // ============================================
    // Debug Line Pass 访问（用于调试渲染）
    // ============================================
    virtual CDebugLinePass& GetDebugLinePass() = 0;

    // ============================================
    // Clustered Lighting Pass 访问（用于 debug UI）
    // ============================================
    virtual CClusteredLightingPass& GetClusteredLightingPass() = 0;
};
