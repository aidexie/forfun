// Engine/Rendering/DebugLinePass.h
#pragma once
#include <DirectXMath.h>
#include <vector>
#include "RHI/RHIPointers.h"

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

    std::vector<LineVertex> m_dynamicLines;

    // RHI resources
    RHI::BufferPtr m_vertexBuffer;
    RHI::BufferPtr m_cbPerFrameVS;
    RHI::BufferPtr m_cbPerFrameGS;
    RHI::ShaderPtr m_vs;
    RHI::ShaderPtr m_gs;
    RHI::ShaderPtr m_ps;
    RHI::PipelineStatePtr m_pso;

    unsigned int m_maxVertices = 10000;  // Max vertices per frame
    float m_lineThickness = 2.0f;
    bool m_initialized = false;
};
