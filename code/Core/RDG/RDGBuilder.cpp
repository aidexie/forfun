#include "RDGBuilder.h"
#include "Core/FFLog.h"

namespace RDG
{

//=============================================================================
// RDGPassBuilder Implementation
//=============================================================================

RDGPassBuilder::RDGPassBuilder(CRDGBuilder& builder, uint32_t passIndex)
    : m_Builder(builder)
    , m_PassIndex(passIndex)
{
}

RDGTextureHandle RDGPassBuilder::CreateTexture(const char* name, const RDGTextureDesc& desc)
{
    return m_Builder.CreateTexture(name, desc);
}

RDGBufferHandle RDGPassBuilder::CreateBuffer(const char* name, const RDGBufferDesc& desc)
{
    return m_Builder.CreateBuffer(name, desc);
}

RDGTextureHandle RDGPassBuilder::ReadTexture(RDGTextureHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] ReadTexture: Invalid handle");
        return handle;
    }

    m_Builder.RecordTextureAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::SRV, ERDGResourceAccess::Read);
    return handle;
}

RDGBufferHandle RDGPassBuilder::ReadBuffer(RDGBufferHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] ReadBuffer: Invalid handle");
        return handle;
    }

    m_Builder.RecordBufferAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::SRV, ERDGResourceAccess::Read);
    return handle;
}

void RDGPassBuilder::WriteRTV(RDGTextureHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] WriteRTV: Invalid handle");
        return;
    }

    m_Builder.RecordTextureAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::RTV, ERDGResourceAccess::Write);
}

void RDGPassBuilder::WriteDSV(RDGTextureHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] WriteDSV: Invalid handle");
        return;
    }

    m_Builder.RecordTextureAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::DSV, ERDGResourceAccess::Write);
}

void RDGPassBuilder::WriteUAV(RDGTextureHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] WriteUAV(Texture): Invalid handle");
        return;
    }

    m_Builder.RecordTextureAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::UAV, ERDGResourceAccess::Write);
}

void RDGPassBuilder::WriteUAV(RDGBufferHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] WriteUAV(Buffer): Invalid handle");
        return;
    }

    m_Builder.RecordBufferAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::UAV, ERDGResourceAccess::Write);
}

void RDGPassBuilder::ReadWriteUAV(RDGTextureHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] ReadWriteUAV(Texture): Invalid handle");
        return;
    }

    m_Builder.RecordTextureAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::UAV, ERDGResourceAccess::ReadWrite);
}

void RDGPassBuilder::ReadWriteUAV(RDGBufferHandle handle)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] ReadWriteUAV(Buffer): Invalid handle");
        return;
    }

    m_Builder.RecordBufferAccess(m_PassIndex, handle.GetIndex(), ERDGViewType::UAV, ERDGResourceAccess::ReadWrite);
}

//=============================================================================
// CRDGBuilder Implementation
//=============================================================================

CRDGBuilder::CRDGBuilder()
{
}

CRDGBuilder::~CRDGBuilder()
{
}

void CRDGBuilder::BeginFrame(uint32_t frameId)
{
    // Clear previous frame data
    m_Textures.clear();
    m_Buffers.clear();
    m_Passes.clear();
    m_ExecutionOrder.clear();
    m_AliasingGroups.clear();
    m_ExtractionRequests.clear();

    m_FrameId = frameId;
    m_IsCompiled = false;
}

//-----------------------------------------------------------------------------
// Resource Creation
//-----------------------------------------------------------------------------

RDGTextureHandle CRDGBuilder::CreateTexture(const char* name, const RDGTextureDesc& desc)
{
    uint32_t index = static_cast<uint32_t>(m_Textures.size());

    RDGTextureEntry entry;
    entry.Type = RDGTextureEntry::EType::Transient;
    entry.Name = name ? name : "Unnamed";
    entry.Desc = desc;

    m_Textures.push_back(std::move(entry));

    return RDGTextureHandle(index, m_FrameId, name);
}

RDGBufferHandle CRDGBuilder::CreateBuffer(const char* name, const RDGBufferDesc& desc)
{
    uint32_t index = static_cast<uint32_t>(m_Buffers.size());

    RDGBufferEntry entry;
    entry.Type = RDGBufferEntry::EType::Transient;
    entry.Name = name ? name : "Unnamed";
    entry.Desc = desc;

    m_Buffers.push_back(std::move(entry));

    return RDGBufferHandle(index, m_FrameId, name);
}

RDGTextureHandle CRDGBuilder::ImportTexture(
    const char* name,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_STATES finalState)
{
    if (!resource)
    {
        CFFLog::Error("[RDG] ImportTexture: null resource");
        return RDGTextureHandle();
    }

    uint32_t index = static_cast<uint32_t>(m_Textures.size());

    RDGTextureEntry entry;
    entry.Type = RDGTextureEntry::EType::Imported;
    entry.Name = name ? name : "ImportedTexture";
    entry.ImportDesc.Resource = resource;
    entry.ImportDesc.InitialState = initialState;
    entry.ImportDesc.FinalState = finalState;
    entry.ResolvedResource = resource;  // Already known for imported

    // Extract format and dimensions from resource desc
    D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
    entry.Desc.Width = static_cast<uint32_t>(resourceDesc.Width);
    entry.Desc.Height = resourceDesc.Height;
    entry.Desc.Format = resourceDesc.Format;
    entry.Desc.MipLevels = resourceDesc.MipLevels;
    entry.Desc.SampleCount = resourceDesc.SampleDesc.Count;
    entry.Desc.Flags = resourceDesc.Flags;

    m_Textures.push_back(std::move(entry));

    return RDGTextureHandle(index, m_FrameId, name);
}

RDGBufferHandle CRDGBuilder::ImportBuffer(
    const char* name,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_STATES finalState)
{
    if (!resource)
    {
        CFFLog::Error("[RDG] ImportBuffer: null resource");
        return RDGBufferHandle();
    }

    uint32_t index = static_cast<uint32_t>(m_Buffers.size());

    RDGBufferEntry entry;
    entry.Type = RDGBufferEntry::EType::Imported;
    entry.Name = name ? name : "ImportedBuffer";
    entry.ImportDesc.Resource = resource;
    entry.ImportDesc.InitialState = initialState;
    entry.ImportDesc.FinalState = finalState;
    entry.ResolvedResource = resource;  // Already known for imported

    // Extract size from resource desc
    D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
    entry.Desc.SizeInBytes = resourceDesc.Width;

    m_Buffers.push_back(std::move(entry));

    return RDGBufferHandle(index, m_FrameId, name);
}

void CRDGBuilder::ExtractTexture(
    RDGTextureHandle handle,
    ID3D12Resource** outResource,
    D3D12_RESOURCE_STATES finalState)
{
    if (!handle.IsValid())
    {
        CFFLog::Error("[RDG] ExtractTexture: Invalid handle");
        return;
    }

    if (handle.GetFrameId() != m_FrameId)
    {
        CFFLog::Error("[RDG] ExtractTexture: Stale handle from frame %u (current: %u)",
            handle.GetFrameId(), m_FrameId);
        return;
    }

    ExtractionRequest request;
    request.TextureIndex = handle.GetIndex();
    request.OutResource = outResource;
    request.FinalState = finalState;

    m_ExtractionRequests.push_back(request);
}

//-----------------------------------------------------------------------------
// Resource Access Recording
//-----------------------------------------------------------------------------

void CRDGBuilder::RecordTextureAccess(
    uint32_t passIndex,
    uint32_t textureIndex,
    ERDGViewType viewType,
    ERDGResourceAccess access)
{
    if (passIndex >= m_Passes.size())
    {
        CFFLog::Error("[RDG] RecordTextureAccess: Invalid pass index %u", passIndex);
        return;
    }

    if (textureIndex >= m_Textures.size())
    {
        CFFLog::Error("[RDG] RecordTextureAccess: Invalid texture index %u", textureIndex);
        return;
    }

    IRDGPass::ResourceAccess access_entry;
    access_entry.ResourceIndex = textureIndex;
    access_entry.ViewType = viewType;
    access_entry.Access = access;

    m_Passes[passIndex]->TextureAccesses.push_back(access_entry);
}

void CRDGBuilder::RecordBufferAccess(
    uint32_t passIndex,
    uint32_t bufferIndex,
    ERDGViewType viewType,
    ERDGResourceAccess access)
{
    if (passIndex >= m_Passes.size())
    {
        CFFLog::Error("[RDG] RecordBufferAccess: Invalid pass index %u", passIndex);
        return;
    }

    if (bufferIndex >= m_Buffers.size())
    {
        CFFLog::Error("[RDG] RecordBufferAccess: Invalid buffer index %u", bufferIndex);
        return;
    }

    IRDGPass::ResourceAccess access_entry;
    access_entry.ResourceIndex = bufferIndex;
    access_entry.ViewType = viewType;
    access_entry.Access = access;

    m_Passes[passIndex]->BufferAccesses.push_back(access_entry);
}

//-----------------------------------------------------------------------------
// Compilation & Execution (stubs for now - implemented in later phases)
//-----------------------------------------------------------------------------

void CRDGBuilder::Compile()
{
    if (m_IsCompiled)
    {
        CFFLog::Warning("[RDG] Graph already compiled");
        return;
    }

    CFFLog::Info("[RDG] Compiling graph: %zu passes, %zu textures, %zu buffers",
        m_Passes.size(), m_Textures.size(), m_Buffers.size());

    // TODO Phase 3: Build DAG and topological sort
    // TODO Phase 4: Lifetime analysis and memory aliasing
    // TODO Phase 5: Heap allocation for placed resources
    // TODO Phase 6: Barrier planning

    // For now, just use declaration order
    m_ExecutionOrder.clear();
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_Passes.size()); ++i)
    {
        m_ExecutionOrder.push_back(i);
    }

    m_IsCompiled = true;
}

void CRDGBuilder::Execute(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_IsCompiled)
    {
        CFFLog::Error("[RDG] Graph not compiled - call Compile() first");
        return;
    }

    if (!cmdList)
    {
        CFFLog::Error("[RDG] Execute: null command list");
        return;
    }

    CFFLog::Info("[RDG] Executing %zu passes", m_ExecutionOrder.size());

    // TODO Phase 7: Create RDGContext and execute passes
    // For now, this is a stub

    // Process extraction requests
    for (const auto& request : m_ExtractionRequests)
    {
        if (request.TextureIndex < m_Textures.size())
        {
            const auto& tex = m_Textures[request.TextureIndex];
            if (request.OutResource)
            {
                *request.OutResource = tex.ResolvedResource;
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Debug Helpers
//-----------------------------------------------------------------------------

void CRDGBuilder::DumpGraph() const
{
    CFFLog::Info("[RDG] === Graph Dump ===");
    CFFLog::Info("[RDG] Frame ID: %u", m_FrameId);
    CFFLog::Info("[RDG] Textures: %zu", m_Textures.size());

    for (size_t i = 0; i < m_Textures.size(); ++i)
    {
        const auto& tex = m_Textures[i];
        const char* typeStr = (tex.Type == RDGTextureEntry::EType::Transient) ? "Transient" : "Imported";
        CFFLog::Info("[RDG]   [%zu] %s (%s) %ux%u %s",
            i, tex.Name.c_str(), typeStr,
            tex.Desc.Width, tex.Desc.Height,
            tex.ResolvedResource ? "resolved" : "pending");
    }

    CFFLog::Info("[RDG] Buffers: %zu", m_Buffers.size());
    for (size_t i = 0; i < m_Buffers.size(); ++i)
    {
        const auto& buf = m_Buffers[i];
        const char* typeStr = (buf.Type == RDGBufferEntry::EType::Transient) ? "Transient" : "Imported";
        CFFLog::Info("[RDG]   [%zu] %s (%s) %llu bytes",
            i, buf.Name.c_str(), typeStr, buf.Desc.SizeInBytes);
    }

    CFFLog::Info("[RDG] Passes: %zu", m_Passes.size());
    for (size_t i = 0; i < m_Passes.size(); ++i)
    {
        const auto& pass = m_Passes[i];
        CFFLog::Info("[RDG]   [%zu] %s - %zu tex accesses, %zu buf accesses",
            i, pass->Name,
            pass->TextureAccesses.size(),
            pass->BufferAccesses.size());
    }

    CFFLog::Info("[RDG] === End Dump ===");
}

} // namespace RDG
