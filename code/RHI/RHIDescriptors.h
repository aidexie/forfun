#pragma once
#include "RHICommon.h"
#include <vector>

// ============================================
// RHI Resource Descriptors
// ============================================

namespace RHI {

// ============================================
// Buffer Descriptor
// ============================================
struct BufferDesc {
    uint32_t size = 0;
    EBufferUsage usage = EBufferUsage::None;
    ECPUAccess cpuAccess = ECPUAccess::None;
    uint32_t structureByteStride = 0;  // For structured buffers
    const char* debugName = nullptr;

    BufferDesc() = default;
    BufferDesc(uint32_t size_, EBufferUsage usage_, ECPUAccess cpuAccess_ = ECPUAccess::None)
        : size(size_), usage(usage_), cpuAccess(cpuAccess_) {}
};

// ============================================
// Texture Descriptor
// ============================================
struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t arraySize = 1;
    uint32_t mipLevels = 1;
    ETextureFormat format = ETextureFormat::Unknown;
    ETextureUsage usage = ETextureUsage::ShaderResource;
    uint32_t sampleCount = 1;  // For MSAA
    const char* debugName = nullptr;

    TextureDesc() = default;

    static TextureDesc Texture2D(uint32_t w, uint32_t h, ETextureFormat fmt, ETextureUsage usage_ = ETextureUsage::ShaderResource) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = fmt;
        desc.usage = usage_;
        return desc;
    }

    static TextureDesc RenderTarget(uint32_t w, uint32_t h, ETextureFormat fmt) {
        return Texture2D(w, h, fmt, ETextureUsage::RenderTarget | ETextureUsage::ShaderResource);
    }

    static TextureDesc DepthStencil(uint32_t w, uint32_t h) {
        return Texture2D(w, h, ETextureFormat::D24_UNORM_S8_UINT, ETextureUsage::DepthStencil);
    }
};

// ============================================
// Sampler Descriptor
// ============================================
struct SamplerDesc {
    EFilter filter = EFilter::MinMagMipLinear;
    ETextureAddressMode addressU = ETextureAddressMode::Wrap;
    ETextureAddressMode addressV = ETextureAddressMode::Wrap;
    ETextureAddressMode addressW = ETextureAddressMode::Wrap;
    float mipLODBias = 0.0f;
    uint32_t maxAnisotropy = 1;
    EComparisonFunc comparisonFunc = EComparisonFunc::Never;
    float borderColor[4] = {0, 0, 0, 0};
    float minLOD = 0.0f;
    float maxLOD = 3.402823466e+38f;  // FLT_MAX
};

// ============================================
// Shader Descriptor
// ============================================
enum class EShaderType {
    Vertex,
    Pixel,
    Compute,
    Geometry,
    Hull,
    Domain
};

struct ShaderDesc {
    EShaderType type = EShaderType::Vertex;
    const void* bytecode = nullptr;
    size_t bytecodeSize = 0;
    const char* entryPoint = "main";  // For reflection, not used in compiled shader
    const char* debugName = nullptr;

    ShaderDesc() = default;
    ShaderDesc(EShaderType type_, const void* bytecode_, size_t size_)
        : type(type_), bytecode(bytecode_), bytecodeSize(size_) {}
};

// ============================================
// Pipeline State Descriptor
// ============================================
struct RasterizerDesc {
    ECullMode cullMode = ECullMode::Back;
    EFillMode fillMode = EFillMode::Solid;
    bool frontCounterClockwise = false;
    int depthBias = 0;
    float depthBiasClamp = 0.0f;
    float slopeScaledDepthBias = 0.0f;
    bool depthClipEnable = true;
    bool scissorEnable = false;
    bool multisampleEnable = false;
    bool antialiasedLineEnable = false;
};

struct DepthStencilDesc {
    bool depthEnable = true;
    bool depthWriteEnable = true;
    EComparisonFunc depthFunc = EComparisonFunc::Less;
    bool stencilEnable = false;
    uint8_t stencilReadMask = 0xFF;
    uint8_t stencilWriteMask = 0xFF;
    // TODO: Stencil ops if needed
};

struct BlendDesc {
    bool blendEnable = false;
    EBlendFactor srcBlend = EBlendFactor::One;
    EBlendFactor dstBlend = EBlendFactor::Zero;
    EBlendOp blendOp = EBlendOp::Add;
    EBlendFactor srcBlendAlpha = EBlendFactor::One;
    EBlendFactor dstBlendAlpha = EBlendFactor::Zero;
    EBlendOp blendOpAlpha = EBlendOp::Add;
    uint8_t renderTargetWriteMask = 0x0F;  // All channels
};

struct PipelineStateDesc {
    // Shaders
    class IShader* vertexShader = nullptr;
    class IShader* pixelShader = nullptr;
    class IShader* geometryShader = nullptr;  // Optional
    class IShader* hullShader = nullptr;      // Optional
    class IShader* domainShader = nullptr;    // Optional

    // Input layout
    std::vector<VertexElement> inputLayout;

    // Rasterizer state
    RasterizerDesc rasterizer;

    // Depth stencil state
    DepthStencilDesc depthStencil;

    // Blend state (per render target, but we simplify to one for now)
    BlendDesc blend;

    // Render target formats
    std::vector<ETextureFormat> renderTargetFormats;
    ETextureFormat depthStencilFormat = ETextureFormat::Unknown;

    // Primitive topology
    EPrimitiveTopology primitiveTopology = EPrimitiveTopology::TriangleList;

    // Debug name
    const char* debugName = nullptr;
};

// ============================================
// Compute Pipeline Descriptor
// ============================================
struct ComputePipelineDesc {
    class IShader* computeShader = nullptr;
    const char* debugName = nullptr;
};

} // namespace RHI
