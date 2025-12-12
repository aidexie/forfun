#pragma once

#include "DX12Common.h"
#include <vector>
#include <deque>

// ============================================
// DX12 Upload Manager
// ============================================
// Manages upload buffers for texture and buffer data uploads
// Uses a ring buffer approach with fence-based deferred release

namespace RHI {
namespace DX12 {

// ============================================
// Upload Allocation
// ============================================
struct UploadAllocation {
    ID3D12Resource* resource = nullptr;
    void* cpuAddress = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    uint64_t offset = 0;
    uint64_t size = 0;

    bool IsValid() const { return resource != nullptr; }
};

// ============================================
// Upload Page
// ============================================
// A single upload buffer that can be subdivided
class CUploadPage {
public:
    CUploadPage() = default;
    CUploadPage(ID3D12Device* device, uint64_t size);
    ~CUploadPage();

    // Non-copyable
    CUploadPage(const CUploadPage&) = delete;
    CUploadPage& operator=(const CUploadPage&) = delete;

    // Move semantics
    CUploadPage(CUploadPage&& other) noexcept;
    CUploadPage& operator=(CUploadPage&& other) noexcept;

    // Try to allocate from this page
    UploadAllocation Allocate(uint64_t size, uint64_t alignment);

    // Reset for reuse
    void Reset();

    uint64_t GetSize() const { return m_size; }
    uint64_t GetOffset() const { return m_offset; }
    bool HasSpace(uint64_t size, uint64_t alignment) const;

private:
    ComPtr<ID3D12Resource> m_resource;
    void* m_cpuAddress = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuAddress = 0;
    uint64_t m_size = 0;
    uint64_t m_offset = 0;
};

// ============================================
// Pending Upload Page
// ============================================
struct PendingUploadPage {
    CUploadPage page;
    uint64_t fenceValue;
};

// ============================================
// Upload Manager
// ============================================
class CDX12UploadManager {
public:
    static CDX12UploadManager& Instance();

    // Initialize with device reference
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // Allocate upload memory
    UploadAllocation Allocate(uint64_t size, uint64_t alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // Signal that uploads in current batch are complete (call after ExecuteCommandLists)
    void FinishUploads(uint64_t fenceValue);

    // Check completed fence values and recycle pages
    void ProcessCompletedUploads(uint64_t completedFenceValue);

    // Get default page size
    static constexpr uint64_t DEFAULT_PAGE_SIZE = 2 * 1024 * 1024;  // 2 MB

private:
    CDX12UploadManager() = default;
    ~CDX12UploadManager() = default;

    // Non-copyable
    CDX12UploadManager(const CDX12UploadManager&) = delete;
    CDX12UploadManager& operator=(const CDX12UploadManager&) = delete;

    // Create a new upload page
    CUploadPage CreatePage(uint64_t size);

private:
    ID3D12Device* m_device = nullptr;

    // Current page being allocated from
    std::vector<CUploadPage> m_currentPages;

    // Pages waiting for GPU to finish using them
    std::deque<PendingUploadPage> m_pendingPages;

    // Pool of available (recycled) pages
    std::vector<CUploadPage> m_availablePages;

    bool m_initialized = false;
};

} // namespace DX12
} // namespace RHI
