#include "DX12DynamicBuffer.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

CDX12DynamicBufferRing::~CDX12DynamicBufferRing() {
    if (m_buffer && m_cpuMappedAddress) {
        m_buffer->Unmap(0, nullptr);
        m_cpuMappedAddress = nullptr;
    }
}

bool CDX12DynamicBufferRing::Initialize(ID3D12Device* device, size_t sizePerFrame, uint32_t frameCount) {
    m_sizePerFrame = sizePerFrame;
    m_frameCount = frameCount;

    size_t totalSize = sizePerFrame * frameCount;

    // Create upload buffer (persistently mapped)
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_buffer)
    );

    if (FAILED(hr)) {
        CFFLog::Error("[DX12DynamicBuffer] Failed to create buffer: 0x%08X", hr);
        return false;
    }

    // Persistently map the buffer
    D3D12_RANGE readRange = { 0, 0 };  // We don't read from this buffer
    hr = m_buffer->Map(0, &readRange, &m_cpuMappedAddress);
    if (FAILED(hr)) {
        CFFLog::Error("[DX12DynamicBuffer] Failed to map buffer: 0x%08X", hr);
        return false;
    }

    m_gpuBaseAddress = m_buffer->GetGPUVirtualAddress();

    CFFLog::Info("[DX12DynamicBuffer] Created ring buffer: %zu bytes/frame, %u frames, total %zu bytes",
                 sizePerFrame, frameCount, totalSize);

    return true;
}

void CDX12DynamicBufferRing::BeginFrame(uint32_t frameIndex) {
    m_currentFrameIndex = frameIndex % m_frameCount;
    m_frameStartOffset = m_currentFrameIndex * m_sizePerFrame;
    m_currentOffset = m_frameStartOffset;
}

SDynamicAllocation CDX12DynamicBufferRing::Allocate(size_t size, size_t alignment) {
    SDynamicAllocation alloc;

    // Align the current offset
    size_t alignedOffset = (m_currentOffset + alignment - 1) & ~(alignment - 1);

    // Check if we have enough space in this frame's region
    size_t frameEndOffset = m_frameStartOffset + m_sizePerFrame;
    if (alignedOffset + size > frameEndOffset) {
        CFFLog::Error("[DX12DynamicBuffer] Out of memory! Frame %u, requested %zu bytes, available %zu bytes",
                      m_currentFrameIndex, size, frameEndOffset - alignedOffset);
        return alloc;  // Return invalid allocation
    }

    alloc.cpuAddress = static_cast<uint8_t*>(m_cpuMappedAddress) + alignedOffset;
    alloc.gpuAddress = m_gpuBaseAddress + alignedOffset;
    alloc.size = size;

    m_currentOffset = alignedOffset + size;

    return alloc;
}

} // namespace DX12
} // namespace RHI
