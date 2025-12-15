#pragma once

#include "DX12Common.h"
#include <vector>

// ============================================
// DX12 Dynamic Constant Buffer Ring
// ============================================
// Provides per-draw constant data for DX12 by allocating from a ring buffer.
// Each allocation returns a unique GPU virtual address that won't be overwritten
// until the next frame (after GPU fence).

namespace RHI {
namespace DX12 {

// Alignment requirement for constant buffers
constexpr size_t CB_ALIGNMENT = 256;

// Single allocation from the ring buffer
struct SDynamicAllocation {
    void* cpuAddress = nullptr;           // CPU-mapped pointer for writing data
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;  // GPU address for binding
    size_t size = 0;                      // Size of allocation

    bool IsValid() const { return cpuAddress != nullptr && gpuAddress != 0; }
};

// Per-frame ring buffer for dynamic constant data
class CDX12DynamicBufferRing {
public:
    CDX12DynamicBufferRing() = default;
    ~CDX12DynamicBufferRing();

    // Initialize with total size (shared across all frames)
    // frameCount: number of frames in flight (typically 3 for triple buffering)
    // sizePerFrame: bytes available per frame
    bool Initialize(ID3D12Device* device, size_t sizePerFrame, uint32_t frameCount);

    // Call at start of frame - advances to next frame's region
    void BeginFrame(uint32_t frameIndex);

    // Allocate constant buffer data with specified size
    // Returns GPU virtual address for binding, and CPU pointer for writing
    SDynamicAllocation Allocate(size_t size, size_t alignment = CB_ALIGNMENT);

    // Get current frame's base offset (for debugging)
    size_t GetCurrentOffset() const { return m_currentOffset; }
    size_t GetFrameSize() const { return m_sizePerFrame; }

private:
    ComPtr<ID3D12Resource> m_buffer;
    void* m_cpuMappedAddress = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBaseAddress = 0;

    size_t m_sizePerFrame = 0;
    uint32_t m_frameCount = 0;
    uint32_t m_currentFrameIndex = 0;

    size_t m_currentOffset = 0;      // Current allocation offset within the frame's region
    size_t m_frameStartOffset = 0;   // Start of current frame's region
};

} // namespace DX12
} // namespace RHI
