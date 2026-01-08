#pragma once
#include "RHICommon.h"
#include <vector>

// ============================================
// RHI Resource Descriptors
// ============================================

namespace RHI {

// ============================================
// Subresource Data (for multi-subresource texture creation)
// ============================================
struct SubresourceData {
    const void* pData = nullptr;   // Pointer to pixel data
    uint32_t rowPitch = 0;         // Row pitch in bytes
    uint32_t slicePitch = 0;       // Slice pitch (for 3D textures)
};

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
    uint32_t arraySize = 1;     // Number of textures in array (or number of cubes for cubemap arrays)
    uint32_t mipLevels = 1;
    ETextureFormat format = ETextureFormat::Unknown;
    ETextureDimension dimension = ETextureDimension::Tex2D;  // Texture dimension type
    ETextureUsage usage = ETextureUsage::ShaderResource;
    ECPUAccess cpuAccess = ECPUAccess::None;  // For staging textures: Read or Write
    ETextureMiscFlags miscFlags = ETextureMiscFlags::None;  // Misc flags (GenerateMips, etc.)
    uint32_t sampleCount = 1;   // For MSAA
    const char* debugName = nullptr;

    // View format overrides (for TYPELESS textures)
    // If Unknown, use the main format for view creation
    ETextureFormat rtvFormat = ETextureFormat::Unknown;  // RenderTargetView format
    ETextureFormat dsvFormat = ETextureFormat::Unknown;  // DepthStencilView format
    ETextureFormat srvFormat = ETextureFormat::Unknown;  // ShaderResourceView format
    ETextureFormat uavFormat = ETextureFormat::Unknown;  // UnorderedAccessView format

    // Optimized clear value (DX12 performance optimization)
    // Render targets cleared with this value will be faster
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // Default: black with alpha 0

    // Optimized depth clear value (DX12 performance optimization)
    // -1.0f means "auto" - will use UseReversedZ() to determine (0.0 or 1.0)
    // Set explicitly to 1.0f for shadow maps (always standard-Z)
    float depthClearValue = -1.0f;

    // Legacy compatibility (mapped to dimension)
    bool isCubemap = false;     // DEPRECATED: use dimension = TexCube
    bool isCubemapArray = false; // DEPRECATED: use dimension = TexCubeArray

    TextureDesc() = default;

    static TextureDesc Texture2D(uint32_t w, uint32_t h, ETextureFormat fmt, ETextureUsage usage_ = ETextureUsage::ShaderResource) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = fmt;
        desc.dimension = ETextureDimension::Tex2D;
        desc.usage = usage_;
        return desc;
    }

    static TextureDesc Texture3D(uint32_t w, uint32_t h, uint32_t d, ETextureFormat fmt, ETextureUsage usage_ = ETextureUsage::ShaderResource) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.depth = d;
        desc.format = fmt;
        desc.dimension = ETextureDimension::Tex3D;
        desc.usage = usage_;
        return desc;
    }

    static TextureDesc Texture2DArray(uint32_t w, uint32_t h, uint32_t arrayCount, ETextureFormat fmt, ETextureUsage usage_ = ETextureUsage::ShaderResource) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.arraySize = arrayCount;
        desc.format = fmt;
        desc.dimension = ETextureDimension::Tex2DArray;
        desc.usage = usage_;
        return desc;
    }

    static TextureDesc RenderTarget(uint32_t w, uint32_t h, ETextureFormat fmt) {
        return Texture2D(w, h, fmt, ETextureUsage::RenderTarget | ETextureUsage::ShaderResource);
    }

    static TextureDesc DepthStencil(uint32_t w, uint32_t h) {
        return Texture2D(w, h, ETextureFormat::D24_UNORM_S8_UINT, ETextureUsage::DepthStencil);
    }

    // LDR render target with TYPELESS texture for sRGB RTV + UNORM SRV
    // This enables proper gamma correction: GPU writes sRGB via RTV, shader reads linear via SRV
    static TextureDesc LDRRenderTarget(uint32_t w, uint32_t h) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = ETextureFormat::R8G8B8A8_TYPELESS;
        desc.dimension = ETextureDimension::Tex2D;
        desc.usage = ETextureUsage::RenderTarget | ETextureUsage::ShaderResource;
        desc.rtvFormat = ETextureFormat::R8G8B8A8_UNORM_SRGB;  // sRGB for gamma-correct writes
        desc.srvFormat = ETextureFormat::R8G8B8A8_UNORM;       // Linear for shader sampling
        return desc;
    }

    // Depth stencil with SRV access (for shadow mapping, etc.)
    // Uses standard-Z clear value (1.0) for shadow maps
    static TextureDesc DepthStencilWithSRV(uint32_t w, uint32_t h) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = ETextureFormat::R24G8_TYPELESS;
        desc.dimension = ETextureDimension::Tex2D;
        desc.usage = ETextureUsage::DepthStencil | ETextureUsage::ShaderResource;
        desc.dsvFormat = ETextureFormat::D24_UNORM_S8_UINT;
        desc.srvFormat = ETextureFormat::R24_UNORM_X8_TYPELESS;  // Read depth from R24G8
        desc.depthClearValue = 1.0f;  // Shadow maps always use standard-Z
        return desc;
    }

    // Depth stencil array with SRV access (for cascaded shadow mapping)
    // Uses standard-Z clear value (1.0) for shadow maps
    static TextureDesc DepthStencilArrayWithSRV(uint32_t w, uint32_t h, uint32_t arrayCount) {
        TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.arraySize = arrayCount;
        desc.format = ETextureFormat::R24G8_TYPELESS;
        desc.dimension = ETextureDimension::Tex2DArray;
        desc.usage = ETextureUsage::DepthStencil | ETextureUsage::ShaderResource;
        desc.dsvFormat = ETextureFormat::D24_UNORM_S8_UINT;
        desc.srvFormat = ETextureFormat::R24_UNORM_X8_TYPELESS;  // Read depth from R24G8
        desc.depthClearValue = 1.0f;  // Shadow maps always use standard-Z
        return desc;
    }

    // Cubemap texture (6 faces)
    static TextureDesc Cubemap(uint32_t size, ETextureFormat fmt, uint32_t mipLevels_ = 1) {
        TextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.format = fmt;
        desc.mipLevels = mipLevels_;
        desc.dimension = ETextureDimension::TexCube;
        desc.isCubemap = true;  // Legacy compatibility
        desc.usage = ETextureUsage::ShaderResource;
        return desc;
    }

    // Cubemap render target (6 faces, HDR format for accurate lighting)
    static TextureDesc CubemapRenderTarget(uint32_t size, ETextureFormat fmt = ETextureFormat::R16G16B16A16_FLOAT) {
        TextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.format = fmt;
        desc.mipLevels = 1;
        desc.dimension = ETextureDimension::TexCube;
        desc.isCubemap = true;  // Legacy compatibility
        desc.usage = ETextureUsage::RenderTarget | ETextureUsage::ShaderResource;
        return desc;
    }

    // Cubemap array (multiple cubemaps, for reflection probe arrays)
    // arrayCount: number of cubemaps in the array
    static TextureDesc CubemapArray(uint32_t size, uint32_t arrayCount, ETextureFormat fmt, uint32_t mipLevels_ = 1) {
        TextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.format = fmt;
        desc.mipLevels = mipLevels_;
        desc.arraySize = arrayCount;
        desc.dimension = ETextureDimension::TexCubeArray;
        desc.isCubemapArray = true;  // Legacy compatibility
        desc.usage = ETextureUsage::ShaderResource;
        return desc;
    }

    // Staging cubemap (for CPU write, then copy to GPU cubemap array)
    // arraySize = 6 faces
    static TextureDesc StagingCubemap(uint32_t size, ETextureFormat fmt, ECPUAccess access = ECPUAccess::Write) {
        TextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.format = fmt;
        desc.mipLevels = 1;
        desc.arraySize = 6;  // 6 faces
        desc.dimension = ETextureDimension::Tex2DArray;  // Staging cubemap is just a 2D array
        desc.usage = ETextureUsage::Staging;
        desc.cpuAccess = access;
        return desc;
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
    Domain,
    Library     // DXR shader library (DXIL)
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
