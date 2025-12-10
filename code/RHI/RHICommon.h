#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ============================================
// RHI Common Types and Enums
// ============================================

namespace RHI {

// ============================================
// Backend Type
// ============================================
enum class EBackend {
    DX11,
    DX12,
    // Vulkan,  // Future
    // Metal,   // Future
};

// ============================================
// Shader Stage
// ============================================
enum class EShaderStage : uint32_t {
    Vertex = 0,
    Pixel = 1,
    Compute = 2,
    Geometry = 3,
    Hull = 4,
    Domain = 5
};

// ============================================
// Resource Usage Flags
// ============================================
enum class EBufferUsage : uint32_t {
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Constant = 1 << 2,
    Structured = 1 << 3,
    UnorderedAccess = 1 << 4,
    IndirectArgs = 1 << 5
};

inline EBufferUsage operator|(EBufferUsage a, EBufferUsage b) {
    return static_cast<EBufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(EBufferUsage a, EBufferUsage b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

enum class ETextureUsage : uint32_t {
    None = 0,
    ShaderResource = 1 << 0,
    RenderTarget = 1 << 1,
    DepthStencil = 1 << 2,
    UnorderedAccess = 1 << 3
};

inline ETextureUsage operator|(ETextureUsage a, ETextureUsage b) {
    return static_cast<ETextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(ETextureUsage a, ETextureUsage b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// ============================================
// CPU Access
// ============================================
enum class ECPUAccess {
    None,
    Read,
    Write
};

// ============================================
// Texture Format
// ============================================
enum class ETextureFormat {
    Unknown,
    // Color formats
    R8_UNORM,
    R8G8B8A8_UNORM,
    R8G8B8A8_UNORM_SRGB,
    R16G16B16A16_FLOAT,
    R32G32B32A32_FLOAT,
    B8G8R8A8_UNORM,
    B8G8R8A8_UNORM_SRGB,
    // Depth formats
    D24_UNORM_S8_UINT,
    D32_FLOAT,
    // Compressed formats
    BC1_UNORM,
    BC1_UNORM_SRGB,
    BC3_UNORM,
    BC3_UNORM_SRGB,
    BC5_UNORM,
    BC7_UNORM,
    BC7_UNORM_SRGB,
    // Integer formats
    R32_UINT,
    R32G32_UINT,
    R32G32B32A32_UINT
};

// ============================================
// Index Format
// ============================================
enum class EIndexFormat {
    UInt16,
    UInt32
};

// ============================================
// Resource State (for DX12 barriers)
// ============================================
enum class EResourceState {
    Common,
    RenderTarget,
    DepthWrite,
    DepthRead,
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDest,
    Present
};

// ============================================
// Rasterizer State
// ============================================
enum class ECullMode {
    None,
    Front,
    Back
};

enum class EFillMode {
    Solid,
    Wireframe
};

// ============================================
// Depth Stencil State
// ============================================
enum class EComparisonFunc {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

// ============================================
// Blend State
// ============================================
enum class EBlendFactor {
    Zero,
    One,
    SrcColor,
    InvSrcColor,
    SrcAlpha,
    InvSrcAlpha,
    DstColor,
    InvDstColor,
    DstAlpha,
    InvDstAlpha
};

enum class EBlendOp {
    Add,
    Subtract,
    RevSubtract,
    Min,
    Max
};

// ============================================
// Sampler State
// ============================================
enum class EFilter {
    MinMagMipPoint,
    MinMagPointMipLinear,
    MinPointMagLinearMipPoint,
    MinPointMagMipLinear,
    MinLinearMagMipPoint,
    MinLinearMagPointMipLinear,
    MinMagLinearMipPoint,
    MinMagMipLinear,
    Anisotropic,
    ComparisonMinMagMipPoint,
    ComparisonMinMagMipLinear,
    ComparisonAnisotropic
};

enum class ETextureAddressMode {
    Wrap,
    Mirror,
    Clamp,
    Border,
    MirrorOnce
};

// ============================================
// Primitive Topology
// ============================================
enum class EPrimitiveTopology {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip
};

// ============================================
// Vertex Input Element
// ============================================
enum class EVertexSemantic {
    Position,
    Normal,
    Tangent,
    Texcoord,
    Color
};

enum class EVertexFormat {
    Float,
    Float2,
    Float3,
    Float4,
    UByte4,
    UByte4_Norm
};

struct VertexElement {
    EVertexSemantic semantic;
    uint32_t semanticIndex;
    EVertexFormat format;
    uint32_t offset;
    uint32_t inputSlot;
    bool instanceData;  // true for per-instance data

    VertexElement()
        : semantic(EVertexSemantic::Position)
        , semanticIndex(0)
        , format(EVertexFormat::Float3)
        , offset(0)
        , inputSlot(0)
        , instanceData(false)
    {}
};

} // namespace RHI
