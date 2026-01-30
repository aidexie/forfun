#pragma once

#include "DX12Common.h"
#include <Windows.h>
#include <vector>
#include <mutex>

// ============================================
// DX12 Context (Singleton)
// ============================================
// Manages the core DX12 objects: Device, CommandQueue, SwapChain, Fence
// Similar to CDX11Context but with DX12-specific synchronization

namespace RHI {
namespace DX12 {

// ============================================
// Deferred Deletion Queue
// ============================================
// Holds resources until GPU finishes using them.
// Similar to UE5's FD3D12DeferredDeletionQueue.

class CDX12DeferredDeletionQueue {
public:
    // Queue a resource for deferred deletion
    // The resource will be released when the GPU passes the specified fence value
    void DeferredRelease(ID3D12Resource* resource, uint64_t fenceValue);

    // Queue a descriptor heap for deferred deletion
    void DeferredRelease(ID3D12DescriptorHeap* heap, uint64_t fenceValue);

    // Process completed deletions - call at frame start
    void ProcessCompleted(uint64_t completedFenceValue);

    // Force release all resources - call at shutdown
    void ReleaseAll();

    // Get pending count for debugging
    size_t GetPendingCount() const { return m_pendingResources.size() + m_pendingHeaps.size(); }

private:
    struct SPendingResource {
        ComPtr<ID3D12Resource> resource;
        uint64_t fenceValue;
    };

    struct SPendingHeap {
        ComPtr<ID3D12DescriptorHeap> heap;
        uint64_t fenceValue;
    };

    std::vector<SPendingResource> m_pendingResources;
    std::vector<SPendingHeap> m_pendingHeaps;
    mutable std::mutex m_mutex;  // Thread safety for multi-threaded resource destruction
};

class CDX12Context {
public:
    // Singleton access
    static CDX12Context& Instance();

    // Lifecycle
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height);

    // Frame synchronization
    void WaitForGPU();
    void MoveToNextFrame();
    uint64_t SignalFence();
    void WaitForFenceValue(uint64_t fenceValue);

    // Accessors
    ID3D12Device* GetDevice() const { return m_device.Get(); }
    ID3D12Device5* GetDevice5() const { return m_device5.Get(); }  // For ray tracing (cached)
    ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }
    IDXGISwapChain3* GetSwapChain() const { return m_swapChain.Get(); }

    ID3D12CommandAllocator* GetCurrentCommandAllocator() const {
        return m_commandAllocators[m_frameIndex].Get();
    }

    ID3D12Resource* GetCurrentBackbuffer() const {
        return m_backbuffers[m_frameIndex].Get();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentBackbufferRTV() const;

    uint32_t GetFrameIndex() const { return m_frameIndex; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint64_t GetCurrentFenceValue() const { return m_fenceValue; }

    // Check if device supports required features
    bool SupportsRaytracing() const { return m_supportsRaytracing; }
    bool SupportsMeshShaders() const { return m_supportsMeshShaders; }

    // Debug: Flush and log any D3D12 debug messages
    void FlushDebugMessages();

    // ImGui support - dedicated SRV heap for ImGui fonts/textures
    ID3D12DescriptorHeap* GetImGuiSrvHeap() const { return m_imguiSrvHeap.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiSrvCpuHandle() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiSrvGpuHandle() const;

    // Viewport texture support - allocate a descriptor for viewport rendering
    // Returns GPU handle for use with ImGui::Image, or {0} if allocation fails
    D3D12_GPU_DESCRIPTOR_HANDLE AllocateImGuiTextureDescriptor(ID3D12Resource* texture, DXGI_FORMAT format);

    // Update an existing ImGui texture descriptor at a specific slot
    // slot: The slot index (1-63, 0 is reserved for font texture)
    // Returns the GPU handle for the slot
    D3D12_GPU_DESCRIPTOR_HANDLE UpdateImGuiTextureDescriptor(uint32_t slot, ID3D12Resource* texture, DXGI_FORMAT format);

    // Get the descriptor size for SRV heap
    uint32_t GetSrvDescriptorSize() const { return m_srvDescriptorSize; }

    // Deferred deletion - queue resources for deletion after GPU finishes
    void DeferredRelease(ID3D12Resource* resource);
    void DeferredRelease(ID3D12DescriptorHeap* heap);

    // Get pending deletion count for debugging
    size_t GetPendingDeletionCount() const { return m_deletionQueue.GetPendingCount(); }

private:
    CDX12Context() = default;
    ~CDX12Context();

    // Non-copyable
    CDX12Context(const CDX12Context&) = delete;
    CDX12Context& operator=(const CDX12Context&) = delete;

    // Initialization helpers
    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateSwapChain(HWND hwnd);
    bool CreateCommandAllocators();
    bool CreateFence();
    bool CreateRTVHeap();
    bool CreateImGuiSrvHeap();
    void CreateBackbufferRTVs();
    void EnableDebugLayer();
    void CheckFeatureSupport();

    // Cleanup helpers
    void ReleaseBackbuffers();

private:
    // Core objects
    ComPtr<IDXGIFactory4> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;  // Stored for D3D12MA initialization
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12Device5> m_device5;  // Cached for ray tracing (avoids repeated QueryInterface)
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;

    // Per-frame resources
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[NUM_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12Resource> m_backbuffers[NUM_FRAMES_IN_FLIGHT];

    // RTV heap for backbuffers (small, dedicated heap)
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    uint32_t m_rtvDescriptorSize = 0;

    // SRV heap for ImGui (shader-visible, for font texture + viewport textures)
    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
    uint32_t m_srvDescriptorSize = 0;
    uint32_t m_imguiSrvNextSlot = 1;  // Slot 0 is reserved for ImGui font texture
    static constexpr uint32_t IMGUI_SRV_HEAP_SIZE = 64;  // Support multiple viewport textures

    // Synchronization
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint64_t m_frameFenceValues[NUM_FRAMES_IN_FLIGHT] = {};

    // State
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_frameIndex = 0;
    bool m_initialized = false;

    // Feature support flags
    bool m_supportsRaytracing = false;
    bool m_supportsMeshShaders = false;

    // Deferred deletion queue
    CDX12DeferredDeletionQueue m_deletionQueue;
};

} // namespace DX12
} // namespace RHI
