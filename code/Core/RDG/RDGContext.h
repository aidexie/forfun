#pragma once

#include "RDGTypes.h"
#include "RDGBarrierBatcher.h"
#include <d3d12.h>
#include <unordered_map>
#include <vector>

namespace RDG
{

// Forward declarations
class CRDGBuilder;
struct RDGTextureEntry;
struct RDGBufferEntry;

//=============================================================================
// RDGContext - Execution context passed to pass lambdas
//=============================================================================

class RDGContext
{
public:
    RDGContext(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Device* device,
        const std::vector<RDGTextureEntry>& textures,
        const std::vector<RDGBufferEntry>& buffers);

    ~RDGContext();

    //-------------------------------------------------------------------------
    // Command List Access
    //-------------------------------------------------------------------------

    ID3D12GraphicsCommandList* GetCommandList() const { return m_CommandList; }

    //-------------------------------------------------------------------------
    // Resource Resolution (Handle -> GPU Resource)
    //-------------------------------------------------------------------------

    ID3D12Resource* GetResource(RDGTextureHandle handle) const;
    ID3D12Resource* GetResource(RDGBufferHandle handle) const;

    //-------------------------------------------------------------------------
    // Descriptor Access (creates descriptors on demand)
    //-------------------------------------------------------------------------

    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(RDGTextureHandle handle);
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(RDGBufferHandle handle);

    D3D12_CPU_DESCRIPTOR_HANDLE GetUAV(RDGTextureHandle handle);
    D3D12_CPU_DESCRIPTOR_HANDLE GetUAV(RDGBufferHandle handle);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(RDGTextureHandle handle);
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV(RDGTextureHandle handle);

    //-------------------------------------------------------------------------
    // GPU Descriptor Handle (for binding to shader)
    //-------------------------------------------------------------------------

    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUSRV(RDGTextureHandle handle);
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUUAV(RDGTextureHandle handle);

    //-------------------------------------------------------------------------
    // Convenience Methods
    //-------------------------------------------------------------------------

    // Set render targets (handles barrier transitions internally)
    void SetRenderTargets(
        const std::vector<RDGTextureHandle>& colorTargets,
        RDGTextureHandle depthTarget = RDGTextureHandle());

    // Clear render target
    void ClearRenderTarget(RDGTextureHandle handle, const float* clearColor);

    // Clear depth stencil
    void ClearDepthStencil(RDGTextureHandle handle, float depth = 1.0f, uint8_t stencil = 0);

private:
    ID3D12GraphicsCommandList* m_CommandList;
    ID3D12Device* m_Device;

    const std::vector<RDGTextureEntry>& m_Textures;
    const std::vector<RDGBufferEntry>& m_Buffers;

    // Descriptor caches (created on demand)
    struct DescriptorCache
    {
        std::unordered_map<uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE> SRVs;
        std::unordered_map<uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE> UAVs;
        std::unordered_map<uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE> RTVs;
        std::unordered_map<uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE> DSVs;
    };

    DescriptorCache m_TextureDescriptors;
    DescriptorCache m_BufferDescriptors;

    // Helper to create descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE CreateSRV(ID3D12Resource* resource, DXGI_FORMAT format);
    D3D12_CPU_DESCRIPTOR_HANDLE CreateUAV(ID3D12Resource* resource, DXGI_FORMAT format);
    D3D12_CPU_DESCRIPTOR_HANDLE CreateRTV(ID3D12Resource* resource, DXGI_FORMAT format);
    D3D12_CPU_DESCRIPTOR_HANDLE CreateDSV(ID3D12Resource* resource, DXGI_FORMAT format);
};

} // namespace RDG
