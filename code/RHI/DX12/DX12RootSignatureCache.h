#pragma once

#include "DX12Common.h"
#include "DX12DescriptorSet.h"
#include <unordered_map>
#include <array>

// ============================================
// DX12 Root Signature Cache
// ============================================
// Caches root signatures based on descriptor set layout combinations.
// Root signatures are shared between pipelines using the same layouts.

namespace RHI {
namespace DX12 {

// ============================================
// Root Signature Result
// ============================================
// Contains the root signature and per-set binding info for command list use.

struct SRootSignatureResult {
    ComPtr<ID3D12RootSignature> rootSignature;
    SSetRootParamInfo setBindings[4];

    bool IsValid() const { return rootSignature.Get() != nullptr; }
};

// ============================================
// CDX12RootSignatureCache
// ============================================

class CDX12RootSignatureCache {
public:
    // Singleton access
    static CDX12RootSignatureCache& Instance();

    // Initialize with device
    void Initialize(ID3D12Device* device);

    // Shutdown and release all cached root signatures
    void Shutdown();

    // Get or create root signature for given layout combination
    // layouts: array of 4 layout pointers (nullptr for unused sets)
    SRootSignatureResult GetOrCreate(IDescriptorSetLayout* const layouts[4]);

private:
    CDX12RootSignatureCache() = default;
    ~CDX12RootSignatureCache() = default;

    // Non-copyable
    CDX12RootSignatureCache(const CDX12RootSignatureCache&) = delete;
    CDX12RootSignatureCache& operator=(const CDX12RootSignatureCache&) = delete;

    // Cache key: array of 4 layout pointers
    struct CacheKey {
        std::array<IDescriptorSetLayout*, 4> layouts;

        bool operator==(const CacheKey& other) const {
            return layouts == other.layouts;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& key) const {
            size_t hash = 0;
            for (auto* layout : key.layouts) {
                hash ^= std::hash<void*>{}(layout) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    // Cache entry
    struct CacheEntry {
        ComPtr<ID3D12RootSignature> rootSignature;
        SSetRootParamInfo setBindings[4];
    };

    // Build root signature from layouts
    CacheEntry BuildRootSignature(IDescriptorSetLayout* const layouts[4]);

    // Calculate root signature DWORD cost for validation
    uint32_t CalculateDWordCost(IDescriptorSetLayout* const layouts[4]) const;

private:
    ID3D12Device* m_device = nullptr;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> m_cache;
};

} // namespace DX12
} // namespace RHI
