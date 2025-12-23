#include "DX12ShaderBindingTable.h"
#include "DX12Context.h"
#include "../../Core/FFLog.h"
#include <cassert>

namespace RHI {
namespace DX12 {

// ============================================
// CDX12ShaderBindingTable Implementation
// ============================================

CDX12ShaderBindingTable::CDX12ShaderBindingTable(
    ID3D12Resource* sbtBuffer,
    uint64_t rayGenOffset,
    uint64_t rayGenSize,
    uint64_t missOffset,
    uint64_t missSize,
    uint64_t missStride,
    uint64_t hitGroupOffset,
    uint64_t hitGroupSize,
    uint64_t hitGroupStride)
    : m_sbtBuffer(sbtBuffer)
    , m_rayGenOffset(rayGenOffset)
    , m_rayGenSize(rayGenSize)
    , m_missOffset(missOffset)
    , m_missSize(missSize)
    , m_missStride(missStride)
    , m_hitGroupOffset(hitGroupOffset)
    , m_hitGroupSize(hitGroupSize)
    , m_hitGroupStride(hitGroupStride)
{
    assert(sbtBuffer != nullptr);
}

CDX12ShaderBindingTable::~CDX12ShaderBindingTable() {
    // ComPtr handles release
}

uint64_t CDX12ShaderBindingTable::GetRayGenShaderRecordAddress() const {
    return m_sbtBuffer->GetGPUVirtualAddress() + m_rayGenOffset;
}

uint64_t CDX12ShaderBindingTable::GetMissShaderTableAddress() const {
    return m_sbtBuffer->GetGPUVirtualAddress() + m_missOffset;
}

uint64_t CDX12ShaderBindingTable::GetHitGroupTableAddress() const {
    return m_sbtBuffer->GetGPUVirtualAddress() + m_hitGroupOffset;
}

// ============================================
// CDX12ShaderBindingTableBuilder Implementation
// ============================================

CDX12ShaderBindingTableBuilder::CDX12ShaderBindingTableBuilder() {
}

void CDX12ShaderBindingTableBuilder::SetPipeline(IRayTracingPipelineState* pipeline) {
    m_pipeline = pipeline;
}

void CDX12ShaderBindingTableBuilder::AddRayGenRecord(const char* exportName, const void* localRootArgs, uint32_t localRootArgsSize) {
    ShaderRecord record;
    record.exportName = exportName;
    if (localRootArgs && localRootArgsSize > 0) {
        record.localRootArgs.resize(localRootArgsSize);
        memcpy(record.localRootArgs.data(), localRootArgs, localRootArgsSize);
    }
    m_rayGenRecords.push_back(std::move(record));
}

void CDX12ShaderBindingTableBuilder::AddMissRecord(const char* exportName, const void* localRootArgs, uint32_t localRootArgsSize) {
    ShaderRecord record;
    record.exportName = exportName;
    if (localRootArgs && localRootArgsSize > 0) {
        record.localRootArgs.resize(localRootArgsSize);
        memcpy(record.localRootArgs.data(), localRootArgs, localRootArgsSize);
    }
    m_missRecords.push_back(std::move(record));
}

void CDX12ShaderBindingTableBuilder::AddHitGroupRecord(const char* hitGroupName, const void* localRootArgs, uint32_t localRootArgsSize) {
    ShaderRecord record;
    record.exportName = hitGroupName;
    if (localRootArgs && localRootArgsSize > 0) {
        record.localRootArgs.resize(localRootArgsSize);
        memcpy(record.localRootArgs.data(), localRootArgs, localRootArgsSize);
    }
    m_hitGroupRecords.push_back(std::move(record));
}

uint64_t CDX12ShaderBindingTableBuilder::AlignShaderRecord(uint64_t size) {
    // Shader records must be aligned to D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT (32 bytes)
    const uint64_t alignment = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
    return (size + alignment - 1) & ~(alignment - 1);
}

uint64_t CDX12ShaderBindingTableBuilder::CalculateRecordSize(const ShaderRecord& record) const {
    // Record = shader identifier (32 bytes) + local root arguments
    uint64_t size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    size += record.localRootArgs.size();
    return AlignShaderRecord(size);
}

CDX12ShaderBindingTable* CDX12ShaderBindingTableBuilder::Build(ID3D12Device* device) {
    if (!device) {
        CFFLog::Error("[DX12ShaderBindingTable] Build: null device");
        return nullptr;
    }

    if (!m_pipeline) {
        CFFLog::Error("[DX12ShaderBindingTable] Build: no pipeline set");
        return nullptr;
    }

    if (m_rayGenRecords.empty()) {
        CFFLog::Error("[DX12ShaderBindingTable] Build: no ray generation records");
        return nullptr;
    }

    // Calculate sizes and offsets
    uint64_t rayGenRecordSize = 0;
    for (const auto& record : m_rayGenRecords) {
        rayGenRecordSize = std::max(rayGenRecordSize, CalculateRecordSize(record));
    }
    uint64_t rayGenSize = rayGenRecordSize * m_rayGenRecords.size();

    uint64_t missRecordSize = 0;
    for (const auto& record : m_missRecords) {
        missRecordSize = std::max(missRecordSize, CalculateRecordSize(record));
    }
    uint64_t missSize = missRecordSize * m_missRecords.size();

    uint64_t hitGroupRecordSize = 0;
    for (const auto& record : m_hitGroupRecords) {
        hitGroupRecordSize = std::max(hitGroupRecordSize, CalculateRecordSize(record));
    }
    uint64_t hitGroupSize = hitGroupRecordSize * m_hitGroupRecords.size();

    // Calculate offsets (each section aligned to 64 bytes for table start alignment)
    const uint64_t tableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    uint64_t rayGenOffset = 0;
    uint64_t missOffset = (rayGenOffset + rayGenSize + tableAlignment - 1) & ~(tableAlignment - 1);
    uint64_t hitGroupOffset = (missOffset + missSize + tableAlignment - 1) & ~(tableAlignment - 1);
    uint64_t totalSize = hitGroupOffset + hitGroupSize;

    // Create upload buffer for SBT
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> sbtBuffer;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&sbtBuffer));

    if (FAILED(hr)) {
        CFFLog::Error("[DX12ShaderBindingTable] CreateCommittedResource failed: 0x%08X", hr);
        return nullptr;
    }

    sbtBuffer->SetName(L"ShaderBindingTable");

    // Map and fill the buffer
    void* mappedData = nullptr;
    hr = sbtBuffer->Map(0, nullptr, &mappedData);
    if (FAILED(hr)) {
        CFFLog::Error("[DX12ShaderBindingTable] Map failed: 0x%08X", hr);
        return nullptr;
    }

    uint8_t* sbtData = static_cast<uint8_t*>(mappedData);
    memset(sbtData, 0, totalSize);

    // Write ray generation records
    uint8_t* rayGenData = sbtData + rayGenOffset;
    for (size_t i = 0; i < m_rayGenRecords.size(); ++i) {
        const auto& record = m_rayGenRecords[i];
        uint8_t* recordPtr = rayGenData + i * rayGenRecordSize;

        // Copy shader identifier
        const void* identifier = m_pipeline->GetShaderIdentifier(record.exportName.c_str());
        if (identifier) {
            memcpy(recordPtr, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        } else {
            CFFLog::Error("[DX12ShaderBindingTable] Missing RayGen shader identifier: %s", record.exportName.c_str());
        }

        // Copy local root arguments
        if (!record.localRootArgs.empty()) {
            memcpy(recordPtr + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
                   record.localRootArgs.data(),
                   record.localRootArgs.size());
        }
    }

    // Write miss shader records
    uint8_t* missData = sbtData + missOffset;
    for (size_t i = 0; i < m_missRecords.size(); ++i) {
        const auto& record = m_missRecords[i];
        uint8_t* recordPtr = missData + i * missRecordSize;

        const void* identifier = m_pipeline->GetShaderIdentifier(record.exportName.c_str());
        if (identifier) {
            memcpy(recordPtr, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }

        if (!record.localRootArgs.empty()) {
            memcpy(recordPtr + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
                   record.localRootArgs.data(),
                   record.localRootArgs.size());
        }
    }

    // Write hit group records
    uint8_t* hitGroupData = sbtData + hitGroupOffset;
    for (size_t i = 0; i < m_hitGroupRecords.size(); ++i) {
        const auto& record = m_hitGroupRecords[i];
        uint8_t* recordPtr = hitGroupData + i * hitGroupRecordSize;

        const void* identifier = m_pipeline->GetShaderIdentifier(record.exportName.c_str());
        if (identifier) {
            memcpy(recordPtr, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }

        if (!record.localRootArgs.empty()) {
            memcpy(recordPtr + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
                   record.localRootArgs.data(),
                   record.localRootArgs.size());
        }
    }

    sbtBuffer->Unmap(0, nullptr);

    CFFLog::Info("[DX12ShaderBindingTable] Created SBT: rayGen=%llu, miss=%llu, hitGroup=%llu, total=%llu bytes",
                 rayGenSize, missSize, hitGroupSize, totalSize);

    return new CDX12ShaderBindingTable(
        sbtBuffer.Get(),
        rayGenOffset, rayGenRecordSize,
        missOffset, missSize, missRecordSize,
        hitGroupOffset, hitGroupSize, hitGroupRecordSize);
}

} // namespace DX12
} // namespace RHI
