#include "DX12UploadManager.h"
#include "DX12Common.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// CUploadPage Implementation
// ============================================

CUploadPage::CUploadPage(ID3D12Device* device, uint64_t size)
    : m_size(size)
    , m_offset(0)
{
    // Create upload buffer
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = DX12_CHECK(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_resource)
    ));

    if (FAILED(hr)) {
        CFFLog::Error("[UploadPage] Failed to create upload buffer: %s", HRESULTToString(hr).c_str());
        return;
    }

    // Map the buffer
    D3D12_RANGE readRange = { 0, 0 };
    hr = DX12_CHECK(m_resource->Map(0, &readRange, &m_cpuAddress));
    if (FAILED(hr)) {
        CFFLog::Error("[UploadPage] Failed to map upload buffer: %s", HRESULTToString(hr).c_str());
        m_resource.Reset();
        return;
    }

    m_gpuAddress = m_resource->GetGPUVirtualAddress();

    DX12_SET_DEBUG_NAME(m_resource, "UploadPage");
}

CUploadPage::~CUploadPage() {
    if (m_resource && m_cpuAddress) {
        m_resource->Unmap(0, nullptr);
    }
}

CUploadPage::CUploadPage(CUploadPage&& other) noexcept
    : m_resource(std::move(other.m_resource))
    , m_cpuAddress(other.m_cpuAddress)
    , m_gpuAddress(other.m_gpuAddress)
    , m_size(other.m_size)
    , m_offset(other.m_offset)
{
    other.m_cpuAddress = nullptr;
    other.m_gpuAddress = 0;
    other.m_size = 0;
    other.m_offset = 0;
}

CUploadPage& CUploadPage::operator=(CUploadPage&& other) noexcept {
    if (this != &other) {
        if (m_resource && m_cpuAddress) {
            m_resource->Unmap(0, nullptr);
        }

        m_resource = std::move(other.m_resource);
        m_cpuAddress = other.m_cpuAddress;
        m_gpuAddress = other.m_gpuAddress;
        m_size = other.m_size;
        m_offset = other.m_offset;

        other.m_cpuAddress = nullptr;
        other.m_gpuAddress = 0;
        other.m_size = 0;
        other.m_offset = 0;
    }
    return *this;
}

UploadAllocation CUploadPage::Allocate(uint64_t size, uint64_t alignment) {
    UploadAllocation result;

    // Align offset
    uint64_t alignedOffset = AlignUp(m_offset, alignment);

    if (alignedOffset + size > m_size) {
        return result;  // Not enough space
    }

    result.resource = m_resource.Get();
    result.cpuAddress = static_cast<uint8_t*>(m_cpuAddress) + alignedOffset;
    result.gpuAddress = m_gpuAddress + alignedOffset;
    result.offset = alignedOffset;
    result.size = size;

    m_offset = alignedOffset + size;

    return result;
}

void CUploadPage::Reset() {
    m_offset = 0;
}

bool CUploadPage::HasSpace(uint64_t size, uint64_t alignment) const {
    uint64_t alignedOffset = AlignUp(m_offset, alignment);
    return alignedOffset + size <= m_size;
}

// ============================================
// CDX12UploadManager Implementation
// ============================================

CDX12UploadManager& CDX12UploadManager::Instance() {
    static CDX12UploadManager instance;
    return instance;
}

bool CDX12UploadManager::Initialize(ID3D12Device* device) {
    if (m_initialized) {
        return true;
    }

    m_device = device;
    m_initialized = true;

    CFFLog::Info("[UploadManager] Initialized");
    return true;
}

void CDX12UploadManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    m_currentPages.clear();
    m_pendingPages.clear();
    m_availablePages.clear();
    m_device = nullptr;
    m_initialized = false;

    CFFLog::Info("[UploadManager] Shutdown");
}

UploadAllocation CDX12UploadManager::Allocate(uint64_t size, uint64_t alignment) {
    UploadAllocation result;

    if (!m_initialized) {
        CFFLog::Error("[UploadManager] Not initialized");
        return result;
    }

    // Try to allocate from current pages
    for (auto& page : m_currentPages) {
        if (page.HasSpace(size, alignment)) {
            result = page.Allocate(size, alignment);
            if (result.IsValid()) {
                return result;
            }
        }
    }

    // No space in current pages, get a new one
    uint64_t pageSize = std::max(DEFAULT_PAGE_SIZE, size);

    // Try to get from available pool first
    CUploadPage* newPage = nullptr;
    for (size_t i = 0; i < m_availablePages.size(); ++i) {
        if (m_availablePages[i].GetSize() >= pageSize) {
            m_currentPages.push_back(std::move(m_availablePages[i]));
            m_availablePages.erase(m_availablePages.begin() + i);
            newPage = &m_currentPages.back();
            break;
        }
    }

    // Create new page if none available
    if (!newPage) {
        m_currentPages.push_back(CreatePage(pageSize));
        newPage = &m_currentPages.back();
    }

    return newPage->Allocate(size, alignment);
}

void CDX12UploadManager::FinishUploads(uint64_t fenceValue) {
    // Move all current pages to pending
    for (auto& page : m_currentPages) {
        PendingUploadPage pending;
        pending.page = std::move(page);
        pending.fenceValue = fenceValue;
        m_pendingPages.push_back(std::move(pending));
    }
    m_currentPages.clear();
}

void CDX12UploadManager::ProcessCompletedUploads(uint64_t completedFenceValue) {
    // Move completed pending pages back to available pool
    while (!m_pendingPages.empty()) {
        auto& pending = m_pendingPages.front();
        if (pending.fenceValue <= completedFenceValue) {
            pending.page.Reset();
            m_availablePages.push_back(std::move(pending.page));
            m_pendingPages.pop_front();
        } else {
            break;  // Remaining pages are still in use
        }
    }
}

CUploadPage CDX12UploadManager::CreatePage(uint64_t size) {
    return CUploadPage(m_device, size);
}

} // namespace DX12
} // namespace RHI
