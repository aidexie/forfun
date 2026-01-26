#pragma once
#include "RHICommon.h"
#include "RHIResources.h"
#include <vector>
#include <initializer_list>

// ============================================
// Descriptor Set Abstraction
// ============================================
// Vulkan-style descriptor binding for DX12/Vulkan backends.
// DX11 is not supported (use legacy SetShaderResource calls).
//
// Frequency Model (4 sets):
// - Set 0 (space0): PerFrame - shadow maps, IBL, BRDF LUT
// - Set 1 (space1): PerPass - G-Buffer, post-process inputs
// - Set 2 (space2): PerMaterial - material textures
// - Set 3 (space3): PerDraw - object transforms (push constants)

namespace RHI {

// Forward declarations
class ITexture;
class IBuffer;
class ISampler;
class IAccelerationStructure;

// ============================================
// Shader Visibility (for descriptor bindings)
// ============================================
enum class EShaderVisibility : uint32_t {
    None = 0,
    Vertex = 1 << 0,
    Pixel = 1 << 1,
    Compute = 1 << 2,
    Geometry = 1 << 3,
    Hull = 1 << 4,
    Domain = 1 << 5,
    All = Vertex | Pixel | Compute | Geometry | Hull | Domain
};

inline EShaderVisibility operator|(EShaderVisibility a, EShaderVisibility b) {
    return static_cast<EShaderVisibility>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(EShaderVisibility a, EShaderVisibility b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// ============================================
// Descriptor Types
// ============================================
enum class EDescriptorType : uint8_t {
    Texture_SRV,              // Texture2D, TextureCube, Texture2DArray, etc.
    Buffer_SRV,               // StructuredBuffer, ByteAddressBuffer
    Texture_UAV,              // RWTexture2D, RWTexture3D
    Buffer_UAV,               // RWStructuredBuffer, RWByteAddressBuffer
    ConstantBuffer,           // Static constant buffer (pre-allocated IBuffer)
    VolatileCBV,              // Dynamic constant buffer (per-draw, ring allocated)
    PushConstants,            // Small inline data (DX12: root constants, Vulkan: push constants)
    Sampler,                  // SamplerState
    AccelerationStructure     // RaytracingAccelerationStructure (TLAS)
};

// ============================================
// BindingLayoutItem - Schema for one binding slot
// Use static factory methods for clean, readable API (NVRHI-style)
// ============================================
struct BindingLayoutItem {
    EDescriptorType type = EDescriptorType::Texture_SRV;
    uint32_t slot = 0;
    uint32_t count = 1;       // Array size
    uint32_t size = 0;        // For VolatileCBV/PushConstants: data size in bytes
    EShaderVisibility visibility = EShaderVisibility::All;

    // Static factory methods - NVRHI-style clean API
    static BindingLayoutItem Texture_SRV(uint32_t slot);
    static BindingLayoutItem Texture_SRVArray(uint32_t slot, uint32_t count);
    static BindingLayoutItem Buffer_SRV(uint32_t slot);
    static BindingLayoutItem Texture_UAV(uint32_t slot);
    static BindingLayoutItem Buffer_UAV(uint32_t slot);
    static BindingLayoutItem ConstantBuffer(uint32_t slot);
    static BindingLayoutItem VolatileCBV(uint32_t slot, uint32_t size);
    static BindingLayoutItem PushConstants(uint32_t slot, uint32_t size);
    static BindingLayoutItem Sampler(uint32_t slot);
    static BindingLayoutItem AccelerationStructure(uint32_t slot);
};

// ============================================
// BindingLayoutDesc - Fluent builder for layout
// ============================================
class BindingLayoutDesc {
public:
    explicit BindingLayoutDesc(const char* debugName = nullptr)
        : m_debugName(debugName) {}

    // Fluent API - chain AddItem() calls
    BindingLayoutDesc& AddItem(const BindingLayoutItem& item) {
        m_items.push_back(item);
        return *this;
    }

    BindingLayoutDesc& SetVisibility(EShaderVisibility visibility) {
        m_defaultVisibility = visibility;
        return *this;
    }

    // Getters
    const std::vector<BindingLayoutItem>& GetItems() const { return m_items; }
    const char* GetDebugName() const { return m_debugName; }
    EShaderVisibility GetDefaultVisibility() const { return m_defaultVisibility; }

private:
    std::vector<BindingLayoutItem> m_items;
    const char* m_debugName = nullptr;
    EShaderVisibility m_defaultVisibility = EShaderVisibility::All;
};

// ============================================
// IDescriptorSetLayout (Immutable, cached)
// ============================================
class IDescriptorSetLayout {
public:
    virtual ~IDescriptorSetLayout() = default;
    virtual uint32_t GetBindingCount() const = 0;
    virtual const BindingLayoutItem& GetBinding(uint32_t index) const = 0;
    virtual const char* GetDebugName() const = 0;

    // Query helpers for DX12 root signature construction
    virtual uint32_t GetSRVCount() const = 0;      // Texture_SRV + Buffer_SRV count
    virtual uint32_t GetUAVCount() const = 0;      // Texture_UAV + Buffer_UAV count
    virtual uint32_t GetSamplerCount() const = 0;
    virtual bool HasVolatileCBV() const = 0;
    virtual bool HasConstantBuffer() const = 0;
    virtual bool HasPushConstants() const = 0;
    virtual uint32_t GetVolatileCBVSize() const = 0;
    virtual uint32_t GetPushConstantSize() const = 0;
};

// ============================================
// BindingSetItem - Actual resource binding
// Use static factory methods mirroring BindingLayoutItem
// ============================================
struct BindingSetItem {
    uint32_t slot = 0;
    EDescriptorType type = EDescriptorType::Texture_SRV;

    // Resource union (only one valid based on type)
    ITexture* texture = nullptr;
    IBuffer* buffer = nullptr;
    ISampler* sampler = nullptr;
    IAccelerationStructure* accelStruct = nullptr;
    const void* volatileData = nullptr;
    uint32_t volatileDataSize = 0;
    uint32_t arraySlice = 0;    // For Texture_SRVSlice
    uint32_t mipLevel = 0;      // For UAV mip binding

    // Static factory methods - mirror BindingLayoutItem naming
    static BindingSetItem Texture_SRV(uint32_t slot, ITexture* tex);
    static BindingSetItem Texture_SRVSlice(uint32_t slot, ITexture* tex, uint32_t arraySlice);
    static BindingSetItem Buffer_SRV(uint32_t slot, IBuffer* buf);
    static BindingSetItem Texture_UAV(uint32_t slot, ITexture* tex, uint32_t mip = 0);
    static BindingSetItem Buffer_UAV(uint32_t slot, IBuffer* buf);
    static BindingSetItem ConstantBuffer(uint32_t slot, IBuffer* buf);
    static BindingSetItem VolatileCBV(uint32_t slot, const void* data, uint32_t size);
    static BindingSetItem PushConstants(uint32_t slot, const void* data, uint32_t size);
    static BindingSetItem Sampler(uint32_t slot, ISampler* samp);
    static BindingSetItem AccelerationStructure(uint32_t slot, IAccelerationStructure* as);
};

// ============================================
// IDescriptorSet - Mutable resource bindings
// ============================================
class IDescriptorSet {
public:
    virtual ~IDescriptorSet() = default;

    // Bind single resource
    virtual void Bind(const BindingSetItem& item) = 0;

    // Bind multiple resources at once
    virtual void Bind(const BindingSetItem* items, uint32_t count) = 0;

    // Convenience: bind initializer list
    void Bind(std::initializer_list<BindingSetItem> items) {
        Bind(items.begin(), static_cast<uint32_t>(items.size()));
    }

    virtual IDescriptorSetLayout* GetLayout() const = 0;
    virtual bool IsComplete() const = 0;
};

// ============================================
// IDescriptorSetAllocator
// ============================================
class IDescriptorSetAllocator {
public:
    virtual ~IDescriptorSetAllocator() = default;

    // Create layout - each call creates NEW instance (no caching)
    // User must manage layout lifetime and share instances explicitly
    // STRICT: Set's layout pointer must equal pipeline's expected layout pointer
    virtual IDescriptorSetLayout* CreateLayout(const BindingLayoutDesc& desc) = 0;

    // Destroy a layout created by CreateLayout
    virtual void DestroyLayout(IDescriptorSetLayout* layout) = 0;

    // Allocate descriptor set (user manages lifetime)
    virtual IDescriptorSet* AllocateSet(IDescriptorSetLayout* layout) = 0;

    // Free a descriptor set
    virtual void FreeSet(IDescriptorSet* set) = 0;
};

} // namespace RHI
