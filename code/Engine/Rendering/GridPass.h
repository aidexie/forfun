// Engine/Rendering/GridPass.h
#pragma once
#include <DirectXMath.h>
#include "RHI/IRenderContext.h"
#include "RHI/ICommandList.h"
#include "RHI/RHIResources.h"

// CGridPass: Renders an infinite procedural grid on the XZ plane at Y=0
// Uses shader-based rendering (full-screen quad) with depth buffer reconstruction
// Supports dual-scale grid lines, distance fading, and view angle fading
// 
// Phase 1 Migration: Uses RHI internally, but compatible with existing code
class CGridPass {
public:
    static CGridPass& Instance();

    void Initialize();  // Creates internal RHI context
    void Shutdown();

    // Render the grid
    // Legacy signature for backward compatibility
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

    RHI::IRenderContext* m_renderContext = nullptr;  // Owned by GridPass for Phase 1
    RHI::IShader* m_vs = nullptr;
    RHI::IShader* m_ps = nullptr;
    RHI::IBuffer* m_cbPerFrame = nullptr;
    RHI::IPipelineState* m_pso = nullptr;

    bool m_initialized = false;
    bool m_enabled = true;

    // Grid settings
    DirectX::XMFLOAT3 m_gridColor = DirectX::XMFLOAT3(0.5f, 0.5f, 0.55f);
    float m_fadeStart = 50.0f;   // Start fading at 50m
    float m_fadeEnd = 100.0f;    // Fully fade at 100m
};
