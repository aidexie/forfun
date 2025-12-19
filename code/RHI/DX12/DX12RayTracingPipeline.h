#pragma once

#include "DX12Common.h"
#include "../RHIRayTracing.h"
#include <unordered_map>
#include <string>

// ============================================
// DX12 Ray Tracing Pipeline State Implementation
// ============================================
// Implements IRayTracingPipelineState for DXR.
// Wraps ID3D12StateObject and provides shader identifier lookup.

namespace RHI {
namespace DX12 {

class CDX12RayTracingPipelineState : public IRayTracingPipelineState {
public:
    CDX12RayTracingPipelineState(
        ID3D12StateObject* stateObject,
        ID3D12StateObjectProperties* properties);

    ~CDX12RayTracingPipelineState() override;

    // IRayTracingPipelineState interface
    const void* GetShaderIdentifier(const char* exportName) const override;
    uint32_t GetShaderIdentifierSize() const override;
    void* GetNativeHandle() override { return m_stateObject.Get(); }

    // DX12-specific accessors
    ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
    ID3D12StateObjectProperties* GetProperties() const { return m_properties.Get(); }

private:
    ComPtr<ID3D12StateObject> m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_properties;

    // Cache shader identifiers for fast lookup
    mutable std::unordered_map<std::string, std::vector<uint8_t>> m_shaderIdentifierCache;
};

// ============================================
// Pipeline State Builder
// ============================================
// Helper class to construct ray tracing state objects

class CDX12RayTracingPipelineBuilder {
public:
    CDX12RayTracingPipelineBuilder();

    // Set the DXIL library containing all shaders
    void SetShaderLibrary(const void* bytecode, size_t bytecodeSize);

    // Add ray generation shader export
    void AddRayGenShader(const wchar_t* exportName);

    // Add miss shader export
    void AddMissShader(const wchar_t* exportName);

    // Add hit group (closest hit, any hit, intersection)
    void AddHitGroup(
        const wchar_t* hitGroupName,
        const wchar_t* closestHitExport = nullptr,
        const wchar_t* anyHitExport = nullptr,
        const wchar_t* intersectionExport = nullptr);

    // Set pipeline configuration
    void SetMaxPayloadSize(uint32_t size) { m_maxPayloadSize = size; }
    void SetMaxAttributeSize(uint32_t size) { m_maxAttributeSize = size; }
    void SetMaxRecursionDepth(uint32_t depth) { m_maxRecursionDepth = depth; }

    // Set global root signature (for global resources like TLAS)
    void SetGlobalRootSignature(ID3D12RootSignature* rootSig);

    // Set local root signature (for per-shader resources)
    void SetLocalRootSignature(ID3D12RootSignature* rootSig, const wchar_t* exportName);

    // Build the state object
    CDX12RayTracingPipelineState* Build(ID3D12Device5* device);

private:
    // DXIL library
    std::vector<uint8_t> m_shaderBytecode;

    // Export names
    std::vector<std::wstring> m_rayGenExports;
    std::vector<std::wstring> m_missExports;

    // Hit groups
    struct HitGroupInfo {
        std::wstring name;
        std::wstring closestHit;
        std::wstring anyHit;
        std::wstring intersection;
    };
    std::vector<HitGroupInfo> m_hitGroups;

    // Root signatures
    ID3D12RootSignature* m_globalRootSig = nullptr;
    std::vector<std::pair<ID3D12RootSignature*, std::wstring>> m_localRootSigs;

    // Configuration
    uint32_t m_maxPayloadSize = 32;
    uint32_t m_maxAttributeSize = 8;  // Default: 2 floats for barycentrics
    uint32_t m_maxRecursionDepth = 1;
};

} // namespace DX12
} // namespace RHI
