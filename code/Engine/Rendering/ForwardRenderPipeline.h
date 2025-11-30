#pragma once
#include "RenderPipeline.h"
#include "SceneRenderer.h"
#include "ShadowPass.h"
#include "PostProcessPass.h"
#include "DebugLinePass.h"
#include "GridPass.h"
#include <d3d11.h>
#include <wrl/client.h>

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
    ID3D11ShaderResourceView* GetOffscreenSRV() const { return m_offLDR.srv.Get(); }
    ID3D11Texture2D* GetOffscreenTexture() const { return m_offLDR.color.Get(); }
    unsigned int GetOffscreenWidth() const { return m_offLDR.w; }
    unsigned int GetOffscreenHeight() const { return m_offLDR.h; }

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
    // 离屏目标（HDR + LDR）
    // ============================================
    struct Offscreen {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> color;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV;
        unsigned int w = 0, h = 0;

        void Reset() {
            color.Reset(); rtv.Reset(); srv.Reset();
            depth.Reset(); dsv.Reset(); depthSRV.Reset();
            w = 0; h = 0;
        }
    };

    Offscreen m_off;    // HDR 中间目标（R16G16B16A16_FLOAT）
    Offscreen m_offLDR; // LDR 最终目标（R8G8B8A8_UNORM_SRGB RTV + R8G8B8A8_UNORM SRV）
};
