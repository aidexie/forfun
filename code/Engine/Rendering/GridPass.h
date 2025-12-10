// Engine/Rendering/GridPass.h
#pragma once
#include <DirectXMath.h>
#include "RHI/ICommandList.h"
#include "RHI/RHIPointers.h"

// CGridPass: Renders an infinite procedural grid on the XZ plane at Y=0
// Uses shader-based rendering (full-screen quad) with depth buffer reconstruction
// Supports dual-scale grid lines, distance fading, and view angle fading
//
// Phase 2 Migration: Uses global RHI Manager with smart pointers
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

private:
    CGridPass() = default;
    ~CGridPass() = default;
    CGridPass(const CGridPass&) = delete;
    CGridPass& operator=(const CGridPass&) = delete;

    void CreateShaders();
    void CreateBuffers();
    void CreatePipelineState();

    struct CBPerFrame {
        DirectX::XMMATRIX viewProj;
        DirectX::XMMATRIX invViewProj;
        DirectX::XMFLOAT3 cameraPos;
        float fadeStart;
        float fadeEnd;
        DirectX::XMFLOAT3 padding;
    };

    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_ps;
    RHI::BufferPtr m_cbPerFrame;
    RHI::PipelineStatePtr m_pso;

    bool m_initialized = false;
    bool m_enabled = true;

    // Grid settings
    DirectX::XMFLOAT3 m_gridColor = DirectX::XMFLOAT3(0.5f, 0.5f, 0.55f);
    float m_fadeStart = 50.0f;   // Start fading at 50m
    float m_fadeEnd = 100.0f;    // Fully fade at 100m
};
