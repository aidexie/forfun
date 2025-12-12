#include "DX12PipelineState.h"
#include "DX12Resources.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// PSOCacheKey Implementation
// ============================================

bool PSOCacheKey::operator==(const PSOCacheKey& other) const {
    return vsPtr == other.vsPtr &&
           psPtr == other.psPtr &&
           gsPtr == other.gsPtr &&
           hsPtr == other.hsPtr &&
           dsPtr == other.dsPtr &&
           rasterizerHash == other.rasterizerHash &&
           depthStencilHash == other.depthStencilHash &&
           blendHash == other.blendHash &&
           rtFormatHash == other.rtFormatHash &&
           dsFormat == other.dsFormat &&
           topologyType == other.topologyType &&
           inputLayoutHash == other.inputLayoutHash;
}

size_t PSOCacheKeyHash::operator()(const PSOCacheKey& key) const {
    size_t hash = 0;
    auto hashCombine = [&hash](size_t value) {
        hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    };

    hashCombine(reinterpret_cast<size_t>(key.vsPtr));
    hashCombine(reinterpret_cast<size_t>(key.psPtr));
    hashCombine(reinterpret_cast<size_t>(key.gsPtr));
    hashCombine(reinterpret_cast<size_t>(key.hsPtr));
    hashCombine(reinterpret_cast<size_t>(key.dsPtr));
    hashCombine(key.rasterizerHash);
    hashCombine(key.depthStencilHash);
    hashCombine(key.blendHash);
    hashCombine(key.rtFormatHash);
    hashCombine(static_cast<size_t>(key.dsFormat));
    hashCombine(static_cast<size_t>(key.topologyType));
    hashCombine(key.inputLayoutHash);

    return hash;
}

// ============================================
// CDX12PSOBuilder Implementation
// ============================================

CDX12PSOBuilder::CDX12PSOBuilder() {
    ZeroMemory(&m_desc, sizeof(m_desc));

    // Default rasterizer state
    m_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    m_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    m_desc.RasterizerState.FrontCounterClockwise = FALSE;
    m_desc.RasterizerState.DepthBias = 0;
    m_desc.RasterizerState.DepthBiasClamp = 0.0f;
    m_desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    m_desc.RasterizerState.DepthClipEnable = TRUE;
    m_desc.RasterizerState.MultisampleEnable = FALSE;
    m_desc.RasterizerState.AntialiasedLineEnable = FALSE;
    m_desc.RasterizerState.ForcedSampleCount = 0;
    m_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Default depth stencil state
    m_desc.DepthStencilState.DepthEnable = TRUE;
    m_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    m_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    m_desc.DepthStencilState.StencilEnable = FALSE;
    m_desc.DepthStencilState.StencilReadMask = 0xFF;
    m_desc.DepthStencilState.StencilWriteMask = 0xFF;

    // Default blend state
    m_desc.BlendState.AlphaToCoverageEnable = FALSE;
    m_desc.BlendState.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; ++i) {
        m_desc.BlendState.RenderTarget[i].BlendEnable = FALSE;
        m_desc.BlendState.RenderTarget[i].LogicOpEnable = FALSE;
        m_desc.BlendState.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        m_desc.BlendState.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        m_desc.BlendState.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        m_desc.BlendState.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        m_desc.BlendState.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        m_desc.BlendState.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        m_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // Default sample desc
    m_desc.SampleMask = UINT_MAX;
    m_desc.SampleDesc.Count = 1;
    m_desc.SampleDesc.Quality = 0;

    // Default topology
    m_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

void CDX12PSOBuilder::SetVertexShader(const D3D12_SHADER_BYTECODE& bytecode) {
    m_desc.VS = bytecode;
}

void CDX12PSOBuilder::SetPixelShader(const D3D12_SHADER_BYTECODE& bytecode) {
    m_desc.PS = bytecode;
}

void CDX12PSOBuilder::SetGeometryShader(const D3D12_SHADER_BYTECODE& bytecode) {
    m_desc.GS = bytecode;
}

void CDX12PSOBuilder::SetHullShader(const D3D12_SHADER_BYTECODE& bytecode) {
    m_desc.HS = bytecode;
}

void CDX12PSOBuilder::SetDomainShader(const D3D12_SHADER_BYTECODE& bytecode) {
    m_desc.DS = bytecode;
}

void CDX12PSOBuilder::SetRootSignature(ID3D12RootSignature* rootSig) {
    m_desc.pRootSignature = rootSig;
}

void CDX12PSOBuilder::SetInputLayout(const std::vector<D3D12_INPUT_ELEMENT_DESC>& elements) {
    m_inputElements = elements;
    m_desc.InputLayout.pInputElementDescs = m_inputElements.data();
    m_desc.InputLayout.NumElements = static_cast<UINT>(m_inputElements.size());
}

void CDX12PSOBuilder::SetRasterizerState(const D3D12_RASTERIZER_DESC& desc) {
    m_desc.RasterizerState = desc;
}

void CDX12PSOBuilder::SetDepthStencilState(const D3D12_DEPTH_STENCIL_DESC& desc) {
    m_desc.DepthStencilState = desc;
}

void CDX12PSOBuilder::SetBlendState(const D3D12_BLEND_DESC& desc) {
    m_desc.BlendState = desc;
}

void CDX12PSOBuilder::SetRenderTargetFormats(const std::vector<DXGI_FORMAT>& formats) {
    m_desc.NumRenderTargets = static_cast<UINT>(formats.size());
    for (size_t i = 0; i < formats.size() && i < 8; ++i) {
        m_desc.RTVFormats[i] = formats[i];
    }
}

void CDX12PSOBuilder::SetDepthStencilFormat(DXGI_FORMAT format) {
    m_desc.DSVFormat = format;
}

void CDX12PSOBuilder::SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
    m_desc.PrimitiveTopologyType = type;
}

void CDX12PSOBuilder::SetSampleDesc(UINT count, UINT quality) {
    m_desc.SampleDesc.Count = count;
    m_desc.SampleDesc.Quality = quality;
}

ID3D12PipelineState* CDX12PSOBuilder::Build(ID3D12Device* device) {
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateGraphicsPipelineState(&m_desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12PSOBuilder] CreateGraphicsPipelineState failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }
    return pso.Detach();
}

// ============================================
// CDX12PSOCache Implementation
// ============================================

CDX12PSOCache& CDX12PSOCache::Instance() {
    static CDX12PSOCache instance;
    return instance;
}

bool CDX12PSOCache::Initialize(ID3D12Device* device) {
    if (m_initialized) return true;

    m_device = device;
    m_initialized = true;
    return true;
}

void CDX12PSOCache::Shutdown() {
    Clear();
    m_device = nullptr;
    m_initialized = false;
}

ID3D12PipelineState* CDX12PSOCache::GetOrCreateGraphicsPSO(
    const PSOCacheKey& key,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc
) {
    auto it = m_graphicsPSOCache.find(key);
    if (it != m_graphicsPSOCache.end()) {
        return it->second.Get();
    }

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12PSOCache] CreateGraphicsPipelineState failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    ID3D12PipelineState* result = pso.Get();
    m_graphicsPSOCache[key] = std::move(pso);
    return result;
}

ID3D12PipelineState* CDX12PSOCache::GetOrCreateComputePSO(
    const void* shaderPtr,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc
) {
    auto it = m_computePSOCache.find(shaderPtr);
    if (it != m_computePSOCache.end()) {
        return it->second.Get();
    }

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12PSOCache] CreateComputePipelineState failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    ID3D12PipelineState* result = pso.Get();
    m_computePSOCache[shaderPtr] = std::move(pso);
    return result;
}

void CDX12PSOCache::Clear() {
    m_graphicsPSOCache.clear();
    m_computePSOCache.clear();
}

// ============================================
// Conversion Helpers
// ============================================

D3D12_CULL_MODE ToD3D12CullMode(ECullMode mode) {
    switch (mode) {
        case ECullMode::None:  return D3D12_CULL_MODE_NONE;
        case ECullMode::Front: return D3D12_CULL_MODE_FRONT;
        case ECullMode::Back:  return D3D12_CULL_MODE_BACK;
        default:               return D3D12_CULL_MODE_BACK;
    }
}

D3D12_FILL_MODE ToD3D12FillMode(EFillMode mode) {
    switch (mode) {
        case EFillMode::Solid:     return D3D12_FILL_MODE_SOLID;
        case EFillMode::Wireframe: return D3D12_FILL_MODE_WIREFRAME;
        default:                   return D3D12_FILL_MODE_SOLID;
    }
}

D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(EComparisonFunc func) {
    switch (func) {
        case EComparisonFunc::Never:        return D3D12_COMPARISON_FUNC_NEVER;
        case EComparisonFunc::Less:         return D3D12_COMPARISON_FUNC_LESS;
        case EComparisonFunc::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
        case EComparisonFunc::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case EComparisonFunc::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
        case EComparisonFunc::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case EComparisonFunc::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case EComparisonFunc::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
        default:                            return D3D12_COMPARISON_FUNC_LESS;
    }
}

D3D12_BLEND ToD3D12BlendFactor(EBlendFactor factor) {
    switch (factor) {
        case EBlendFactor::Zero:        return D3D12_BLEND_ZERO;
        case EBlendFactor::One:         return D3D12_BLEND_ONE;
        case EBlendFactor::SrcColor:    return D3D12_BLEND_SRC_COLOR;
        case EBlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
        case EBlendFactor::SrcAlpha:    return D3D12_BLEND_SRC_ALPHA;
        case EBlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case EBlendFactor::DstColor:    return D3D12_BLEND_DEST_COLOR;
        case EBlendFactor::InvDstColor: return D3D12_BLEND_INV_DEST_COLOR;
        case EBlendFactor::DstAlpha:    return D3D12_BLEND_DEST_ALPHA;
        case EBlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        default:                        return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP ToD3D12BlendOp(EBlendOp op) {
    switch (op) {
        case EBlendOp::Add:         return D3D12_BLEND_OP_ADD;
        case EBlendOp::Subtract:    return D3D12_BLEND_OP_SUBTRACT;
        case EBlendOp::RevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case EBlendOp::Min:         return D3D12_BLEND_OP_MIN;
        case EBlendOp::Max:         return D3D12_BLEND_OP_MAX;
        default:                    return D3D12_BLEND_OP_ADD;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(EPrimitiveTopology topology) {
    switch (topology) {
        case EPrimitiveTopology::PointList:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case EPrimitiveTopology::LineList:
        case EPrimitiveTopology::LineStrip:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case EPrimitiveTopology::TriangleList:
        case EPrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        default:                                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

DXGI_FORMAT ToD3D12VertexFormat(EVertexFormat format) {
    switch (format) {
        case EVertexFormat::Float:       return DXGI_FORMAT_R32_FLOAT;
        case EVertexFormat::Float2:      return DXGI_FORMAT_R32G32_FLOAT;
        case EVertexFormat::Float3:      return DXGI_FORMAT_R32G32B32_FLOAT;
        case EVertexFormat::Float4:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case EVertexFormat::UByte4:      return DXGI_FORMAT_R8G8B8A8_UINT;
        case EVertexFormat::UByte4_Norm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:                         return DXGI_FORMAT_R32G32B32_FLOAT;
    }
}

const char* ToD3D12SemanticName(EVertexSemantic semantic) {
    switch (semantic) {
        case EVertexSemantic::Position: return "POSITION";
        case EVertexSemantic::Normal:   return "NORMAL";
        case EVertexSemantic::Tangent:  return "TANGENT";
        case EVertexSemantic::Texcoord: return "TEXCOORD";
        case EVertexSemantic::Color:    return "COLOR";
        default:                        return "POSITION";
    }
}

} // namespace DX12
} // namespace RHI
