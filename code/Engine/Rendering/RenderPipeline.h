#pragma once
#include "ShowFlags.h"
#include <d3d11.h>

// Forward declarations
class CCamera;
class CScene;

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
// - CForwardRenderPipeline: Forward 渲染流程（编辑器 + 游戏视图）
// - CReflectionProbePipeline: Reflection Probe 烘焙流程（未来）
// - CDeferredRenderPipeline: Deferred 渲染流程（未来）
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

        // Output Targets (required)
        ID3D11RenderTargetView* outputRTV;  // 输出 RenderTarget
        ID3D11DepthStencilView* outputDSV;  // 输出 DepthStencil

        // Viewport Size (required)
        unsigned int width;     // 渲染分辨率宽度
        unsigned int height;    // 渲染分辨率高度

        // Time (required)
        float deltaTime;        // Delta time (用于动画等)

        // Rendering Control (required)
        FShowFlags showFlags;   // 控制渲染哪些内容
    };

    virtual ~CRenderPipeline() = default;

    // ============================================
    // 核心渲染接口（纯虚函数）
    // ============================================
    // 子类必须实现此方法，定义具体的渲染流程
    //
    // 典型的渲染流程：
    // 1. Shadow Pass (if showFlags.Shadows)
    // 2. Scene Rendering (Opaque + Transparent + Skybox)
    // 3. Debug Rendering (if showFlags.DebugLines)
    // 4. Post-Processing (if showFlags.PostProcessing)
    // 5. Editor Tools (Grid, Gizmos, etc.)
    virtual void Render(const RenderContext& ctx) = 0;

    // ============================================
    // 初始化/清理（可选，子类可重写）
    // ============================================
    virtual bool Initialize() { return true; }
    virtual void Shutdown() {}
};
