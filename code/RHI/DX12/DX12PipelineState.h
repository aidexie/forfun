#pragma once

#include "DX12Common.h"
#include "../RHICommon.h"
#include <unordered_map>

// ============================================
// DX12 Pipeline State Management
// ============================================

namespace RHI {
namespace DX12 {

// Forward declarations
class CDX12RenderContext;
class CDX12Shader;

// ============================================
// PSO Cache Key
// ============================================
// Hash key for PSO caching

struct PSOCacheKey {
    // Shaders
    const void* vsPtr = nullptr;
    const void* psPtr = nullptr;
    const void* gsPtr = nullptr;
    const void* hsPtr = nullptr;
    const void* dsPtr = nullptr;

    // States
    uint32_t rasterizerHash = 0;
    uint32_t depthStencilHash = 0;
    uint32_t blendHash = 0;

    // Render target formats
    uint32_t rtFormatHash = 0;
    DXGI_FORMAT dsFormat = DXGI_FORMAT_UNKNOWN;

    // Topology
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Input layout hash
    uint32_t inputLayoutHash = 0;

    bool operator==(const PSOCacheKey& other) const;
};

struct PSOCacheKeyHash {
    size_t operator()(const PSOCacheKey& key) const;
};

// ============================================
// PSO Builder
// ============================================
// Helper class to build D3D12_GRAPHICS_PIPELINE_STATE_DESC

class CDX12PSOBuilder {
public:
    CDX12PSOBuilder();

    // Set shaders
    void SetVertexShader(const D3D12_SHADER_BYTECODE& bytecode);
    void SetPixelShader(const D3D12_SHADER_BYTECODE& bytecode);
    void SetGeometryShader(const D3D12_SHADER_BYTECODE& bytecode);
    void SetHullShader(const D3D12_SHADER_BYTECODE& bytecode);
    void SetDomainShader(const D3D12_SHADER_BYTECODE& bytecode);

    // Set root signature
    void SetRootSignature(ID3D12RootSignature* rootSig);

    // Set input layout
    void SetInputLayout(const std::vector<D3D12_INPUT_ELEMENT_DESC>& elements);

    // Set rasterizer state
    void SetRasterizerState(const D3D12_RASTERIZER_DESC& desc);

    // Set depth stencil state
    void SetDepthStencilState(const D3D12_DEPTH_STENCIL_DESC& desc);

    // Set blend state
    void SetBlendState(const D3D12_BLEND_DESC& desc);

    // Set render target formats
    void SetRenderTargetFormats(const std::vector<DXGI_FORMAT>& formats);

    // Set depth stencil format
    void SetDepthStencilFormat(DXGI_FORMAT format);

    // Set primitive topology type
    void SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE type);

    // Set sample desc
    void SetSampleDesc(UINT count, UINT quality);

    // Build the PSO
    ID3D12PipelineState* Build(ID3D12Device* device);

    // Get desc for inspection
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& GetDesc() const { return m_desc; }

private:
    D3D12_GRAPHICS_PIPELINE_STATE_DESC m_desc;
    std::vector<D3D12_INPUT_ELEMENT_DESC> m_inputElements;
};

// ============================================
// PSO Cache
// ============================================
// Caches created PSOs to avoid redundant creation

class CDX12PSOCache {
public:
    static CDX12PSOCache& Instance();

    // Initialize with device
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // Get or create graphics PSO
    ID3D12PipelineState* GetOrCreateGraphicsPSO(
        const PSOCacheKey& key,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc
    );

    // Get or create compute PSO
    ID3D12PipelineState* GetOrCreateComputePSO(
        const void* shaderPtr,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc
    );

    // Clear cache
    void Clear();

private:
    CDX12PSOCache() = default;
    ~CDX12PSOCache() = default;

    // Non-copyable
    CDX12PSOCache(const CDX12PSOCache&) = delete;
    CDX12PSOCache& operator=(const CDX12PSOCache&) = delete;

private:
    ID3D12Device* m_device = nullptr;

    // Graphics PSO cache
    std::unordered_map<PSOCacheKey, ComPtr<ID3D12PipelineState>, PSOCacheKeyHash> m_graphicsPSOCache;

    // Compute PSO cache
    std::unordered_map<const void*, ComPtr<ID3D12PipelineState>> m_computePSOCache;

    bool m_initialized = false;
};

// ============================================
// Conversion Helpers
// ============================================

// Convert RHI cull mode to D3D12
D3D12_CULL_MODE ToD3D12CullMode(ECullMode mode);

// Convert RHI fill mode to D3D12
D3D12_FILL_MODE ToD3D12FillMode(EFillMode mode);

// Convert RHI comparison func to D3D12
D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(EComparisonFunc func);

// Convert RHI blend factor to D3D12
D3D12_BLEND ToD3D12BlendFactor(EBlendFactor factor);

// Convert RHI blend op to D3D12
D3D12_BLEND_OP ToD3D12BlendOp(EBlendOp op);

// Convert RHI primitive topology to D3D12 topology type
D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(EPrimitiveTopology topology);

// Convert RHI vertex format to DXGI format
DXGI_FORMAT ToD3D12VertexFormat(EVertexFormat format);

// Get semantic name string
const char* ToD3D12SemanticName(EVertexSemantic semantic);

} // namespace DX12
} // namespace RHI
