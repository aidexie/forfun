#pragma once
#include "RenderPipeline.h"
#include "SceneRenderer.h"
#include "ShadowPass.h"
#include "PostProcessPass.h"
#include "DebugLinePass.h"
#include "GridPass.h"
#include "RHI/RHIPointers.h"

// ============================================
// CForwardRenderPipeline - Forward 渲染流程
// ============================================
// 完整的 Forward 渲染流程实现
//
// 渲染顺序：
// 1. Shadow Pass (if showFlags.Shadows)
// 2. Scene Rendering (Opaque + Transparent + Skybox) → HDR RT
// 3. Post-Processing (if showFlags.PostProcessing) → LDR RT
// 4. Debug Lines (if showFlags.DebugLines) → LDR RT
// 5. Grid (if showFlags.Grid) → LDR RT
//
// 使用场景：
// - 编辑器 Scene View（完整功能）
// - 游戏 Game View（无编辑器工具）
// ============================================
class CForwardRenderPipeline : public CRenderPipeline
{
public:
    CForwardRenderPipeline() = default;
    ~CForwardRenderPipeline() override = default;

    // 初始化/清理
    bool Initialize() override;
    void Shutdown() override;

    // 核心渲染接口
    void Render(const RenderContext& ctx) override;

    // 访问离屏纹理（用于 ImGui 显示）
    void* GetOffscreenSRV() const {
        return m_offLDR ? m_offLDR->GetSRV() : nullptr;
    }
    void* GetOffscreenTexture() const {
        return m_offLDR ? m_offLDR->GetNativeHandle() : nullptr;
    }
    unsigned int GetOffscreenWidth() const { return m_offscreenWidth; }
    unsigned int GetOffscreenHeight() const { return m_offscreenHeight; }

    // 访问内部 Pass（用于特殊需求）
    CDebugLinePass& GetDebugLinePass() { return m_debugLinePass; }
    CSceneRenderer& GetSceneRenderer() { return m_sceneRenderer; }

private:
    // 确保离屏目标尺寸正确
    void ensureOffscreen(unsigned int w, unsigned int h);

    // ============================================
    // 渲染 Pass
    // ============================================
    CShadowPass m_shadowPass;
    CSceneRenderer m_sceneRenderer;
    CDebugLinePass m_debugLinePass;
    CPostProcessPass m_postProcess;

    // ============================================
    // 离屏目标（HDR + LDR）使用 RHI 管理
    // ============================================
    RHI::TexturePtr m_offHDR;        // HDR 中间目标（R16G16B16A16_FLOAT）
    RHI::TexturePtr m_offDepth;      // 深度缓冲（R24G8_TYPELESS）
    RHI::TexturePtr m_offLDR;        // LDR 最终目标（R8G8B8A8_TYPELESS with sRGB RTV）
    unsigned int m_offscreenWidth = 0;
    unsigned int m_offscreenHeight = 0;
};
