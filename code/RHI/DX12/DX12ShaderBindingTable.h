#pragma once

#include "DX12Common.h"
#include "../RHIRayTracing.h"

// ============================================
// DX12 Shader Binding Table Implementation
// ============================================
// Implements IShaderBindingTable for DXR.
// Manages shader records for ray generation, miss, and hit group shaders.

namespace RHI {
namespace DX12 {

class CDX12ShaderBindingTable : public IShaderBindingTable {
public:
    CDX12ShaderBindingTable(
        ID3D12Resource* sbtBuffer,
        uint64_t rayGenOffset,
        uint64_t rayGenSize,
        uint64_t missOffset,
        uint64_t missSize,
        uint64_t missStride,
        uint64_t hitGroupOffset,
        uint64_t hitGroupSize,
        uint64_t hitGroupStride);

    ~CDX12ShaderBindingTable() override;

    // IShaderBindingTable interface
    uint64_t GetRayGenShaderRecordAddress() const override;
    uint64_t GetRayGenShaderRecordSize() const override { return m_rayGenSize; }

    uint64_t GetMissShaderTableAddress() const override;
    uint64_t GetMissShaderTableSize() const override { return m_missSize; }
    uint64_t GetMissShaderTableStride() const override { return m_missStride; }

    uint64_t GetHitGroupTableAddress() const override;
    uint64_t GetHitGroupTableSize() const override { return m_hitGroupSize; }
    uint64_t GetHitGroupTableStride() const override { return m_hitGroupStride; }

    void* GetNativeHandle() override { return m_sbtBuffer.Get(); }

    // DX12-specific accessors
    ID3D12Resource* GetBuffer() const { return m_sbtBuffer.Get(); }

private:
    ComPtr<ID3D12Resource> m_sbtBuffer;

    // Ray generation
    uint64_t m_rayGenOffset = 0;
    uint64_t m_rayGenSize = 0;

    // Miss shaders
    uint64_t m_missOffset = 0;
    uint64_t m_missSize = 0;
    uint64_t m_missStride = 0;

    // Hit groups
    uint64_t m_hitGroupOffset = 0;
    uint64_t m_hitGroupSize = 0;
    uint64_t m_hitGroupStride = 0;
};

// ============================================
// SBT Builder
// ============================================
// Helper class to construct shader binding tables

class CDX12ShaderBindingTableBuilder {
public:
    CDX12ShaderBindingTableBuilder();

    // Set the pipeline state (required for shader identifier lookup)
    void SetPipeline(IRayTracingPipelineState* pipeline);

    // Add ray generation shader record
    // exportName: Name of the ray generation shader export
    // localRootArgs: Optional local root arguments (can be nullptr)
    // localRootArgsSize: Size of local root arguments in bytes
    void AddRayGenRecord(const char* exportName, const void* localRootArgs = nullptr, uint32_t localRootArgsSize = 0);

    // Add miss shader record
    void AddMissRecord(const char* exportName, const void* localRootArgs = nullptr, uint32_t localRootArgsSize = 0);

    // Add hit group record
    void AddHitGroupRecord(const char* hitGroupName, const void* localRootArgs = nullptr, uint32_t localRootArgsSize = 0);

    // Build the SBT
    CDX12ShaderBindingTable* Build(ID3D12Device* device);

private:
    struct ShaderRecord {
        std::string exportName;
        std::vector<uint8_t> localRootArgs;
    };

    // Align size to D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT
    static uint64_t AlignShaderRecord(uint64_t size);

    // Calculate record size (identifier + local root args, aligned)
    uint64_t CalculateRecordSize(const ShaderRecord& record) const;

    IRayTracingPipelineState* m_pipeline = nullptr;

    std::vector<ShaderRecord> m_rayGenRecords;
    std::vector<ShaderRecord> m_missRecords;
    std::vector<ShaderRecord> m_hitGroupRecords;
};

} // namespace DX12
} // namespace RHI
