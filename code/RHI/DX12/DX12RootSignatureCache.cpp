#include "DX12RootSignatureCache.h"
#include "../../Core/FFLog.h"
#include <vector>

namespace RHI {
namespace DX12 {

// ============================================
// Singleton Instance
// ============================================

CDX12RootSignatureCache& CDX12RootSignatureCache::Instance() {
    static CDX12RootSignatureCache instance;
    return instance;
}

void CDX12RootSignatureCache::Initialize(ID3D12Device* device) {
    m_device = device;
}

void CDX12RootSignatureCache::Shutdown() {
    m_cache.clear();
    m_device = nullptr;
}

// ============================================
// Get or Create Root Signature
// ============================================

SRootSignatureResult CDX12RootSignatureCache::GetOrCreate(IDescriptorSetLayout* const layouts[4]) {
    CacheKey key;
    for (int i = 0; i < 4; ++i) {
        key.layouts[i] = layouts[i];
    }

    // Check cache
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        SRootSignatureResult result;
        result.rootSignature = it->second.rootSignature.Get();
        for (int i = 0; i < 4; ++i) {
            result.setBindings[i] = it->second.setBindings[i];
        }
        return result;
    }

    // Validate root signature size
    uint32_t dwordCost = CalculateDWordCost(layouts);
    if (dwordCost > 64) {
        CFFLog::Error("Root signature exceeds 64 DWORD limit: %u DWORDs", dwordCost);
    } else if (dwordCost > 56) {
        CFFLog::Warning("Root signature approaching 64 DWORD limit: %u DWORDs", dwordCost);
    }

    // Build new root signature
    CacheEntry entry = BuildRootSignature(layouts);
    if (!entry.rootSignature) {
        return SRootSignatureResult{};
    }

    // Cache and return
    m_cache[key] = std::move(entry);

    SRootSignatureResult result;
    result.rootSignature = m_cache[key].rootSignature.Get();
    for (int i = 0; i < 4; ++i) {
        result.setBindings[i] = m_cache[key].setBindings[i];
    }
    return result;
}

// ============================================
// Build Root Signature
// ============================================

CDX12RootSignatureCache::CacheEntry CDX12RootSignatureCache::BuildRootSignature(
    IDescriptorSetLayout* const layouts[4]) {

    CacheEntry entry;
    std::vector<D3D12_ROOT_PARAMETER1> rootParams;
    std::vector<D3D12_DESCRIPTOR_RANGE1> allRanges;

    // Reserve space for ranges (estimate: max 16 bindings per set * 4 sets)
    allRanges.reserve(64);

    // Process each set
    for (uint32_t setIndex = 0; setIndex < 4; ++setIndex) {
        IDescriptorSetLayout* layout = layouts[setIndex];
        if (!layout) {
            entry.setBindings[setIndex].isUsed = false;
            continue;
        }

        auto* dx12Layout = static_cast<CDX12DescriptorSetLayout*>(layout);
        entry.setBindings[setIndex].isUsed = true;
        entry.setBindings[setIndex].srvCount = dx12Layout->GetSRVCount();
        entry.setBindings[setIndex].uavCount = dx12Layout->GetUAVCount();
        entry.setBindings[setIndex].samplerCount = dx12Layout->GetSamplerCount();

        // Add Push Constants (root constants) - highest priority
        if (dx12Layout->HasPushConstants()) {
            uint32_t dwordCount = (dx12Layout->GetPushConstantSize() + 3) / 4;
            entry.setBindings[setIndex].pushConstantDwordCount = dwordCount;
            entry.setBindings[setIndex].pushConstantRootParam = static_cast<uint32_t>(rootParams.size());

            D3D12_ROOT_PARAMETER1 param = {};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            param.Constants.ShaderRegister = dx12Layout->GetPushConstantSlot();
            param.Constants.RegisterSpace = setIndex;
            param.Constants.Num32BitValues = dwordCount;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            rootParams.push_back(param);
        }

        // Add Volatile CBV (root CBV) - second priority
        if (dx12Layout->HasVolatileCBV()) {
            entry.setBindings[setIndex].volatileCBVSize = dx12Layout->GetVolatileCBVSize();
            entry.setBindings[setIndex].volatileCBVRootParam = static_cast<uint32_t>(rootParams.size());

            D3D12_ROOT_PARAMETER1 param = {};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor.ShaderRegister = dx12Layout->GetVolatileCBVSlot();
            param.Descriptor.RegisterSpace = setIndex;
            param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            rootParams.push_back(param);
        }

        // Add Static Constant Buffer (root CBV)
        if (dx12Layout->HasConstantBuffer()) {
            entry.setBindings[setIndex].constantBufferRootParam = static_cast<uint32_t>(rootParams.size());

            D3D12_ROOT_PARAMETER1 param = {};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor.ShaderRegister = dx12Layout->GetConstantBufferSlot();
            param.Descriptor.RegisterSpace = setIndex;
            param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            rootParams.push_back(param);
        }

        // Add SRV table
        if (dx12Layout->GetSRVCount() > 0) {
            size_t rangeStart = allRanges.size();

            // Allocate space for SRV ranges
            allRanges.resize(rangeStart + dx12Layout->GetBindingCount());
            uint32_t rangeCount = dx12Layout->PopulateSRVRanges(&allRanges[rangeStart], setIndex);
            allRanges.resize(rangeStart + rangeCount);

            if (rangeCount > 0) {
                entry.setBindings[setIndex].srvTableRootParam = static_cast<uint32_t>(rootParams.size());

                D3D12_ROOT_PARAMETER1 param = {};
                param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                param.DescriptorTable.NumDescriptorRanges = rangeCount;
                param.DescriptorTable.pDescriptorRanges = &allRanges[rangeStart];
                param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                rootParams.push_back(param);
            }
        }

        // Add UAV table
        if (dx12Layout->GetUAVCount() > 0) {
            size_t rangeStart = allRanges.size();

            allRanges.resize(rangeStart + dx12Layout->GetBindingCount());
            uint32_t rangeCount = dx12Layout->PopulateUAVRanges(&allRanges[rangeStart], setIndex);
            allRanges.resize(rangeStart + rangeCount);

            if (rangeCount > 0) {
                entry.setBindings[setIndex].uavTableRootParam = static_cast<uint32_t>(rootParams.size());

                D3D12_ROOT_PARAMETER1 param = {};
                param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                param.DescriptorTable.NumDescriptorRanges = rangeCount;
                param.DescriptorTable.pDescriptorRanges = &allRanges[rangeStart];
                param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                rootParams.push_back(param);
            }
        }

        // Add Sampler table
        if (dx12Layout->GetSamplerCount() > 0) {
            size_t rangeStart = allRanges.size();

            allRanges.resize(rangeStart + dx12Layout->GetBindingCount());
            uint32_t rangeCount = dx12Layout->PopulateSamplerRanges(&allRanges[rangeStart], setIndex);
            allRanges.resize(rangeStart + rangeCount);

            if (rangeCount > 0) {
                entry.setBindings[setIndex].samplerTableRootParam = static_cast<uint32_t>(rootParams.size());

                D3D12_ROOT_PARAMETER1 param = {};
                param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                param.DescriptorTable.NumDescriptorRanges = rangeCount;
                param.DescriptorTable.pDescriptorRanges = &allRanges[rangeStart];
                param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                rootParams.push_back(param);
            }
        }
    }

    // Create root signature description
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParams.size());
    rootSigDesc.Desc_1_1.pParameters = rootParams.empty() ? nullptr : rootParams.data();
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize root signature
    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            CFFLog::Error("Failed to serialize root signature: %s",
                static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return entry;
    }

    // Create root signature
    hr = m_device->CreateRootSignature(
        0,
        signatureBlob->GetBufferPointer(),
        signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&entry.rootSignature));

    if (FAILED(hr)) {
        CFFLog::Error("Failed to create root signature: 0x%08X", hr);
        return entry;
    }

    return entry;
}

// ============================================
// Calculate DWORD Cost
// ============================================

uint32_t CDX12RootSignatureCache::CalculateDWordCost(IDescriptorSetLayout* const layouts[4]) const {
    uint32_t cost = 0;

    for (uint32_t i = 0; i < 4; ++i) {
        IDescriptorSetLayout* layout = layouts[i];
        if (!layout) continue;

        auto* dx12Layout = static_cast<CDX12DescriptorSetLayout*>(layout);

        // Push constants: 1 DWORD per 4 bytes
        if (dx12Layout->HasPushConstants()) {
            cost += (dx12Layout->GetPushConstantSize() + 3) / 4;
        }

        // Root CBV: 2 DWORDs (64-bit GPU VA)
        if (dx12Layout->HasVolatileCBV()) {
            cost += 2;
        }

        // Static CBV: 2 DWORDs
        if (dx12Layout->HasConstantBuffer()) {
            cost += 2;
        }

        // Descriptor table: 1 DWORD each
        if (dx12Layout->GetSRVCount() > 0) cost += 1;
        if (dx12Layout->GetUAVCount() > 0) cost += 1;
        if (dx12Layout->GetSamplerCount() > 0) cost += 1;
    }

    return cost;
}

} // namespace DX12
} // namespace RHI
