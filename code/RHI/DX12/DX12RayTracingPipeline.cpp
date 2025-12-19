#include "DX12RayTracingPipeline.h"
#include "DX12Context.h"
#include "../../Core/FFLog.h"
#include <cassert>

namespace RHI {
namespace DX12 {

// ============================================
// CDX12RayTracingPipelineState Implementation
// ============================================

CDX12RayTracingPipelineState::CDX12RayTracingPipelineState(
    ID3D12StateObject* stateObject,
    ID3D12StateObjectProperties* properties)
    : m_stateObject(stateObject)
    , m_properties(properties)
{
    assert(stateObject != nullptr);
    assert(properties != nullptr);
}

CDX12RayTracingPipelineState::~CDX12RayTracingPipelineState() {
    // ComPtr handles release
}

const void* CDX12RayTracingPipelineState::GetShaderIdentifier(const char* exportName) const {
    if (!exportName || !m_properties) {
        return nullptr;
    }

    // Check cache first
    std::string key(exportName);
    auto it = m_shaderIdentifierCache.find(key);
    if (it != m_shaderIdentifierCache.end()) {
        return it->second.data();
    }

    // Convert to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, exportName, -1, nullptr, 0);
    std::wstring wideExport(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, exportName, -1, wideExport.data(), wideLen);

    // Get identifier from state object properties
    const void* identifier = m_properties->GetShaderIdentifier(wideExport.c_str());
    if (!identifier) {
        CFFLog::Warning("[DX12RayTracingPipeline] Shader identifier not found: %s", exportName);
        return nullptr;
    }

    // Cache the identifier (copy the 32 bytes)
    std::vector<uint8_t> identifierCopy(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    memcpy(identifierCopy.data(), identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    m_shaderIdentifierCache[key] = std::move(identifierCopy);

    return m_shaderIdentifierCache[key].data();
}

uint32_t CDX12RayTracingPipelineState::GetShaderIdentifierSize() const {
    return D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;  // Always 32 bytes
}

// ============================================
// CDX12RayTracingPipelineBuilder Implementation
// ============================================

CDX12RayTracingPipelineBuilder::CDX12RayTracingPipelineBuilder() {
}

void CDX12RayTracingPipelineBuilder::SetShaderLibrary(const void* bytecode, size_t bytecodeSize) {
    m_shaderBytecode.resize(bytecodeSize);
    memcpy(m_shaderBytecode.data(), bytecode, bytecodeSize);
}

void CDX12RayTracingPipelineBuilder::AddRayGenShader(const wchar_t* exportName) {
    m_rayGenExports.push_back(exportName);
}

void CDX12RayTracingPipelineBuilder::AddMissShader(const wchar_t* exportName) {
    m_missExports.push_back(exportName);
}

void CDX12RayTracingPipelineBuilder::AddHitGroup(
    const wchar_t* hitGroupName,
    const wchar_t* closestHitExport,
    const wchar_t* anyHitExport,
    const wchar_t* intersectionExport)
{
    HitGroupInfo info;
    info.name = hitGroupName;
    if (closestHitExport) info.closestHit = closestHitExport;
    if (anyHitExport) info.anyHit = anyHitExport;
    if (intersectionExport) info.intersection = intersectionExport;
    m_hitGroups.push_back(std::move(info));
}

void CDX12RayTracingPipelineBuilder::SetGlobalRootSignature(ID3D12RootSignature* rootSig) {
    m_globalRootSig = rootSig;
}

void CDX12RayTracingPipelineBuilder::SetLocalRootSignature(ID3D12RootSignature* rootSig, const wchar_t* exportName) {
    m_localRootSigs.push_back({rootSig, exportName});
}

CDX12RayTracingPipelineState* CDX12RayTracingPipelineBuilder::Build(ID3D12Device5* device) {
    if (!device) {
        CFFLog::Error("[DX12RayTracingPipeline] Build: null device");
        return nullptr;
    }

    if (m_shaderBytecode.empty()) {
        CFFLog::Error("[DX12RayTracingPipeline] Build: no shader library set");
        return nullptr;
    }

    // Calculate number of subobjects needed
    // 1 DXIL library + exports + hit groups + shader config + pipeline config + global root sig
    size_t numSubobjects = 1;  // DXIL library
    numSubobjects += m_hitGroups.size();  // Hit groups
    numSubobjects += 1;  // Shader config
    numSubobjects += 1;  // Shader config association
    numSubobjects += 1;  // Pipeline config
    if (m_globalRootSig) numSubobjects += 1;  // Global root signature
    numSubobjects += m_localRootSigs.size() * 2;  // Local root sigs + associations

    std::vector<D3D12_STATE_SUBOBJECT> subobjects(numSubobjects);
    size_t subobjectIndex = 0;

    // Collect all export names for association
    std::vector<std::wstring> allExports;
    for (const auto& e : m_rayGenExports) allExports.push_back(e);
    for (const auto& e : m_missExports) allExports.push_back(e);
    for (const auto& hg : m_hitGroups) allExports.push_back(hg.name);

    // 1. DXIL Library
    D3D12_DXIL_LIBRARY_DESC libDesc = {};
    libDesc.DXILLibrary.pShaderBytecode = m_shaderBytecode.data();
    libDesc.DXILLibrary.BytecodeLength = m_shaderBytecode.size();
    // Export all symbols (no explicit exports = export all)
    libDesc.NumExports = 0;
    libDesc.pExports = nullptr;

    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[subobjectIndex].pDesc = &libDesc;
    subobjectIndex++;

    // 2. Hit Groups
    std::vector<D3D12_HIT_GROUP_DESC> hitGroupDescs(m_hitGroups.size());
    for (size_t i = 0; i < m_hitGroups.size(); ++i) {
        auto& hg = m_hitGroups[i];
        auto& desc = hitGroupDescs[i];

        desc.HitGroupExport = hg.name.c_str();
        desc.Type = hg.intersection.empty() ?
            D3D12_HIT_GROUP_TYPE_TRIANGLES :
            D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        desc.ClosestHitShaderImport = hg.closestHit.empty() ? nullptr : hg.closestHit.c_str();
        desc.AnyHitShaderImport = hg.anyHit.empty() ? nullptr : hg.anyHit.c_str();
        desc.IntersectionShaderImport = hg.intersection.empty() ? nullptr : hg.intersection.c_str();

        subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        subobjects[subobjectIndex].pDesc = &desc;
        subobjectIndex++;
    }

    // 3. Shader Config (payload and attribute sizes)
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = m_maxPayloadSize;
    shaderConfig.MaxAttributeSizeInBytes = m_maxAttributeSize;

    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[subobjectIndex].pDesc = &shaderConfig;
    size_t shaderConfigIndex = subobjectIndex;
    subobjectIndex++;

    // 4. Shader Config Association (associate with all exports)
    std::vector<LPCWSTR> exportPtrs;
    for (const auto& e : allExports) {
        exportPtrs.push_back(e.c_str());
    }

    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderConfigAssoc = {};
    shaderConfigAssoc.pSubobjectToAssociate = &subobjects[shaderConfigIndex];
    shaderConfigAssoc.NumExports = static_cast<UINT>(exportPtrs.size());
    shaderConfigAssoc.pExports = exportPtrs.empty() ? nullptr : exportPtrs.data();

    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    subobjects[subobjectIndex].pDesc = &shaderConfigAssoc;
    subobjectIndex++;

    // 5. Pipeline Config (max recursion depth)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = m_maxRecursionDepth;

    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[subobjectIndex].pDesc = &pipelineConfig;
    subobjectIndex++;

    // 6. Global Root Signature (optional)
    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSigDesc = {};
    if (m_globalRootSig) {
        globalRootSigDesc.pGlobalRootSignature = m_globalRootSig;

        subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        subobjects[subobjectIndex].pDesc = &globalRootSigDesc;
        subobjectIndex++;
    }

    // 7. Local Root Signatures (optional)
    std::vector<D3D12_LOCAL_ROOT_SIGNATURE> localRootSigDescs(m_localRootSigs.size());
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> localRootSigAssocs(m_localRootSigs.size());
    std::vector<LPCWSTR> localExportPtrs(m_localRootSigs.size());

    for (size_t i = 0; i < m_localRootSigs.size(); ++i) {
        localRootSigDescs[i].pLocalRootSignature = m_localRootSigs[i].first;

        subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
        subobjects[subobjectIndex].pDesc = &localRootSigDescs[i];
        size_t localRootSigIndex = subobjectIndex;
        subobjectIndex++;

        // Association
        localExportPtrs[i] = m_localRootSigs[i].second.c_str();
        localRootSigAssocs[i].pSubobjectToAssociate = &subobjects[localRootSigIndex];
        localRootSigAssocs[i].NumExports = 1;
        localRootSigAssocs[i].pExports = &localExportPtrs[i];

        subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
        subobjects[subobjectIndex].pDesc = &localRootSigAssocs[i];
        subobjectIndex++;
    }

    // Create state object
    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjectIndex);
    stateObjectDesc.pSubobjects = subobjects.data();

    ComPtr<ID3D12StateObject> stateObject;
    HRESULT hr = device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&stateObject));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RayTracingPipeline] CreateStateObject failed: 0x%08X", hr);
        return nullptr;
    }

    // Get state object properties for shader identifier lookup
    ComPtr<ID3D12StateObjectProperties> properties;
    hr = stateObject->QueryInterface(IID_PPV_ARGS(&properties));
    if (FAILED(hr)) {
        CFFLog::Error("[DX12RayTracingPipeline] QueryInterface for properties failed: 0x%08X", hr);
        return nullptr;
    }

    CFFLog::Info("[DX12RayTracingPipeline] Created ray tracing pipeline state");
    return new CDX12RayTracingPipelineState(stateObject.Get(), properties.Get());
}

} // namespace DX12
} // namespace RHI
