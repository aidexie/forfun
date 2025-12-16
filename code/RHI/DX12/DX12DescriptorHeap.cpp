#include "DX12DescriptorHeap.h"
#include "DX12Common.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// CDX12DescriptorHeap Implementation
// ============================================

CDX12DescriptorHeap::~CDX12DescriptorHeap() {
    Shutdown();
}

bool CDX12DescriptorHeap::Initialize(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t numDescriptors,
    bool shaderVisible,
    const char* debugName
) {
    if (!device || numDescriptors == 0) {
        CFFLog::Error("[DX12DescriptorHeap] Invalid parameters");
        return false;
    }

    // RTV and DSV heaps cannot be shader visible
    if (shaderVisible && (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) {
        CFFLog::Warning("[DX12DescriptorHeap] RTV/DSV heaps cannot be shader visible, ignoring flag");
        shaderVisible = false;
    }

    m_type = type;
    m_capacity = numDescriptors;
    m_shaderVisible = shaderVisible;

    // Create the descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = type;
    heapDesc.NumDescriptors = numDescriptors;
    heapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDesc.NodeMask = 0;

    HRESULT hr = DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12DescriptorHeap] CreateDescriptorHeap failed: %s", HRESULTToString(hr).c_str());
        return false;
    }

    // Set debug name
    if (debugName) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, debugName, -1, wname, 128);
        m_heap->SetName(wname);
    }

    // Get descriptor size for this heap type
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);

    // Get start handles
    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) {
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
    }

    // Initialize free list with all indices (in reverse order for LIFO behavior)
    m_freeList.reserve(numDescriptors);
    for (uint32_t i = numDescriptors; i > 0; --i) {
        m_freeList.push_back(i - 1);
    }

    m_allocatedCount = 0;

    const char* typeNames[] = { "CBV_SRV_UAV", "SAMPLER", "RTV", "DSV" };
    CFFLog::Info("[DX12DescriptorHeap] Created %s heap: %u descriptors, %s",
        typeNames[type], numDescriptors, shaderVisible ? "shader-visible" : "CPU-only");

    return true;
}

void CDX12DescriptorHeap::Shutdown() {
    if (m_allocatedCount > 0) {
        CFFLog::Warning("[DX12DescriptorHeap] Shutting down with %u descriptors still allocated", m_allocatedCount);
    }

    m_heap.Reset();
    m_freeList.clear();
    m_capacity = 0;
    m_allocatedCount = 0;
    m_cpuStart = {};
    m_gpuStart = {};
}

SDescriptorHandle CDX12DescriptorHeap::Allocate() {
    SDescriptorHandle handle;

    if (m_freeList.empty()) {
        CFFLog::Error("[DX12DescriptorHeap] Heap is full! Cannot allocate descriptor");
        return handle;  // Returns invalid handle
    }

    // Pop from free list (LIFO)
    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    m_allocatedCount++;

    // Calculate handles
    handle.index = index;
    handle.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
    if (m_shaderVisible) {
        handle.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
    }

    return handle;
}

SDescriptorHandle CDX12DescriptorHeap::AllocateRange(uint32_t count) {
    SDescriptorHandle handle;

    if (count == 0) {
        return handle;  // Invalid
    }

    if (count == 1) {
        return Allocate();  // Use simple path
    }

    // For range allocation, we need contiguous descriptors
    // This is a simple linear search - could be optimized with a more sophisticated allocator
    // For now, we only support range allocation from the end of the heap

    if (GetFreeCount() < count) {
        CFFLog::Error("[DX12DescriptorHeap] Not enough free descriptors for range allocation (%u requested, %u free)",
            count, GetFreeCount());
        return handle;
    }

    // Find contiguous free block
    // Sort free list and look for contiguous range
    std::vector<uint32_t> sortedFree = m_freeList;
    std::sort(sortedFree.begin(), sortedFree.end());

    uint32_t startIndex = UINT32_MAX;
    uint32_t consecutive = 0;

    for (size_t i = 0; i < sortedFree.size(); ++i) {
        if (i == 0 || sortedFree[i] != sortedFree[i - 1] + 1) {
            // Start new sequence
            startIndex = sortedFree[i];
            consecutive = 1;
        } else {
            consecutive++;
        }

        if (consecutive >= count) {
            // Found contiguous block
            startIndex = sortedFree[i] - count + 1;
            break;
        }
    }

    if (consecutive < count) {
        CFFLog::Error("[DX12DescriptorHeap] No contiguous block of %u descriptors available", count);
        return handle;
    }

    // Remove allocated indices from free list
    for (uint32_t i = startIndex; i < startIndex + count; ++i) {
        auto it = std::find(m_freeList.begin(), m_freeList.end(), i);
        if (it != m_freeList.end()) {
            m_freeList.erase(it);
        }
    }
    m_allocatedCount += count;

    // Return handle to first descriptor
    handle.index = startIndex;
    handle.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(startIndex) * m_descriptorSize;
    if (m_shaderVisible) {
        handle.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(startIndex) * m_descriptorSize;
    }

    return handle;
}

void CDX12DescriptorHeap::Free(const SDescriptorHandle& handle) {
    if (!handle.IsValid()) {
        return;
    }

    if (handle.index >= m_capacity) {
        CFFLog::Error("[DX12DescriptorHeap] Invalid descriptor index %u (capacity %u)", handle.index, m_capacity);
        return;
    }

    // Check for double-free (debug only)
#ifdef _DEBUG
    if (std::find(m_freeList.begin(), m_freeList.end(), handle.index) != m_freeList.end()) {
        CFFLog::Error("[DX12DescriptorHeap] Double free detected for index %u", handle.index);
        return;
    }
#endif

    m_freeList.push_back(handle.index);
    m_allocatedCount--;
}

void CDX12DescriptorHeap::FreeRange(const SDescriptorHandle& handle, uint32_t count) {
    if (!handle.IsValid() || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        SDescriptorHandle h;
        h.index = handle.index + i;
        h.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(h.index) * m_descriptorSize;
        if (m_shaderVisible) {
            h.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(h.index) * m_descriptorSize;
        }
        Free(h);
    }
}

SDescriptorHandle CDX12DescriptorHeap::GetHandle(uint32_t index) const {
    SDescriptorHandle handle;

    if (index >= m_capacity) {
        return handle;  // Invalid
    }

    handle.index = index;
    handle.cpuHandle.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
    if (m_shaderVisible) {
        handle.gpuHandle.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
    }

    return handle;
}

// ============================================
// CDX12DescriptorStagingRing Implementation
// ============================================

bool CDX12DescriptorStagingRing::Initialize(ID3D12Device* device, uint32_t descriptorsPerFrame, uint32_t frameCount) {
    if (!device || descriptorsPerFrame == 0 || frameCount == 0) {
        CFFLog::Error("[DX12DescriptorStagingRing] Invalid parameters");
        return false;
    }

    // Create our own shader-visible heap
    uint32_t totalDescriptors = descriptorsPerFrame * frameCount;
    if (!m_heap.Initialize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            totalDescriptors,
            true,  // shader visible
            "SRV_Staging_Heap")) {
        CFFLog::Error("[DX12DescriptorStagingRing] Failed to create staging heap");
        return false;
    }

    m_descriptorsPerFrame = descriptorsPerFrame;
    m_frameCount = frameCount;
    m_currentFrame = 0;
    m_currentOffset = 0;

    CFFLog::Info("[DX12DescriptorStagingRing] Initialized: perFrame=%u, frames=%u, total=%u",
        descriptorsPerFrame, frameCount, totalDescriptors);

    return true;
}

void CDX12DescriptorStagingRing::Shutdown() {
    m_heap.Shutdown();
    m_descriptorsPerFrame = 0;
    m_frameCount = 0;
    m_currentFrame = 0;
    m_currentOffset = 0;
}

void CDX12DescriptorStagingRing::BeginFrame(uint32_t frameIndex) {
    m_currentFrame = frameIndex % m_frameCount;
    m_currentOffset = 0;
}

SDescriptorHandle CDX12DescriptorStagingRing::AllocateContiguous(uint32_t count) {
    SDescriptorHandle handle;

    if (count == 0) {
        return handle;  // Invalid
    }

    if (m_currentOffset + count > m_descriptorsPerFrame) {
        CFFLog::Error("[DX12DescriptorStagingRing] Out of staging space! Requested %u, remaining %u",
            count, m_descriptorsPerFrame - m_currentOffset);
        return handle;  // Invalid
    }

    // Calculate the actual index in the heap
    uint32_t frameStart = m_currentFrame * m_descriptorsPerFrame;
    uint32_t allocIndex = frameStart + m_currentOffset;

    // Get handle from our owned heap
    handle = m_heap.GetHandle(allocIndex);

    // Advance offset
    m_currentOffset += count;

    return handle;
}

uint32_t CDX12DescriptorStagingRing::GetRemainingCapacity() const {
    return m_descriptorsPerFrame - m_currentOffset;
}

// ============================================
// CDX12DescriptorHeapManager Implementation
// ============================================

CDX12DescriptorHeapManager& CDX12DescriptorHeapManager::Instance() {
    static CDX12DescriptorHeapManager instance;
    return instance;
}

bool CDX12DescriptorHeapManager::Initialize(ID3D12Device* device) {
    if (m_initialized) {
        CFFLog::Warning("[DX12DescriptorHeapManager] Already initialized");
        return true;
    }

    CFFLog::Info("[DX12DescriptorHeapManager] Initializing descriptor heaps...");

    // Create CBV/SRV/UAV heap (CPU only) - for persistent SRV/UAV/CBV storage (copy source)
    if (!m_cbvSrvUavHeap.Initialize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            CBV_SRV_UAV_HEAP_SIZE_CPU,
            false,  // NOT shader visible - CPU only
            "CBV_SRV_UAV_Heap_CPU")) {
        CFFLog::Error("[DX12DescriptorHeapManager] Failed to create CBV_SRV_UAV CPU heap");
        return false;
    }

    // Create Sampler heap (shader visible - samplers bind directly)
    if (!m_samplerHeap.Initialize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            SAMPLER_HEAP_SIZE,
            true,  // shader visible
            "Sampler_Heap")) {
        CFFLog::Error("[DX12DescriptorHeapManager] Failed to create Sampler heap");
        return false;
    }

    // Create RTV heap (CPU only)
    if (!m_rtvHeap.Initialize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            RTV_HEAP_SIZE,
            false,  // not shader visible
            "RTV_Heap")) {
        CFFLog::Error("[DX12DescriptorHeapManager] Failed to create RTV heap");
        return false;
    }

    // Create DSV heap (CPU only)
    if (!m_dsvHeap.Initialize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
            DSV_HEAP_SIZE,
            false,  // not shader visible
            "DSV_Heap")) {
        CFFLog::Error("[DX12DescriptorHeapManager] Failed to create DSV heap");
        return false;
    }

    // Initialize SRV staging ring (owns its own GPU shader-visible heap)
    if (!m_srvStagingRing.Initialize(device,
            SRV_STAGING_PER_FRAME,
            NUM_FRAMES_IN_FLIGHT)) {
        CFFLog::Error("[DX12DescriptorHeapManager] Failed to initialize SRV staging ring");
        return false;
    }

    m_initialized = true;
    CFFLog::Info("[DX12DescriptorHeapManager] All descriptor heaps created successfully");

    return true;
}

void CDX12DescriptorHeapManager::Shutdown() {
    if (!m_initialized) return;

    CFFLog::Info("[DX12DescriptorHeapManager] Shutting down...");

    // Log allocation stats before shutdown
    CFFLog::Info("[DX12DescriptorHeapManager] CBV_SRV_UAV: %u/%u allocated",
        m_cbvSrvUavHeap.GetAllocatedCount(), m_cbvSrvUavHeap.GetCapacity());
    CFFLog::Info("[DX12DescriptorHeapManager] Sampler: %u/%u allocated",
        m_samplerHeap.GetAllocatedCount(), m_samplerHeap.GetCapacity());
    CFFLog::Info("[DX12DescriptorHeapManager] RTV: %u/%u allocated",
        m_rtvHeap.GetAllocatedCount(), m_rtvHeap.GetCapacity());
    CFFLog::Info("[DX12DescriptorHeapManager] DSV: %u/%u allocated",
        m_dsvHeap.GetAllocatedCount(), m_dsvHeap.GetCapacity());

    m_cbvSrvUavHeap.Shutdown();
    m_samplerHeap.Shutdown();
    m_rtvHeap.Shutdown();
    m_dsvHeap.Shutdown();
    m_srvStagingRing.Shutdown();

    m_initialized = false;
    CFFLog::Info("[DX12DescriptorHeapManager] Shutdown complete");
}

void CDX12DescriptorHeapManager::BeginFrame(uint32_t frameIndex) {
    m_srvStagingRing.BeginFrame(frameIndex);
}

} // namespace DX12
} // namespace RHI
