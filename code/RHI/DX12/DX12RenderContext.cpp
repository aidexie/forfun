#include "DX12RenderContext.h"
#include "DX12Common.h"
#include "DX12Context.h"
#include "DX12Resources.h"
#include "DX12DescriptorHeap.h"
#include "DX12UploadManager.h"
#include "DX12PipelineState.h"
#include "DX12AccelerationStructure.h"
#include "DX12RayTracingPipeline.h"
#include "DX12ShaderBindingTable.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// Constructor / Destructor
// ============================================

CDX12RenderContext::CDX12RenderContext() {
}

CDX12RenderContext::~CDX12RenderContext() {
    Shutdown();
}

// ============================================
// Lifecycle
// ============================================

bool CDX12RenderContext::Initialize(void* nativeWindowHandle, uint32_t width, uint32_t height) {
    HWND hwnd = static_cast<HWND>(nativeWindowHandle);

    // Initialize DX12 context (device, swapchain, etc.)
    if (!CDX12Context::Instance().Initialize(hwnd, width, height)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize DX12Context");
        return false;
    }

    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Initialize descriptor heap manager
    if (!CDX12DescriptorHeapManager::Instance().Initialize(device)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize descriptor heap manager");
        return false;
    }

    // Initialize upload manager
    if (!CDX12UploadManager::Instance().Initialize(device)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize upload manager");
        return false;
    }

    // Initialize PSO cache
    if (!CDX12PSOCache::Instance().Initialize(device)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize PSO cache");
        return false;
    }

    // Create root signatures
    if (!CreateRootSignatures()) {
        CFFLog::Error("[DX12RenderContext] Failed to create root signatures");
        return false;
    }

    // Create command list
    m_commandList = std::make_unique<CDX12CommandList>(this);
    if (!m_commandList->Initialize()) {
        CFFLog::Error("[DX12RenderContext] Failed to create command list");
        return false;
    }

    // Create backbuffer wrappers
    CreateBackbufferWrappers();

    // Create depth stencil buffer
    CreateDepthStencilBuffer();

    // Create dynamic constant buffer ring
    // 4MB per frame should be enough for ~16000 draws with 256-byte CBs
    m_dynamicBufferRing = std::make_unique<CDX12DynamicBufferRing>();
    if (!m_dynamicBufferRing->Initialize(device, 4 * 1024 * 1024, NUM_FRAMES_IN_FLIGHT)) {
        CFFLog::Error("[DX12RenderContext] Failed to initialize dynamic buffer ring");
        return false;
    }

    // Set dynamic buffer ring on command list
    m_commandList->SetDynamicBufferRing(m_dynamicBufferRing.get());

    CFFLog::Info("[DX12RenderContext] Initialized successfully");
    return true;
}

void CDX12RenderContext::Shutdown() {
    // Wait for GPU to finish
    //CDX12Context::Instance().WaitForGPU();

    // Shutdown internal passes first (they hold GPU resources)
    m_generateMipsPass.Shutdown();

    // Release resources
    m_dynamicBufferRing.reset();
    m_depthStencilBuffer.reset();
    ReleaseBackbufferWrappers();
    m_commandList.reset();

    m_graphicsRootSignature.Reset();
    m_computeRootSignature.Reset();

    // Shutdown managers
    CDX12PSOCache::Instance().Shutdown();
    CDX12UploadManager::Instance().Shutdown();
    CDX12DescriptorHeapManager::Instance().Shutdown();
    CDX12Context::Instance().Shutdown();

    CFFLog::Info("[DX12RenderContext] Shutdown complete");
}

void CDX12RenderContext::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    // Wait for GPU
    CDX12Context::Instance().WaitForGPU();

    // Release depth stencil and backbuffer wrappers
    ReleaseDepthStencilBuffer();
    ReleaseBackbufferWrappers();

    // Resize swapchain
    CDX12Context::Instance().OnResize(width, height);

    // Recreate backbuffer wrappers and depth stencil
    CreateBackbufferWrappers();
    CreateDepthStencilBuffer();
}

// ============================================
// Frame Control
// ============================================

void CDX12RenderContext::BeginFrame() {
    if (m_frameInProgress) {
        CFFLog::Warning("[DX12RenderContext] BeginFrame called while frame in progress");
        return;
    }

    // Advance dynamic buffer ring to current frame's region
    uint32_t frameIndex = CDX12Context::Instance().GetFrameIndex();
    m_dynamicBufferRing->BeginFrame(frameIndex);

    // Reset descriptor staging rings for this frame
    CDX12DescriptorHeapManager::Instance().BeginFrame(frameIndex);

    // Reset command list with current frame's allocator
    m_commandList->Reset(CDX12Context::Instance().GetCurrentCommandAllocator());

    // Transition backbuffer from PRESENT to RENDER_TARGET
    ID3D12Resource* backbuffer = CDX12Context::Instance().GetCurrentBackbuffer();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->GetNativeCommandList()->ResourceBarrier(1, &barrier);

    // Update backbuffer wrapper's tracked state to match
    if (m_backbufferWrappers[frameIndex]) {
        m_backbufferWrappers[frameIndex]->SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    m_frameInProgress = true;
}

void CDX12RenderContext::EndFrame() {
    if (!m_frameInProgress) {
        CFFLog::Warning("[DX12RenderContext] EndFrame called without BeginFrame");
        return;
    }

    // Update backbuffer wrapper's tracked state before transition
    uint32_t frameIndex = CDX12Context::Instance().GetFrameIndex();
    if (m_backbufferWrappers[frameIndex]) {
        m_backbufferWrappers[frameIndex]->SetCurrentState(D3D12_RESOURCE_STATE_PRESENT);
    }

    // Transition backbuffer to present state
    ID3D12Resource* backbuffer = CDX12Context::Instance().GetCurrentBackbuffer();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->GetNativeCommandList()->ResourceBarrier(1, &barrier);

    // Close command list
    m_commandList->Close();

    // Execute command list
    ID3D12CommandList* cmdLists[] = { m_commandList->GetNativeCommandList() };
    CDX12Context::Instance().GetCommandQueue()->ExecuteCommandLists(1, cmdLists);

    // Signal fence for upload manager
    uint64_t fenceValue = CDX12Context::Instance().SignalFence();
    CDX12UploadManager::Instance().FinishUploads(fenceValue);

    m_frameInProgress = false;
}

void CDX12RenderContext::Present(bool vsync) {
    auto& context = CDX12Context::Instance();

    UINT syncInterval = vsync ? 1 : 0;
    UINT presentFlags = 0;

    HRESULT hr = DX12_CHECK(context.GetSwapChain()->Present(syncInterval, presentFlags));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] Present failed: %s", HRESULTToString(hr).c_str());

        // If device removed, get the actual reason
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            HRESULT removeReason = context.GetDevice()->GetDeviceRemovedReason();
            CFFLog::Error("[DX12RenderContext] Device removed reason: %s", HRESULTToString(removeReason).c_str());

            // Try to get DRED data for more detailed diagnostics
            ComPtr<ID3D12DeviceRemovedExtendedData> dred;
            if (SUCCEEDED(context.GetDevice()->QueryInterface(IID_PPV_ARGS(&dred)))) {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
                if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
                    CFFLog::Error("[DX12RenderContext] DRED Breadcrumbs:");
                    const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                    while (node) {
                        if (node->pCommandListDebugNameW) {
                            CFFLog::Error("  CommandList: %S", node->pCommandListDebugNameW);
                        }
                        if (node->pLastBreadcrumbValue && node->pCommandHistory) {
                            uint32_t lastOp = *node->pLastBreadcrumbValue;
                            if (lastOp > 0 && lastOp <= node->BreadcrumbCount) {
                                CFFLog::Error("  Last completed op index: %u / %u", lastOp, node->BreadcrumbCount);
                                CFFLog::Error("  Last op type: %d", node->pCommandHistory[lastOp - 1]);
                            }
                        }
                        node = node->pNext;
                    }
                }

                D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
                if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault))) {
                    if (pageFault.PageFaultVA != 0) {
                        CFFLog::Error("[DX12RenderContext] DRED Page Fault at VA: 0x%llX", pageFault.PageFaultVA);
                    }
                }
            }
        }
    }

    // Move to next frame
    context.MoveToNextFrame();

    // Process completed uploads
    uint64_t completedValue = context.GetCurrentFenceValue();
    CDX12UploadManager::Instance().ProcessCompletedUploads(completedValue);
}

// ============================================
// Command List Access
// ============================================

ICommandList* CDX12RenderContext::GetCommandList() {
    return m_commandList.get();
}

// ============================================
// Resource Creation
// ============================================

IBuffer* CDX12RenderContext::CreateBuffer(const BufferDesc& desc, const void* initialData) {
    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Determine heap type
    D3D12_HEAP_TYPE heapType = GetHeapType(desc.cpuAccess, desc.usage);

    // Buffer size must be aligned for constant buffers
    uint32_t alignedSize = desc.size;
    if (desc.usage & EBufferUsage::Constant) {
        alignedSize = AlignUp(desc.size, CONSTANT_BUFFER_ALIGNMENT);
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = alignedSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Resource flags
    if (desc.usage & EBufferUsage::UnorderedAccess) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    // Acceleration structure buffers also need UAV flag
    if (desc.usage & EBufferUsage::AccelerationStructure) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    // Determine initial state
    D3D12_RESOURCE_STATES initialState = GetInitialResourceState(heapType, desc.usage);
    // Acceleration structure buffers must be created in RAYTRACING_ACCELERATION_STRUCTURE state
    if (desc.usage & EBufferUsage::AccelerationStructure) {
        initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }

     ComPtr<ID3D12Resource> resource;
    HRESULT hr = DX12_CHECK(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource)
    ));
    if (FAILED(hr))
    {
        CFFLog::Error("[DX12RenderContext] CreateBuffer failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        resource->SetName(wname);
    }

    // Create buffer wrapper
    CDX12Buffer* buffer = new CDX12Buffer(resource.Get(), desc, device);
    //resource.Detach();  // Transfer ownership

    // Upload initial data if provided
    if (initialData && heapType == D3D12_HEAP_TYPE_UPLOAD) {
        void* mapped = buffer->Map();
        if (mapped) {
            memcpy(mapped, initialData, desc.size);
            buffer->Unmap();
        }
    } else if (initialData && heapType == D3D12_HEAP_TYPE_DEFAULT) {
        // Upload via staging buffer
        CDX12CommandList* cmdList = m_commandList.get();
        ID3D12GraphicsCommandList* d3dCmdList = cmdList->GetNativeCommandList();

        // Allocate upload buffer
        auto& uploadMgr = CDX12UploadManager::Instance();
        UploadAllocation uploadAlloc = uploadMgr.Allocate(alignedSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if (!uploadAlloc.IsValid()) {
            CFFLog::Error("[DX12RenderContext] Failed to allocate upload buffer for buffer data");
            return buffer;  // Return buffer without initial data
        }

        // Copy data to upload buffer
        memcpy(uploadAlloc.cpuAddress, initialData, desc.size);

        // Transition buffer to COPY_DEST state
        D3D12_RESOURCE_STATES currentState = buffer->GetCurrentState();
        if (currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = buffer->GetD3D12Resource();
            barrier.Transition.StateBefore = currentState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            d3dCmdList->ResourceBarrier(1, &barrier);
        }

        // Copy from upload buffer to default buffer
        d3dCmdList->CopyBufferRegion(
            buffer->GetD3D12Resource(), 0,
            uploadAlloc.resource, uploadAlloc.offset,
            desc.size
        );

        // Transition buffer to appropriate state based on usage
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
        if (desc.usage & EBufferUsage::Constant) {
            finalState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        } else if (desc.usage & EBufferUsage::Vertex) {
            finalState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        } else if (desc.usage & EBufferUsage::Index) {
            finalState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        } else if (desc.usage & EBufferUsage::Structured) {
            // Structured buffers need NON_PIXEL for compute/DXR access
            finalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        } else if (desc.usage & EBufferUsage::UnorderedAccess) {
            finalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer->GetD3D12Resource();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d3dCmdList->ResourceBarrier(1, &barrier);
        buffer->SetCurrentState(finalState);
    }

    return buffer;
}

ITexture* CDX12RenderContext::CreateTexture(const TextureDesc& desc, const void* initialData) {
    if (initialData) {
        SubresourceData subresource;
        subresource.pData = initialData;
        subresource.rowPitch = desc.width * GetBytesPerPixel(desc.format);
        subresource.slicePitch = subresource.rowPitch * desc.height;
        return CreateTextureInternal(desc, &subresource, 1);
    }
    return CreateTextureInternal(desc, nullptr, 0);
}

ITexture* CDX12RenderContext::CreateTextureWithData(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources) {
    return CreateTextureInternal(desc, subresources, numSubresources);
}

ITexture* CDX12RenderContext::CreateTextureInternal(const TextureDesc& desc, const SubresourceData* subresources, uint32_t numSubresources) {
    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Determine resource dimension
    D3D12_RESOURCE_DIMENSION dimension;
    uint32_t arraySize = desc.arraySize;

    switch (desc.dimension) {
        case ETextureDimension::Tex2D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        case ETextureDimension::Tex3D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            break;
        case ETextureDimension::TexCube:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            arraySize = 6;
            break;
        case ETextureDimension::Tex2DArray:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        case ETextureDimension::TexCubeArray:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            arraySize = desc.arraySize * 6;
            break;
        default:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = (desc.usage & ETextureUsage::Staging) ?
                     ((desc.cpuAccess == ECPUAccess::Read) ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD) :
                     D3D12_HEAP_TYPE_DEFAULT;

    // DX12: Staging textures (UPLOAD/READBACK) must be buffers, not textures
    // For texture staging, we create a buffer with appropriate size instead
    if (heapProps.Type == D3D12_HEAP_TYPE_UPLOAD || heapProps.Type == D3D12_HEAP_TYPE_READBACK) {
        // Calculate required buffer size for the texture data
        UINT64 totalSize = 0;
        D3D12_RESOURCE_DESC tempDesc = {};
        tempDesc.Dimension = dimension;
        tempDesc.Width = desc.width;
        tempDesc.Height = desc.height;
        tempDesc.DepthOrArraySize = (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? desc.depth : static_cast<UINT16>(arraySize);
        tempDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        tempDesc.Format = ToDXGIFormat(desc.format);
        tempDesc.SampleDesc.Count = 1;
        tempDesc.SampleDesc.Quality = 0;
        tempDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tempDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        device->GetCopyableFootprints(&tempDesc, 0, desc.mipLevels * arraySize, 0, nullptr, nullptr, nullptr, &totalSize);

        // Create buffer for staging
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = totalSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_RESOURCE_STATES initialState = (heapProps.Type == D3D12_HEAP_TYPE_READBACK) ?
            D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ;

        ComPtr<ID3D12Resource> stagingBuffer;
        HRESULT hr = DX12_CHECK(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&stagingBuffer)
        ));

        if (FAILED(hr)) {
            CFFLog::Error("[DX12RenderContext] CreateTexture (staging buffer) failed: %s", HRESULTToString(hr).c_str());
            return nullptr;
        }

        if (desc.debugName) {
            wchar_t wname[128];
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
            stagingBuffer->SetName(wname);
        }

        // Create texture wrapper with staging buffer
        // Note: This is a staging buffer, not a real texture - it will need special handling
        CDX12Texture* texture = new CDX12Texture(stagingBuffer.Get(), desc, device);
        //stagingBuffer.Detach();
        return texture;
    }

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = dimension;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? desc.depth : static_cast<UINT16>(arraySize);
    resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resourceDesc.Format = ToDXGIFormat(desc.format);
    resourceDesc.SampleDesc.Count = desc.sampleCount;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = GetResourceFlags(desc.usage);

    // GenerateMips support: Add UAV flag and handle SRGB format conversion
    const bool needsGenerateMips = (desc.miscFlags & ETextureMiscFlags::GenerateMips);
    DXGI_FORMAT srvFormat = resourceDesc.Format;  // Default: same as resource
    DXGI_FORMAT uavFormat = resourceDesc.Format;  // Default: same as resource

    if (needsGenerateMips) {
        // Add UAV flag for mipmap generation via compute shader
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        // Handle SRGB formats: UAV doesn't support SRGB, so use TYPELESS resource
        // with SRGB view for SRV and UNORM view for UAV
        switch (resourceDesc.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                resourceDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
                srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // SRV with sRGB for correct sampling
                uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;       // UAV must be UNORM
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                resourceDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                uavFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
                break;
            // Non-SRGB formats: use same format for SRV and UAV
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                // These formats support UAV directly
                break;
            default:
                CFFLog::Warning("[DX12] GenerateMips requested for unsupported format: %d", resourceDesc.Format);
                break;
        }
    }

    D3D12_RESOURCE_STATES initialState = GetInitialResourceState(heapProps.Type, desc.usage);

    // Clear value for render targets / depth stencil
    // NOTE: For TYPELESS resources (e.g., GenerateMips with SRGB), we must use a concrete format
    D3D12_CLEAR_VALUE* pClearValue = nullptr;
    D3D12_CLEAR_VALUE clearValue = {};
    if (desc.usage & ETextureUsage::RenderTarget) {
        // For TYPELESS resources with GenerateMips, use the SRV format which is concrete
        if (needsGenerateMips && srvFormat != resourceDesc.Format) {
            clearValue.Format = srvFormat;  // Use concrete SRGB format
        } else {
            clearValue.Format = (desc.rtvFormat != ETextureFormat::Unknown) ? ToDXGIFormat(desc.rtvFormat) : resourceDesc.Format;
        }
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;
        pClearValue = &clearValue;
    } else if (desc.usage & ETextureUsage::DepthStencil) {
        clearValue.Format = (desc.dsvFormat != ETextureFormat::Unknown) ? ToDXGIFormat(desc.dsvFormat) : resourceDesc.Format;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;
        pClearValue = &clearValue;
    }

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = DX12_CHECK(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        pClearValue,
        IID_PPV_ARGS(&resource)
    ));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateTexture failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        resource->SetName(wname);
    }

    // Create texture wrapper with format overrides for GenerateMips
    TextureDesc finalDesc = desc;
    if (needsGenerateMips) {
        // Update usage to reflect actual UAV capability
        // (TextureLoader may only set RenderTarget for DX11 compatibility)
        finalDesc.usage = finalDesc.usage | ETextureUsage::UnorderedAccess;

        if (srvFormat != resourceDesc.Format) {
            // Override SRV/UAV formats for TYPELESS resource
            finalDesc.srvFormat = FromDXGIFormat(srvFormat);
            finalDesc.uavFormat = FromDXGIFormat(uavFormat);
        }
    }

    // If mipLevels was 0, DX12 auto-calculated the actual mip count
    // Query the actual value from the created resource
    if (desc.mipLevels == 0) {
        D3D12_RESOURCE_DESC actualDesc = resource->GetDesc();
        finalDesc.mipLevels = actualDesc.MipLevels;
        CFFLog::Info("[DX12] Auto-calculated mip levels: %u (for %ux%u texture)",
                     finalDesc.mipLevels, desc.width, desc.height);
    }

    CDX12Texture* texture = new CDX12Texture(resource.Get(), finalDesc, device);
    // resource.Detach();

    // Upload initial data if provided
    if (subresources && numSubresources > 0 && heapProps.Type == D3D12_HEAP_TYPE_DEFAULT) {
        // Get command list for upload
        CDX12CommandList* cmdList = m_commandList.get();
        ID3D12GraphicsCommandList* d3dCmdList = cmdList->GetNativeCommandList();

        // Get texture resource and desc for footprint calculation
        ID3D12Resource* dstResource = texture->GetD3D12Resource();
        D3D12_RESOURCE_DESC dstDesc = dstResource->GetDesc();

        // Calculate total required upload buffer size and footprints
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(numSubresources);
        std::vector<UINT> numRows(numSubresources);
        std::vector<UINT64> rowSizeInBytes(numSubresources);
        UINT64 totalSize = 0;

        device->GetCopyableFootprints(
            &dstDesc,
            0,                      // FirstSubresource
            numSubresources,        // NumSubresources
            0,                      // BaseOffset (will be adjusted per allocation)
            footprints.data(),
            numRows.data(),
            rowSizeInBytes.data(),
            &totalSize
        );

        // Allocate upload buffer
        auto& uploadMgr = CDX12UploadManager::Instance();
        UploadAllocation uploadAlloc = uploadMgr.Allocate(totalSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        if (!uploadAlloc.IsValid()) {
            CFFLog::Error("[DX12RenderContext] Failed to allocate upload buffer for texture data");
            return texture;  // Return texture without initial data
        }

        // Transition texture to COPY_DEST state
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dstResource;
        barrier.Transition.StateBefore = texture->GetCurrentState();
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
            d3dCmdList->ResourceBarrier(1, &barrier);
        }

        // Copy each subresource
        for (uint32_t i = 0; i < numSubresources; ++i) {
            // Get original footprint offset (before adjustment)
            UINT64 originalOffset = footprints[i].Offset;

            // Adjust footprint offset relative to our allocation
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[i];
            footprint.Offset += uploadAlloc.offset;

            // Copy source data to upload buffer
            uint8_t* uploadDst = static_cast<uint8_t*>(uploadAlloc.cpuAddress) + originalOffset;
            const uint8_t* srcData = static_cast<const uint8_t*>(subresources[i].pData);

            // For 3D textures: numRows[i] = height (rows per slice), footprint.Footprint.Depth = depth
            // For 2D textures: numRows[i] = height, footprint.Footprint.Depth = 1
            UINT textureDepth = footprint.Footprint.Depth;
            UINT rowsPerSlice = numRows[i];  // numRows is already per-slice (= height)
            UINT64 dstSlicePitch = static_cast<UINT64>(footprint.Footprint.RowPitch) * rowsPerSlice;

            for (UINT slice = 0; slice < textureDepth; ++slice) {
                for (UINT row = 0; row < rowsPerSlice; ++row) {
                    memcpy(
                        uploadDst + slice * dstSlicePitch + row * footprint.Footprint.RowPitch,
                        srcData + slice * subresources[i].slicePitch + row * subresources[i].rowPitch,
                        rowSizeInBytes[i]
                    );
                }
            }

            // Set up copy locations
            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = uploadAlloc.resource;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = footprint;

            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = dstResource;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = i;

            d3dCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }

        // Transition texture back to common/shader resource state
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        d3dCmdList->ResourceBarrier(1, &barrier);
        texture->SetCurrentState(finalState);
    }

    return texture;
}

ISampler* CDX12RenderContext::CreateSampler(const SamplerDesc& desc) {
    auto& heapMgr = CDX12DescriptorHeapManager::Instance();
    SDescriptorHandle handle = heapMgr.AllocateSampler();
    if (!handle.IsValid()) {
        CFFLog::Error("[DX12RenderContext] Failed to allocate sampler descriptor");
        return nullptr;
    }

    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    D3D12_SAMPLER_DESC samplerDesc = {};

    // Filter conversion
    switch (desc.filter) {
        case EFilter::MinMagMipPoint:       samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; break;
        case EFilter::MinMagMipLinear:      samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; break;
        case EFilter::Anisotropic:          samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC; break;
        case EFilter::ComparisonMinMagMipLinear: samplerDesc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR; break;
        case EFilter::ComparisonAnisotropic: samplerDesc.Filter = D3D12_FILTER_COMPARISON_ANISOTROPIC; break;
        default:                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; break;
    }

    // Address mode conversion
    auto convertAddressMode = [](ETextureAddressMode mode) -> D3D12_TEXTURE_ADDRESS_MODE {
        switch (mode) {
            case ETextureAddressMode::Wrap:   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case ETextureAddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case ETextureAddressMode::Clamp:  return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case ETextureAddressMode::Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default:                          return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    };

    samplerDesc.AddressU = convertAddressMode(desc.addressU);
    samplerDesc.AddressV = convertAddressMode(desc.addressV);
    samplerDesc.AddressW = convertAddressMode(desc.addressW);
    samplerDesc.MipLODBias = desc.mipLODBias;
    samplerDesc.MaxAnisotropy = desc.maxAnisotropy;

    // Comparison func conversion
    auto convertCompFunc = [](EComparisonFunc func) -> D3D12_COMPARISON_FUNC {
        switch (func) {
            case EComparisonFunc::Never:        return D3D12_COMPARISON_FUNC_NEVER;
            case EComparisonFunc::Less:         return D3D12_COMPARISON_FUNC_LESS;
            case EComparisonFunc::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
            case EComparisonFunc::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case EComparisonFunc::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
            case EComparisonFunc::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case EComparisonFunc::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case EComparisonFunc::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
            default:                            return D3D12_COMPARISON_FUNC_NEVER;
        }
    };

    samplerDesc.ComparisonFunc = convertCompFunc(desc.comparisonFunc);
    memcpy(samplerDesc.BorderColor, desc.borderColor, sizeof(float) * 4);
    samplerDesc.MinLOD = desc.minLOD;
    samplerDesc.MaxLOD = desc.maxLOD;

    device->CreateSampler(&samplerDesc, handle.cpuHandle);

    return new CDX12Sampler(handle);
}

IShader* CDX12RenderContext::CreateShader(const ShaderDesc& desc) {
    if (!desc.bytecode || desc.bytecodeSize == 0) {
        CFFLog::Error("[DX12RenderContext] CreateShader failed: No bytecode provided");
        return nullptr;
    }
    return new CDX12Shader(desc.type, desc.bytecode, desc.bytecodeSize);
}

IPipelineState* CDX12RenderContext::CreatePipelineState(const PipelineStateDesc& desc) {
    CDX12PSOBuilder builder;

    // Set root signature
    builder.SetRootSignature(m_graphicsRootSignature.Get());

    // Set shaders
    if (desc.vertexShader) {
        CDX12Shader* vs = static_cast<CDX12Shader*>(desc.vertexShader);
        builder.SetVertexShader(vs->GetBytecode());
    }
    if (desc.pixelShader) {
        CDX12Shader* ps = static_cast<CDX12Shader*>(desc.pixelShader);
        builder.SetPixelShader(ps->GetBytecode());
    }
    if (desc.geometryShader) {
        CDX12Shader* gs = static_cast<CDX12Shader*>(desc.geometryShader);
        builder.SetGeometryShader(gs->GetBytecode());
    }
    if (desc.hullShader) {
        CDX12Shader* hs = static_cast<CDX12Shader*>(desc.hullShader);
        builder.SetHullShader(hs->GetBytecode());
    }
    if (desc.domainShader) {
        CDX12Shader* ds = static_cast<CDX12Shader*>(desc.domainShader);
        builder.SetDomainShader(ds->GetBytecode());
    }

    // Set input layout
    if (!desc.inputLayout.empty()) {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        for (const auto& elem : desc.inputLayout) {
            D3D12_INPUT_ELEMENT_DESC d3dElem = {};
            d3dElem.SemanticName = ToD3D12SemanticName(elem.semantic);
            d3dElem.SemanticIndex = elem.semanticIndex;
            d3dElem.Format = ToD3D12VertexFormat(elem.format);
            d3dElem.InputSlot = elem.inputSlot;
            d3dElem.AlignedByteOffset = elem.offset;
            d3dElem.InputSlotClass = elem.instanceData ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            d3dElem.InstanceDataStepRate = elem.instanceData ? 1 : 0;
            elements.push_back(d3dElem);
        }
        builder.SetInputLayout(elements);
    }

    // Set rasterizer state
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = ToD3D12FillMode(desc.rasterizer.fillMode);
    rasterizerDesc.CullMode = ToD3D12CullMode(desc.rasterizer.cullMode);
    rasterizerDesc.FrontCounterClockwise = desc.rasterizer.frontCounterClockwise;
    rasterizerDesc.DepthBias = desc.rasterizer.depthBias;
    rasterizerDesc.DepthBiasClamp = desc.rasterizer.depthBiasClamp;
    rasterizerDesc.SlopeScaledDepthBias = desc.rasterizer.slopeScaledDepthBias;
    rasterizerDesc.DepthClipEnable = desc.rasterizer.depthClipEnable;
    rasterizerDesc.MultisampleEnable = desc.rasterizer.multisampleEnable;
    rasterizerDesc.AntialiasedLineEnable = desc.rasterizer.antialiasedLineEnable;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    builder.SetRasterizerState(rasterizerDesc);

    // Set depth stencil state
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = desc.depthStencil.depthEnable;
    depthStencilDesc.DepthWriteMask = desc.depthStencil.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc = ToD3D12ComparisonFunc(desc.depthStencil.depthFunc);
    depthStencilDesc.StencilEnable = desc.depthStencil.stencilEnable;
    depthStencilDesc.StencilReadMask = desc.depthStencil.stencilReadMask;
    depthStencilDesc.StencilWriteMask = desc.depthStencil.stencilWriteMask;
    builder.SetDepthStencilState(depthStencilDesc);

    // Set blend state
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = desc.blend.blendEnable;
    blendDesc.RenderTarget[0].SrcBlend = ToD3D12BlendFactor(desc.blend.srcBlend);
    blendDesc.RenderTarget[0].DestBlend = ToD3D12BlendFactor(desc.blend.dstBlend);
    blendDesc.RenderTarget[0].BlendOp = ToD3D12BlendOp(desc.blend.blendOp);
    blendDesc.RenderTarget[0].SrcBlendAlpha = ToD3D12BlendFactor(desc.blend.srcBlendAlpha);
    blendDesc.RenderTarget[0].DestBlendAlpha = ToD3D12BlendFactor(desc.blend.dstBlendAlpha);
    blendDesc.RenderTarget[0].BlendOpAlpha = ToD3D12BlendOp(desc.blend.blendOpAlpha);
    blendDesc.RenderTarget[0].RenderTargetWriteMask = desc.blend.renderTargetWriteMask;
    builder.SetBlendState(blendDesc);

    // Set render target formats
    std::vector<DXGI_FORMAT> rtFormats;
    for (auto fmt : desc.renderTargetFormats) {
        rtFormats.push_back(ToDXGIFormat(fmt));
    }
    // Note: Empty rtFormats is valid for depth-only passes (e.g., shadow mapping)
    // Only add default if we have a pixel shader but no explicit RT format
    if (rtFormats.empty() && desc.pixelShader != nullptr) {
        rtFormats.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);  // Default for passes with PS
    }
    builder.SetRenderTargetFormats(rtFormats);

    // Set depth stencil format
    if (desc.depthStencilFormat != ETextureFormat::Unknown) {
        builder.SetDepthStencilFormat(ToDXGIFormat(desc.depthStencilFormat));
    } else {
        builder.SetDepthStencilFormat(DXGI_FORMAT_D24_UNORM_S8_UINT);
    }

    // Set topology type
    builder.SetPrimitiveTopologyType(ToD3D12TopologyType(desc.primitiveTopology));

    // Build PSO
    ComPtr<ID3D12PipelineState> pso;
    pso.Attach(builder.Build(CDX12Context::Instance().GetDevice()));
    if (!pso) {
        CFFLog::Error("[DX12RenderContext] Failed to create graphics PSO");
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        pso->SetName(wname);
    }

    return new CDX12PipelineState(pso.Get(), m_graphicsRootSignature.Get(), false);
}

IPipelineState* CDX12RenderContext::CreateComputePipelineState(const ComputePipelineDesc& desc) {
    if (!desc.computeShader) {
        CFFLog::Error("[DX12RenderContext] CreateComputePipelineState requires compute shader");
        return nullptr;
    }

    CDX12Shader* cs = static_cast<CDX12Shader*>(desc.computeShader);

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_computeRootSignature.Get();
    psoDesc.CS = cs->GetBytecode();

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = DX12_CHECK(CDX12Context::Instance().GetDevice()->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&pso)));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateComputePipelineState failed: %s", HRESULTToString(hr).c_str());
        return nullptr;
    }

    if (desc.debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname, 128);
        pso->SetName(wname);
    }

    return new CDX12PipelineState(pso.Get(), m_computeRootSignature.Get(), true);
}

ITexture* CDX12RenderContext::WrapNativeTexture(void* nativeTexture, void* nativeSRV, uint32_t width, uint32_t height, ETextureFormat format) {
    // In DX12, we don't have a separate SRV object like DX11 - SRVs are descriptor handles
    // This function is primarily for DX11 interop, so just wrap the texture
    if (!nativeTexture) {
        CFFLog::Error("[DX12RenderContext] WrapNativeTexture: null texture");
        return nullptr;
    }

    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.dimension = ETextureDimension::Tex2D;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.sampleCount = 1;
    desc.usage = ETextureUsage::ShaderResource;

    ID3D12Resource* resource = static_cast<ID3D12Resource*>(nativeTexture);
    //resource->AddRef();  // CDX12Texture will own this reference

    return new CDX12Texture(resource, desc, CDX12Context::Instance().GetDevice());
}

ITexture* CDX12RenderContext::WrapExternalTexture(void* nativeTexture, const TextureDesc& desc) {
    if (!nativeTexture) {
        CFFLog::Error("[DX12RenderContext] WrapExternalTexture: null texture");
        return nullptr;
    }

    ID3D12Resource* resource = static_cast<ID3D12Resource*>(nativeTexture);
    //resource->AddRef();  // CDX12Texture will own this reference

    return new CDX12Texture(resource, desc, CDX12Context::Instance().GetDevice());
}

// ============================================
// Backbuffer Access
// ============================================

ITexture* CDX12RenderContext::GetBackbuffer() {
    // Return the backbuffer wrapper for the current frame
    uint32_t frameIndex = CDX12Context::Instance().GetFrameIndex();
    return m_backbufferWrappers[frameIndex].get();
}

ITexture* CDX12RenderContext::GetDepthStencil() {
    return m_depthStencilBuffer.get();
}

// ============================================
// Query
// ============================================

uint32_t CDX12RenderContext::GetWidth() const {
    return CDX12Context::Instance().GetWidth();
}

uint32_t CDX12RenderContext::GetHeight() const {
    return CDX12Context::Instance().GetHeight();
}

bool CDX12RenderContext::SupportsRaytracing() const {
    return CDX12Context::Instance().SupportsRaytracing();
}

bool CDX12RenderContext::SupportsMeshShaders() const {
    return CDX12Context::Instance().SupportsMeshShaders();
}

// ============================================
// Advanced
// ============================================

void* CDX12RenderContext::GetNativeDevice() {
    return CDX12Context::Instance().GetDevice();
}

void* CDX12RenderContext::GetNativeContext() {
    return m_commandList ? m_commandList->GetNativeCommandList() : nullptr;
}

void CDX12RenderContext::ExecuteAndWait() {
    // Close and execute the current command list, then wait for completion
    if (!m_commandList) return;

    auto& ctx = CDX12Context::Instance();

    // Close command list
    m_commandList->Close();

    // Execute
    ID3D12CommandList* cmdLists[] = { m_commandList->GetNativeCommandList() };
    ctx.GetCommandQueue()->ExecuteCommandLists(1, cmdLists);

    // Wait for GPU completion
    ctx.WaitForGPU();

    // Reset command list for next use
    m_commandList->Reset(ctx.GetCurrentCommandAllocator());
}

ID3D12Device* CDX12RenderContext::GetDevice() const {
    return CDX12Context::Instance().GetDevice();
}

ID3D12CommandQueue* CDX12RenderContext::GetCommandQueue() const {
    return CDX12Context::Instance().GetCommandQueue();
}

// ============================================
// Root Signature Creation
// ============================================

bool CDX12RenderContext::CreateRootSignatures() {
    ID3D12Device* device = CDX12Context::Instance().GetDevice();

    // Graphics Root Signature
    // Parameter 0-6: Root CBV b0-b6
    //   b0 (PerFrame), b1 (PerObject), b2 (Material), b3 (ClusteredParams),
    //   b4 (CB_Probes), b5 (CB_LightProbeParams), b6 (CB_VolumetricLightmap)
    // Parameter 7: SRV Descriptor Table t0-t15
    // Parameter 8: UAV Descriptor Table u0-u7
    // Parameter 9: Sampler Descriptor Table s0-s7

    D3D12_ROOT_PARAMETER rootParams[10] = {};

    // CBV parameters (b0-b6)
    for (int i = 0; i < 7; ++i) {
        rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[i].Descriptor.ShaderRegister = i;
        rootParams[i].Descriptor.RegisterSpace = 0;
        rootParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // SRV table (t0-t24)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 25;  // t0-t24 for VolumetricLightmap textures
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // UAV table
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 8;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Sampler table
    D3D12_DESCRIPTOR_RANGE samplerRange = {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = 8;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.RegisterSpace = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[9].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[9].DescriptorTable.pDescriptorRanges = &samplerRange;
    rootParams[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 10;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = DX12_CHECK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    if (FAILED(hr)) {
        if (error) {
            CFFLog::Error("[DX12RenderContext] Root signature serialization failed: %s", (char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = DX12_CHECK(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_graphicsRootSignature)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateRootSignature (graphics) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_graphicsRootSignature, "GraphicsRootSignature");

    // Compute Root Signature (same layout as graphics for consistency)
    // Parameter 0-6: Root CBV b0-b6
    // Parameter 7: SRV Descriptor Table t0-t24
    // Parameter 8: UAV Descriptor Table u0-u7
    // Parameter 9: Sampler Descriptor Table s0-s7
    D3D12_ROOT_PARAMETER computeParams[10] = {};

    // CBV parameters (b0-b6)
    for (int i = 0; i < 7; ++i) {
        computeParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        computeParams[i].Descriptor.ShaderRegister = i;
        computeParams[i].Descriptor.RegisterSpace = 0;
        computeParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // SRV table
    computeParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[7].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[7].DescriptorTable.pDescriptorRanges = &srvRange;
    computeParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // UAV table
    computeParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[8].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[8].DescriptorTable.pDescriptorRanges = &uavRange;
    computeParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Sampler table
    computeParams[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[9].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[9].DescriptorTable.pDescriptorRanges = &samplerRange;
    computeParams[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeRootSigDesc = {};
    computeRootSigDesc.NumParameters = 10;
    computeRootSigDesc.pParameters = computeParams;
    computeRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    hr = DX12_CHECK(D3D12SerializeRootSignature(&computeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    if (FAILED(hr)) {
        if (error) {
            CFFLog::Error("[DX12RenderContext] Compute root signature serialization failed: %s", (char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = DX12_CHECK(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateRootSignature (compute) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_computeRootSignature, "ComputeRootSignature");

    // ============================================
    // Ray Tracing Root Signature
    // ============================================
    // Layout for DXR lightmap baking shader (LightmapBake.hlsl):
    // Parameter 0: Root CBV b0 (CB_BakeParams)
    // Parameter 1: SRV Descriptor Table (t0=TLAS, t1=Skybox, t2-t4=Materials/Lights/Instances)
    // Parameter 2: UAV Descriptor Table (u0=OutputBuffer)
    // Parameter 3: Sampler Descriptor Table (s0)

    D3D12_ROOT_PARAMETER rtParams[4] = {};

    // Parameter 0: Root CBV for CB_BakeParams (b0)
    rtParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rtParams[0].Descriptor.ShaderRegister = 0;
    rtParams[0].Descriptor.RegisterSpace = 0;
    rtParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: SRV table (t0-t6: TLAS, Skybox, Materials, Lights, Instances, Vertices, Indices)
    D3D12_DESCRIPTOR_RANGE rtSrvRange = {};
    rtSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rtSrvRange.NumDescriptors = 7;  // t0-t6
    rtSrvRange.BaseShaderRegister = 0;
    rtSrvRange.RegisterSpace = 0;
    rtSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rtParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rtParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rtParams[1].DescriptorTable.pDescriptorRanges = &rtSrvRange;
    rtParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 2: UAV table (u0: OutputBuffer)
    D3D12_DESCRIPTOR_RANGE rtUavRange = {};
    rtUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rtUavRange.NumDescriptors = 1;  // u0
    rtUavRange.BaseShaderRegister = 0;
    rtUavRange.RegisterSpace = 0;
    rtUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rtParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rtParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rtParams[2].DescriptorTable.pDescriptorRanges = &rtUavRange;
    rtParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 3: Sampler table (s0)
    D3D12_DESCRIPTOR_RANGE rtSamplerRange = {};
    rtSamplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    rtSamplerRange.NumDescriptors = 1;  // s0
    rtSamplerRange.BaseShaderRegister = 0;
    rtSamplerRange.RegisterSpace = 0;
    rtSamplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rtParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rtParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rtParams[3].DescriptorTable.pDescriptorRanges = &rtSamplerRange;
    rtParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rtRootSigDesc = {};
    rtRootSigDesc.NumParameters = 4;
    rtRootSigDesc.pParameters = rtParams;
    rtRootSigDesc.NumStaticSamplers = 0;
    rtRootSigDesc.pStaticSamplers = nullptr;
    // D3D12_ROOT_SIGNATURE_FLAG_NONE for ray tracing (no input assembler)
    rtRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    hr = DX12_CHECK(D3D12SerializeRootSignature(&rtRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    if (FAILED(hr)) {
        if (error) {
            CFFLog::Error("[DX12RenderContext] Ray tracing root signature serialization failed: %s", (char*)error->GetBufferPointer());
        }
        return false;
    }

    hr = DX12_CHECK(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rayTracingRootSignature)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RenderContext] CreateRootSignature (ray tracing) failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    DX12_SET_DEBUG_NAME(m_rayTracingRootSignature, "RayTracingRootSignature");

    CFFLog::Info("[DX12RenderContext] Root signatures created (graphics, compute, ray tracing)");
    return true;
}

// ============================================
// Depth Stencil Management
// ============================================

void CDX12RenderContext::CreateDepthStencilBuffer() {
    uint32_t width = CDX12Context::Instance().GetWidth();
    uint32_t height = CDX12Context::Instance().GetHeight();

    TextureDesc desc = TextureDesc::DepthStencil(width, height);
    desc.debugName = "MainDepthStencil";

    m_depthStencilBuffer.reset(static_cast<CDX12Texture*>(CreateTexture(desc, nullptr)));
}

void CDX12RenderContext::ReleaseDepthStencilBuffer() {
    m_depthStencilBuffer.reset();
}

// ============================================
// Backbuffer Wrapper Management
// ============================================

void CDX12RenderContext::CreateBackbufferWrappers() {
    auto& dx12Ctx = CDX12Context::Instance();
    uint32_t width = dx12Ctx.GetWidth();
    uint32_t height = dx12Ctx.GetHeight();

    TextureDesc desc = TextureDesc::Texture2D(width, height, ETextureFormat::R8G8B8A8_UNORM, ETextureUsage::RenderTarget);

    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i) {
        // Get backbuffer resource for this frame
        ComPtr<ID3D12Resource> backbuffer;
        HRESULT hr = dx12Ctx.GetSwapChain()->GetBuffer(i, IID_PPV_ARGS(&backbuffer));
        if (FAILED(hr)) {
            CFFLog::Error("[DX12RenderContext] Failed to get swapchain buffer %d: %s", i, HRESULTToString(hr).c_str());
            continue;
        }

        // Set debug name
        wchar_t name[32];
        swprintf(name, 32, L"Backbuffer%d", i);
        backbuffer->SetName(name);

        // Create wrapper (CDX12Texture takes ownership by AddRef)
        m_backbufferWrappers[i] = std::make_unique<CDX12Texture>(backbuffer.Get(), desc, dx12Ctx.GetDevice());
    }

    CFFLog::Info("[DX12RenderContext] Created %d backbuffer wrappers (%dx%d)", NUM_FRAMES_IN_FLIGHT, width, height);
}

void CDX12RenderContext::ReleaseBackbufferWrappers() {
    for (uint32_t i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i) {
        m_backbufferWrappers[i].reset();
    }
}

// ============================================
// Ray Tracing (DXR)
// ============================================

AccelerationStructurePrebuildInfo CDX12RenderContext::GetAccelerationStructurePrebuildInfo(const BLASDesc& desc) {
    if (!SupportsRaytracing()) {
        CFFLog::Warning("[DX12RenderContext] Ray tracing not supported");
        return {};
    }

    ID3D12Device5* device5 = CDX12Context::Instance().GetDevice5();
    if (!device5) {
        CFFLog::Error("[DX12RenderContext] ID3D12Device5 not available");
        return {};
    }

    return GetBLASPrebuildInfo(device5, desc);
}

AccelerationStructurePrebuildInfo CDX12RenderContext::GetAccelerationStructurePrebuildInfo(const TLASDesc& desc) {
    if (!SupportsRaytracing()) {
        CFFLog::Warning("[DX12RenderContext] Ray tracing not supported");
        return {};
    }

    ID3D12Device5* device5 = CDX12Context::Instance().GetDevice5();
    if (!device5) {
        CFFLog::Error("[DX12RenderContext] ID3D12Device5 not available");
        return {};
    }

    return GetTLASPrebuildInfo(device5, desc);
}

IAccelerationStructure* CDX12RenderContext::CreateBLAS(
    const BLASDesc& desc,
    IBuffer* scratchBuffer,
    IBuffer* resultBuffer)
{
    if (!SupportsRaytracing()) {
        CFFLog::Warning("[DX12RenderContext] Ray tracing not supported");
        return nullptr;
    }

    if (!scratchBuffer || !resultBuffer) {
        CFFLog::Error("[DX12RenderContext] CreateBLAS: null buffer");
        return nullptr;
    }

    ID3D12Device5* device5 = CDX12Context::Instance().GetDevice5();
    if (!device5) {
        CFFLog::Error("[DX12RenderContext] ID3D12Device5 not available");
        return nullptr;
    }

    return new CDX12AccelerationStructure(device5, desc, scratchBuffer, resultBuffer);
}

IAccelerationStructure* CDX12RenderContext::CreateTLAS(
    const TLASDesc& desc,
    IBuffer* scratchBuffer,
    IBuffer* resultBuffer,
    IBuffer* instanceBuffer)
{
    if (!SupportsRaytracing()) {
        CFFLog::Warning("[DX12RenderContext] Ray tracing not supported");
        return nullptr;
    }

    if (!scratchBuffer || !resultBuffer || !instanceBuffer) {
        CFFLog::Error("[DX12RenderContext] CreateTLAS: null buffer");
        return nullptr;
    }

    ID3D12Device5* device5 = CDX12Context::Instance().GetDevice5();
    if (!device5) {
        CFFLog::Error("[DX12RenderContext] ID3D12Device5 not available");
        return nullptr;
    }

    // Write instance data to the instance buffer
    void* mappedData = instanceBuffer->Map();
    if (mappedData) {
        WriteInstanceData(mappedData, desc);
        instanceBuffer->Unmap();
    } else {
        CFFLog::Error("[DX12RenderContext] CreateTLAS: Failed to map instance buffer");
        return nullptr;
    }

    return new CDX12AccelerationStructure(device5, desc, scratchBuffer, resultBuffer, instanceBuffer);
}

IRayTracingPipelineState* CDX12RenderContext::CreateRayTracingPipelineState(const RayTracingPipelineDesc& desc) {
    if (!SupportsRaytracing()) {
        CFFLog::Warning("[DX12RenderContext] Ray tracing not supported");
        return nullptr;
    }

    if (!desc.shaderLibrary) {
        CFFLog::Error("[DX12RenderContext] CreateRayTracingPipelineState: null shader library");
        return nullptr;
    }

    ID3D12Device5* device5 = CDX12Context::Instance().GetDevice5();
    if (!device5) {
        CFFLog::Error("[DX12RenderContext] ID3D12Device5 not available");
        return nullptr;
    }

    // Get shader bytecode
    CDX12Shader* shader = static_cast<CDX12Shader*>(desc.shaderLibrary);
    const auto& bytecode = shader->GetBytecodeData();

    // Build pipeline using builder
    CDX12RayTracingPipelineBuilder builder;
    builder.SetShaderLibrary(bytecode.data(), bytecode.size());

    // Helper to convert UTF-8 to wide string
    auto toWide = [](const char* str) -> std::wstring {
        if (!str || str[0] == '\0') return L"";
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
        std::wstring wideStr(wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str, -1, wideStr.data(), wideLen);
        return wideStr;
    };

    // Add shader exports
    for (const auto& shaderExport : desc.exports) {
        std::wstring wideName = toWide(shaderExport.name);
        if (wideName.empty()) continue;

        switch (shaderExport.type) {
            case EShaderExportType::RayGeneration:
                builder.AddRayGenShader(wideName.c_str());
                break;
            case EShaderExportType::Miss:
                builder.AddMissShader(wideName.c_str());
                break;
            // ClosestHit, AnyHit, Intersection are added via hit groups
            default:
                break;
        }
    }

    // Add hit groups
    for (const auto& hitGroup : desc.hitGroups) {
        std::wstring name = toWide(hitGroup.name);
        std::wstring closestHit = toWide(hitGroup.closestHitShader);
        std::wstring anyHit = toWide(hitGroup.anyHitShader);
        std::wstring intersection = toWide(hitGroup.intersectionShader);

        builder.AddHitGroup(
            name.c_str(),
            closestHit.empty() ? nullptr : closestHit.c_str(),
            anyHit.empty() ? nullptr : anyHit.c_str(),
            intersection.empty() ? nullptr : intersection.c_str());
    }

    // Set configuration
    builder.SetMaxPayloadSize(desc.maxPayloadSize);
    builder.SetMaxAttributeSize(desc.maxAttributeSize);
    builder.SetMaxRecursionDepth(desc.maxRecursionDepth);

    // Use ray tracing root signature as global root signature
    // IMPORTANT: This must match what PrepareForRayTracing sets on the command list
    builder.SetGlobalRootSignature(m_rayTracingRootSignature.Get());

    return builder.Build(device5);
}

IShaderBindingTable* CDX12RenderContext::CreateShaderBindingTable(const ShaderBindingTableDesc& desc) {
    if (!SupportsRaytracing()) {
        CFFLog::Warning("[DX12RenderContext] Ray tracing not supported");
        return nullptr;
    }

    if (!desc.pipeline) {
        CFFLog::Error("[DX12RenderContext] CreateShaderBindingTable: null pipeline");
        return nullptr;
    }

    if (desc.rayGenRecords.empty()) {
        CFFLog::Error("[DX12RenderContext] CreateShaderBindingTable: no ray generation records");
        return nullptr;
    }

    CDX12ShaderBindingTableBuilder builder;
    builder.SetPipeline(desc.pipeline);

    // Add ray generation records
    for (const auto& record : desc.rayGenRecords) {
        if (record.exportName) {
            builder.AddRayGenRecord(record.exportName, record.localRootArguments, record.localRootArgumentsSize);
        }
    }

    // Add miss shader records
    for (const auto& record : desc.missRecords) {
        if (record.exportName) {
            builder.AddMissRecord(record.exportName, record.localRootArguments, record.localRootArgumentsSize);
        }
    }

    // Add hit group records
    for (const auto& record : desc.hitGroupRecords) {
        if (record.exportName) {
            builder.AddHitGroupRecord(record.exportName, record.localRootArguments, record.localRootArgumentsSize);
        }
    }

    return builder.Build(CDX12Context::Instance().GetDevice());
}

} // namespace DX12
} // namespace RHI
