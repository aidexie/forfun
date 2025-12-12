#pragma once

#include "DX12Common.h"
#include <Windows.h>

// ============================================
// DX12 Context (Singleton)
// ============================================
// Manages the core DX12 objects: Device, CommandQueue, SwapChain, Fence
// Similar to CDX11Context but with DX12-specific synchronization

namespace RHI {
namespace DX12 {

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
    void CreateBackbufferRTVs();
    void EnableDebugLayer();
    void CheckFeatureSupport();

    // Cleanup helpers
    void ReleaseBackbuffers();

private:
    // Core objects
    ComPtr<IDXGIFactory4> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;

    // Per-frame resources
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[NUM_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12Resource> m_backbuffers[NUM_FRAMES_IN_FLIGHT];

    // RTV heap for backbuffers (small, dedicated heap)
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    uint32_t m_rtvDescriptorSize = 0;

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
};

} // namespace DX12
} // namespace RHI
