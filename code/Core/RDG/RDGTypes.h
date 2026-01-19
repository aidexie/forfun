#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace RDG
{

//=============================================================================
// Handle System
//=============================================================================

template<typename Tag>
class RDGHandle
{
public:
    static constexpr uint32_t InvalidIndex = (1u << 20) - 1;

    RDGHandle() : m_Index(InvalidIndex), m_FrameId(0) {}

    RDGHandle(uint32_t index, uint32_t frameId, [[maybe_unused]] const char* debugName = nullptr)
        : m_Index(index), m_FrameId(frameId)
    {
#if _DEBUG
        m_DebugName = debugName;
#endif
    }

    bool IsValid() const { return m_Index != InvalidIndex; }
    uint32_t GetIndex() const { return m_Index; }
    uint32_t GetFrameId() const { return m_FrameId; }

    bool operator==(const RDGHandle& other) const
    {
        return m_Index == other.m_Index && m_FrameId == other.m_FrameId;
    }

    bool operator!=(const RDGHandle& other) const
    {
        return !(*this == other);
    }

#if _DEBUG
    const char* GetDebugName() const { return m_DebugName; }
#endif

private:
    uint32_t m_Index : 20;      // 1M resources per frame
    uint32_t m_FrameId : 12;    // Stale handle detection

#if _DEBUG
    const char* m_DebugName = nullptr;
#endif
};

// Type-safe handle aliases
struct RDGTextureTag {};
struct RDGBufferTag {};

using RDGTextureHandle = RDGHandle<RDGTextureTag>;
using RDGBufferHandle = RDGHandle<RDGBufferTag>;

//=============================================================================
// Pass Flags
//=============================================================================

enum class ERDGPassFlags : uint32_t
{
    None         = 0,
    Raster       = 1 << 0,   // Uses rasterization pipeline
    Compute      = 1 << 1,   // Uses compute pipeline
    Copy         = 1 << 2,   // Copy operations only
    AsyncCompute = 1 << 3,   // Can run on async compute queue
};

inline ERDGPassFlags operator|(ERDGPassFlags a, ERDGPassFlags b)
{
    return static_cast<ERDGPassFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ERDGPassFlags operator&(ERDGPassFlags a, ERDGPassFlags b)
{
    return static_cast<ERDGPassFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasFlag(ERDGPassFlags flags, ERDGPassFlags flag)
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

//=============================================================================
// Resource Access Flags
//=============================================================================

enum class ERDGResourceAccess : uint32_t
{
    None     = 0,
    Read     = 1 << 0,
    Write    = 1 << 1,
    ReadWrite = Read | Write,
};

inline ERDGResourceAccess operator|(ERDGResourceAccess a, ERDGResourceAccess b)
{
    return static_cast<ERDGResourceAccess>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

//=============================================================================
// Resource View Types
//=============================================================================

enum class ERDGViewType : uint8_t
{
    SRV,    // Shader Resource View
    UAV,    // Unordered Access View
    RTV,    // Render Target View
    DSV,    // Depth Stencil View
};

//=============================================================================
// Texture Descriptor
//=============================================================================

struct RDGTextureDesc
{
    uint32_t Width = 1;
    uint32_t Height = 1;
    uint16_t DepthOrArraySize = 1;
    uint16_t MipLevels = 1;
    DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t SampleCount = 1;
    D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;

    // Convenience constructors
    static RDGTextureDesc Create2D(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                   D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        RDGTextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format = format;
        desc.Flags = flags;
        return desc;
    }

    static RDGTextureDesc CreateRenderTarget(uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        return Create2D(width, height, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    }

    static RDGTextureDesc CreateDepthStencil(uint32_t width, uint32_t height,
                                              DXGI_FORMAT format = DXGI_FORMAT_D32_FLOAT)
    {
        return Create2D(width, height, format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    }

    static RDGTextureDesc CreateUAV(uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        return Create2D(width, height, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }

    // Convert to D3D12_RESOURCE_DESC
    D3D12_RESOURCE_DESC ToD3D12Desc() const
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = (DepthOrArraySize > 1 && Height == 1)
            ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = Width;
        desc.Height = Height;
        desc.DepthOrArraySize = DepthOrArraySize;
        desc.MipLevels = MipLevels;
        desc.Format = Format;
        desc.SampleDesc.Count = SampleCount;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = Flags;
        return desc;
    }
};

//=============================================================================
// Buffer Descriptor
//=============================================================================

struct RDGBufferDesc
{
    uint64_t SizeInBytes = 0;
    uint32_t StructureByteStride = 0;   // 0 for raw/typed buffers
    D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;

    static RDGBufferDesc CreateStructured(uint64_t elementCount, uint32_t stride,
                                          D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        RDGBufferDesc desc;
        desc.SizeInBytes = elementCount * stride;
        desc.StructureByteStride = stride;
        desc.Flags = flags;
        return desc;
    }

    static RDGBufferDesc CreateRaw(uint64_t sizeInBytes,
                                   D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        RDGBufferDesc desc;
        desc.SizeInBytes = sizeInBytes;
        desc.StructureByteStride = 0;
        desc.Flags = flags;
        return desc;
    }

    D3D12_RESOURCE_DESC ToD3D12Desc() const
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = SizeInBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = Flags;
        return desc;
    }
};

//=============================================================================
// Import Descriptor (for external resources)
//=============================================================================

struct RDGImportDesc
{
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES FinalState = D3D12_RESOURCE_STATE_COMMON;
};

//=============================================================================
// Resource Lifetime (computed during compile)
//=============================================================================

struct RDGResourceLifetime
{
    uint32_t FirstPassIndex = UINT32_MAX;
    uint32_t LastPassIndex = 0;
    uint64_t SizeInBytes = 0;
    uint64_t Alignment = 0;
};

//=============================================================================
// Aliasing Group (resources sharing same heap memory)
//=============================================================================

struct RDGAliasingGroup
{
    uint64_t HeapOffset = 0;
    uint64_t Size = 0;
    std::vector<uint32_t> ResourceIndices;  // Indices of resources in this group
};

} // namespace RDG
