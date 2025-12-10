#pragma once
#include "../RHICommon.h"
#include <d3d11.h>

// ============================================
// DX11 Utility Functions - RHI to D3D11 conversions
// ============================================

namespace RHI {
namespace DX11 {

// ============================================
// Format Conversions
// ============================================

inline DXGI_FORMAT ToDXGIFormat(ETextureFormat format) {
    switch (format) {
        case ETextureFormat::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case ETextureFormat::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case ETextureFormat::R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case ETextureFormat::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case ETextureFormat::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case ETextureFormat::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case ETextureFormat::B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case ETextureFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case ETextureFormat::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case ETextureFormat::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
        case ETextureFormat::BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case ETextureFormat::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
        case ETextureFormat::BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case ETextureFormat::BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
        case ETextureFormat::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case ETextureFormat::BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case ETextureFormat::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case ETextureFormat::R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case ETextureFormat::R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

inline DXGI_FORMAT ToDXGIFormat(EIndexFormat format) {
    switch (format) {
        case EIndexFormat::UInt16: return DXGI_FORMAT_R16_UINT;
        case EIndexFormat::UInt32: return DXGI_FORMAT_R32_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// ============================================
// Buffer Usage Conversions
// ============================================

inline D3D11_BIND_FLAG ToD3D11BindFlags(EBufferUsage usage) {
    UINT flags = 0;
    if (usage & EBufferUsage::Vertex) flags |= D3D11_BIND_VERTEX_BUFFER;
    if (usage & EBufferUsage::Index) flags |= D3D11_BIND_INDEX_BUFFER;
    if (usage & EBufferUsage::Constant) flags |= D3D11_BIND_CONSTANT_BUFFER;
    if (usage & EBufferUsage::Structured) flags |= D3D11_BIND_SHADER_RESOURCE;
    if (usage & EBufferUsage::UnorderedAccess) flags |= D3D11_BIND_UNORDERED_ACCESS;
    return static_cast<D3D11_BIND_FLAG>(flags);
}

inline D3D11_BIND_FLAG ToD3D11BindFlags(ETextureUsage usage) {
    UINT flags = 0;
    if (usage & ETextureUsage::ShaderResource) flags |= D3D11_BIND_SHADER_RESOURCE;
    if (usage & ETextureUsage::RenderTarget) flags |= D3D11_BIND_RENDER_TARGET;
    if (usage & ETextureUsage::DepthStencil) flags |= D3D11_BIND_DEPTH_STENCIL;
    if (usage & ETextureUsage::UnorderedAccess) flags |= D3D11_BIND_UNORDERED_ACCESS;
    return static_cast<D3D11_BIND_FLAG>(flags);
}

inline D3D11_USAGE ToD3D11Usage(ECPUAccess cpuAccess) {
    switch (cpuAccess) {
        case ECPUAccess::None: return D3D11_USAGE_DEFAULT;
        case ECPUAccess::Read: return D3D11_USAGE_STAGING;
        case ECPUAccess::Write: return D3D11_USAGE_DYNAMIC;
        default: return D3D11_USAGE_DEFAULT;
    }
}

inline D3D11_CPU_ACCESS_FLAG ToD3D11CPUAccessFlags(ECPUAccess cpuAccess) {
    switch (cpuAccess) {
        case ECPUAccess::Read: return D3D11_CPU_ACCESS_READ;
        case ECPUAccess::Write: return D3D11_CPU_ACCESS_WRITE;
        default: return static_cast<D3D11_CPU_ACCESS_FLAG>(0);
    }
}

// ============================================
// Rasterizer State Conversions
// ============================================

inline D3D11_CULL_MODE ToD3D11CullMode(ECullMode mode) {
    switch (mode) {
        case ECullMode::None: return D3D11_CULL_NONE;
        case ECullMode::Front: return D3D11_CULL_FRONT;
        case ECullMode::Back: return D3D11_CULL_BACK;
        default: return D3D11_CULL_BACK;
    }
}

inline D3D11_FILL_MODE ToD3D11FillMode(EFillMode mode) {
    switch (mode) {
        case EFillMode::Solid: return D3D11_FILL_SOLID;
        case EFillMode::Wireframe: return D3D11_FILL_WIREFRAME;
        default: return D3D11_FILL_SOLID;
    }
}

// ============================================
// Depth Stencil State Conversions
// ============================================

inline D3D11_COMPARISON_FUNC ToD3D11ComparisonFunc(EComparisonFunc func) {
    switch (func) {
        case EComparisonFunc::Never: return D3D11_COMPARISON_NEVER;
        case EComparisonFunc::Less: return D3D11_COMPARISON_LESS;
        case EComparisonFunc::Equal: return D3D11_COMPARISON_EQUAL;
        case EComparisonFunc::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
        case EComparisonFunc::Greater: return D3D11_COMPARISON_GREATER;
        case EComparisonFunc::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
        case EComparisonFunc::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
        case EComparisonFunc::Always: return D3D11_COMPARISON_ALWAYS;
        default: return D3D11_COMPARISON_LESS;
    }
}

// ============================================
// Blend State Conversions
// ============================================

inline D3D11_BLEND ToD3D11Blend(EBlendFactor factor) {
    switch (factor) {
        case EBlendFactor::Zero: return D3D11_BLEND_ZERO;
        case EBlendFactor::One: return D3D11_BLEND_ONE;
        case EBlendFactor::SrcColor: return D3D11_BLEND_SRC_COLOR;
        case EBlendFactor::InvSrcColor: return D3D11_BLEND_INV_SRC_COLOR;
        case EBlendFactor::SrcAlpha: return D3D11_BLEND_SRC_ALPHA;
        case EBlendFactor::InvSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
        case EBlendFactor::DstColor: return D3D11_BLEND_DEST_COLOR;
        case EBlendFactor::InvDstColor: return D3D11_BLEND_INV_DEST_COLOR;
        case EBlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
        case EBlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return D3D11_BLEND_ONE;
    }
}

inline D3D11_BLEND_OP ToD3D11BlendOp(EBlendOp op) {
    switch (op) {
        case EBlendOp::Add: return D3D11_BLEND_OP_ADD;
        case EBlendOp::Subtract: return D3D11_BLEND_OP_SUBTRACT;
        case EBlendOp::RevSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
        case EBlendOp::Min: return D3D11_BLEND_OP_MIN;
        case EBlendOp::Max: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

// ============================================
// Sampler State Conversions
// ============================================

inline D3D11_FILTER ToD3D11Filter(EFilter filter) {
    switch (filter) {
        case EFilter::MinMagMipPoint: return D3D11_FILTER_MIN_MAG_MIP_POINT;
        case EFilter::MinMagPointMipLinear: return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
        case EFilter::MinPointMagLinearMipPoint: return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
        case EFilter::MinPointMagMipLinear: return D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
        case EFilter::MinLinearMagMipPoint: return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
        case EFilter::MinLinearMagPointMipLinear: return D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
        case EFilter::MinMagLinearMipPoint: return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        case EFilter::MinMagMipLinear: return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        case EFilter::Anisotropic: return D3D11_FILTER_ANISOTROPIC;
        case EFilter::ComparisonMinMagMipPoint: return D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        case EFilter::ComparisonMinMagMipLinear: return D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        case EFilter::ComparisonAnisotropic: return D3D11_FILTER_COMPARISON_ANISOTROPIC;
        default: return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

inline D3D11_TEXTURE_ADDRESS_MODE ToD3D11AddressMode(ETextureAddressMode mode) {
    switch (mode) {
        case ETextureAddressMode::Wrap: return D3D11_TEXTURE_ADDRESS_WRAP;
        case ETextureAddressMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case ETextureAddressMode::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case ETextureAddressMode::Border: return D3D11_TEXTURE_ADDRESS_BORDER;
        case ETextureAddressMode::MirrorOnce: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

// ============================================
// Primitive Topology Conversions
// ============================================

inline D3D11_PRIMITIVE_TOPOLOGY ToD3D11Topology(EPrimitiveTopology topology) {
    switch (topology) {
        case EPrimitiveTopology::PointList: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case EPrimitiveTopology::LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case EPrimitiveTopology::LineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case EPrimitiveTopology::TriangleList: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case EPrimitiveTopology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

// ============================================
// Vertex Input Element Conversions
// ============================================

inline const char* ToD3D11SemanticName(EVertexSemantic semantic) {
    switch (semantic) {
        case EVertexSemantic::Position: return "POSITION";
        case EVertexSemantic::Normal: return "NORMAL";
        case EVertexSemantic::Tangent: return "TANGENT";
        case EVertexSemantic::Texcoord: return "TEXCOORD";
        case EVertexSemantic::Color: return "COLOR";
        default: return "POSITION";
    }
}

inline DXGI_FORMAT ToD3D11VertexFormat(EVertexFormat format) {
    switch (format) {
        case EVertexFormat::Float: return DXGI_FORMAT_R32_FLOAT;
        case EVertexFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
        case EVertexFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case EVertexFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case EVertexFormat::UByte4: return DXGI_FORMAT_R8G8B8A8_UINT;
        case EVertexFormat::UByte4_Norm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default: return DXGI_FORMAT_R32G32B32_FLOAT;
    }
}

} // namespace DX11
} // namespace RHI
