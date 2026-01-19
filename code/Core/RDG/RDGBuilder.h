#pragma once

#include "RDGTypes.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RDG
{

// Forward declarations
class CRDGBuilder;
class RDGContext;

//=============================================================================
// RDGPassBuilder - Used during pass setup to declare dependencies
//=============================================================================

class RDGPassBuilder
{
public:
    RDGPassBuilder(CRDGBuilder& builder, uint32_t passIndex);

    // Create transient resources (lifetime managed by RDG)
    RDGTextureHandle CreateTexture(const char* name, const RDGTextureDesc& desc);
    RDGBufferHandle CreateBuffer(const char* name, const RDGBufferDesc& desc);

    // Declare read dependencies
    RDGTextureHandle ReadTexture(RDGTextureHandle handle);
    RDGBufferHandle ReadBuffer(RDGBufferHandle handle);

    // Declare write dependencies
    void WriteRTV(RDGTextureHandle handle);
    void WriteDSV(RDGTextureHandle handle);
    void WriteUAV(RDGTextureHandle handle);
    void WriteUAV(RDGBufferHandle handle);

    // Read-write (for in-place operations)
    void ReadWriteUAV(RDGTextureHandle handle);
    void ReadWriteUAV(RDGBufferHandle handle);

private:
    CRDGBuilder& m_Builder;
    uint32_t m_PassIndex;
};

//=============================================================================
// Pass Base Class (type-erased for storage)
//=============================================================================

class IRDGPass
{
public:
    virtual ~IRDGPass() = default;
    virtual void Execute(RDGContext& context) = 0;

    const char* Name = "";
    ERDGPassFlags Flags = ERDGPassFlags::Raster;

    // Dependencies (populated during setup)
    struct ResourceAccess
    {
        uint32_t ResourceIndex;
        ERDGViewType ViewType;
        ERDGResourceAccess Access;
    };
    std::vector<ResourceAccess> TextureAccesses;
    std::vector<ResourceAccess> BufferAccesses;
};

//=============================================================================
// Typed Pass (stores PassData and execute lambda)
//=============================================================================

template<typename PassData>
class TRDGPass : public IRDGPass
{
public:
    using ExecuteFunc = std::function<void(const PassData&, RDGContext&)>;

    PassData Data;
    ExecuteFunc ExecuteFunction;

    void Execute(RDGContext& context) override
    {
        if (ExecuteFunction)
        {
            ExecuteFunction(Data, context);
        }
    }
};

//=============================================================================
// Internal Resource Storage
//=============================================================================

struct RDGTextureEntry
{
    enum class EType { Transient, Imported };

    EType Type = EType::Transient;
    std::string Name;

    // For transient resources
    RDGTextureDesc Desc;

    // For imported resources
    RDGImportDesc ImportDesc;

    // Resolved during compile/execute
    ID3D12Resource* ResolvedResource = nullptr;
    uint64_t HeapOffset = UINT64_MAX;
    RDGResourceLifetime Lifetime;
};

struct RDGBufferEntry
{
    enum class EType { Transient, Imported };

    EType Type = EType::Transient;
    std::string Name;

    // For transient resources
    RDGBufferDesc Desc;

    // For imported resources
    RDGImportDesc ImportDesc;

    // Resolved during compile/execute
    ID3D12Resource* ResolvedResource = nullptr;
    uint64_t HeapOffset = UINT64_MAX;
    RDGResourceLifetime Lifetime;
};

//=============================================================================
// CRDGBuilder - Main interface for building render graph
//=============================================================================

class CRDGBuilder
{
public:
    CRDGBuilder();
    ~CRDGBuilder();

    // Begin a new frame (resets all state)
    void BeginFrame(uint32_t frameId);

    //-------------------------------------------------------------------------
    // Resource Creation
    //-------------------------------------------------------------------------

    // Create transient texture (lifetime managed by RDG)
    RDGTextureHandle CreateTexture(const char* name, const RDGTextureDesc& desc);

    // Create transient buffer (lifetime managed by RDG)
    RDGBufferHandle CreateBuffer(const char* name, const RDGBufferDesc& desc);

    // Import external texture (caller manages lifetime)
    RDGTextureHandle ImportTexture(
        const char* name,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON);

    // Import external buffer (caller manages lifetime)
    RDGBufferHandle ImportBuffer(
        const char* name,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON);

    // Extract texture to keep alive after RDG execution
    void ExtractTexture(
        RDGTextureHandle handle,
        ID3D12Resource** outResource,
        D3D12_RESOURCE_STATES finalState);

    //-------------------------------------------------------------------------
    // Pass Registration
    //-------------------------------------------------------------------------

    template<typename PassData>
    void AddPass(
        const char* name,
        ERDGPassFlags flags,
        std::function<void(PassData&, RDGPassBuilder&)> setupFunc,
        std::function<void(const PassData&, RDGContext&)> executeFunc)
    {
        auto pass = std::make_unique<TRDGPass<PassData>>();
        pass->Name = name;
        pass->Flags = flags;
        pass->ExecuteFunction = std::move(executeFunc);

        uint32_t passIndex = static_cast<uint32_t>(m_Passes.size());

        // Add pass BEFORE calling setup so RecordTextureAccess can find it
        m_Passes.push_back(std::move(pass));

        RDGPassBuilder builder(*this, passIndex);
        auto* passPtr = static_cast<TRDGPass<PassData>*>(m_Passes.back().get());
        setupFunc(passPtr->Data, builder);
    }

    // Convenience overload for raster passes
    template<typename PassData>
    void AddPass(
        const char* name,
        std::function<void(PassData&, RDGPassBuilder&)> setupFunc,
        std::function<void(const PassData&, RDGContext&)> executeFunc)
    {
        AddPass<PassData>(name, ERDGPassFlags::Raster, std::move(setupFunc), std::move(executeFunc));
    }

    //-------------------------------------------------------------------------
    // Compilation & Execution
    //-------------------------------------------------------------------------

    // Compile the graph (analyze dependencies, allocate memory, plan barriers)
    void Compile();

    // Execute all passes
    void Execute(ID3D12GraphicsCommandList* cmdList);

    //-------------------------------------------------------------------------
    // Accessors (for internal use)
    //-------------------------------------------------------------------------

    uint32_t GetFrameId() const { return m_FrameId; }
    const std::vector<RDGTextureEntry>& GetTextures() const { return m_Textures; }
    const std::vector<RDGBufferEntry>& GetBuffers() const { return m_Buffers; }
    const std::vector<std::unique_ptr<IRDGPass>>& GetPasses() const { return m_Passes; }

    // Record resource access (called by RDGPassBuilder)
    void RecordTextureAccess(uint32_t passIndex, uint32_t textureIndex,
                             ERDGViewType viewType, ERDGResourceAccess access);
    void RecordBufferAccess(uint32_t passIndex, uint32_t bufferIndex,
                            ERDGViewType viewType, ERDGResourceAccess access);

    // Debug
    void DumpGraph() const;

private:
    uint32_t m_FrameId = 0;

    std::vector<RDGTextureEntry> m_Textures;
    std::vector<RDGBufferEntry> m_Buffers;
    std::vector<std::unique_ptr<IRDGPass>> m_Passes;

    // Extraction requests (resources to keep alive after execution)
    struct ExtractionRequest
    {
        uint32_t TextureIndex;
        ID3D12Resource** OutResource;
        D3D12_RESOURCE_STATES FinalState;
    };
    std::vector<ExtractionRequest> m_ExtractionRequests;

    // Compiled data
    bool m_IsCompiled = false;
    std::vector<uint32_t> m_ExecutionOrder;
    std::vector<RDGAliasingGroup> m_AliasingGroups;
};

} // namespace RDG
