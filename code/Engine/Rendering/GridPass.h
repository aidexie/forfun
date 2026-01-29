// Engine/Rendering/GridPass.h
#pragma once
#include <DirectXMath.h>
#include "RHI/ICommandList.h"
#include "RHI/RHIPointers.h"

// Forward declarations
namespace RHI {
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

// CGridPass: Renders an infinite procedural grid on the XZ plane at Y=0
// Uses shader-based rendering (full-screen quad) with depth buffer reconstruction
// Supports dual-scale grid lines, distance fading, and view angle fading
//
// Phase 2 Migration: Uses global RHI Manager with smart pointers
// Descriptor Set Model:
// - Set 1 (PerPass, space1): CBV for grid parameters
class CGridPass {
public:
    static CGridPass& Instance();

    void Initialize();
    void Shutdown();

    // Render the grid
    void Render(DirectX::XMMATRIX view, DirectX::XMMATRIX proj,
                DirectX::XMFLOAT3 cameraPos);

    // Settings
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    void SetGridColor(DirectX::XMFLOAT3 color) { m_gridColor = color; }
    DirectX::XMFLOAT3 GetGridColor() const { return m_gridColor; }

    void SetFadeDistance(float start, float end) {
        m_fadeStart = start;
        m_fadeEnd = end;
    }

    // Check if descriptor set mode is available (DX12 only)
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_pso_ds != nullptr; }

private:
    CGridPass() = default;
    ~CGridPass() = default;
    CGridPass(const CGridPass&) = delete;
    CGridPass& operator=(const CGridPass&) = delete;

    void CreateShaders();
    void CreateBuffers();
    void CreatePipelineState();
    void initDescriptorSets();

    struct CBPerFrame {
        DirectX::XMMATRIX viewProj;
        DirectX::XMMATRIX invViewProj;
        DirectX::XMFLOAT3 cameraPos;
        float fadeStart;
        float fadeEnd;
        DirectX::XMFLOAT3 padding;
    };

    // Legacy resources (SM 5.0)
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;
    RHI::BufferPtr m_cbPerFrame;
    RHI::PipelineStatePtr m_pso;

    // Descriptor set resources (SM 5.1, DX12 only)
    RHI::ShaderPtr m_vs_ds;
    RHI::ShaderPtr m_ps_ds;
    RHI::PipelineStatePtr m_pso_ds;
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    bool m_initialized = false;
    bool m_enabled = true;

    // Grid settings
    DirectX::XMFLOAT3 m_gridColor = DirectX::XMFLOAT3(0.5f, 0.5f, 0.55f);
    float m_fadeStart = 50.0f;   // Start fading at 50m
    float m_fadeEnd = 100.0f;    // Fully fade at 100m
};
