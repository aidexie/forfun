// Engine/Rendering/DebugLinePass.h
#pragma once
#include <DirectXMath.h>
#include <vector>
#include "RHI/RHIPointers.h"

// Forward declarations
namespace RHI {
    class IDescriptorSetLayout;
    class IDescriptorSet;
}

class CDebugLinePass {
public:
    void Initialize();
    void Shutdown();

    // Clear dynamic line buffer (call at start of frame)
    void BeginFrame();

    // CPU interface for adding lines
    void AddLine(DirectX::XMFLOAT3 from, DirectX::XMFLOAT3 to, DirectX::XMFLOAT4 color);
    void AddAABB(DirectX::XMFLOAT3 localMin, DirectX::XMFLOAT3 localMax,
                 DirectX::XMMATRIX worldMatrix, DirectX::XMFLOAT4 color);

    // Render all accumulated lines
    void Render(DirectX::XMMATRIX view, DirectX::XMMATRIX proj,
                unsigned int viewportWidth, unsigned int viewportHeight);

    // Settings
    void SetLineThickness(float thickness) { m_lineThickness = thickness; }
    float GetLineThickness() const { return m_lineThickness; }

    // Check if descriptor set mode is available (DX12 only)
    bool IsDescriptorSetModeAvailable() const { return m_perPassLayout != nullptr && m_pso_ds != nullptr; }

private:
    struct LineVertex {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 color;
    };

    struct CBPerFrameVS {
        DirectX::XMMATRIX viewProj;
    };

    struct CBPerFrameGS {
        DirectX::XMFLOAT2 viewportSize;
        float lineThickness;
        float padding;
    };

    void CreateShaders();
    void CreateBuffers();
    void CreatePipelineState();
    void UpdateVertexBuffer();
    void initDescriptorSets();

    std::vector<LineVertex> m_dynamicLines;

    // Legacy RHI resources (SM 5.0)
    RHI::BufferPtr m_vertexBuffer;
    RHI::BufferPtr m_cbPerFrameVS;
    RHI::BufferPtr m_cbPerFrameGS;
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_gs;
    RHI::ShaderPtr m_ps;
    RHI::PipelineStatePtr m_pso;

    // Descriptor set resources (SM 5.1, DX12 only)
    RHI::ShaderPtr m_vs_ds;
    RHI::ShaderPtr m_gs_ds;
    RHI::ShaderPtr m_ps_ds;
    RHI::PipelineStatePtr m_pso_ds;
    RHI::IDescriptorSetLayout* m_perPassLayout = nullptr;
    RHI::IDescriptorSet* m_perPassSet = nullptr;

    unsigned int m_maxVertices = 10000;  // Max vertices per frame
    float m_lineThickness = 2.0f;
    bool m_initialized = false;
};
