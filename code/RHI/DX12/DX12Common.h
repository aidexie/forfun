#pragma once

// ============================================
// DX12 Common Definitions
// ============================================
// Common includes, types, and utilities for DX12 backend

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

// For D3D12 debug layer
#ifdef _DEBUG
#include <dxgidebug.h>
#endif

namespace RHI {
namespace DX12 {

// ============================================
// ComPtr alias
// ============================================
using Microsoft::WRL::ComPtr;

// ============================================
// Constants
// ============================================
constexpr uint32_t NUM_FRAMES_IN_FLIGHT = 3;

// Descriptor heap sizes
constexpr uint32_t CBV_SRV_UAV_HEAP_SIZE = 4096;
constexpr uint32_t SAMPLER_HEAP_SIZE = 256;
constexpr uint32_t RTV_HEAP_SIZE = 128;
constexpr uint32_t DSV_HEAP_SIZE = 32;

// ============================================
// Error Handling
// ============================================

// Convert HRESULT to human-readable string
inline std::string HRESULTToString(HRESULT hr) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "HRESULT 0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

// Check HRESULT and log error
inline bool CheckHR(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        // Log will be handled by caller using CFFLog
        return false;
    }
    return true;
}

// ============================================
// Debug Macros
// ============================================

#ifdef _DEBUG
#define DX12_SET_DEBUG_NAME(obj, name) \
    do { \
        if (obj) { \
            obj->SetName(L##name); \
        } \
    } while(0)

#define DX12_SET_DEBUG_NAME_INDEXED(obj, name, index) \
    do { \
        if (obj) { \
            wchar_t buffer[128]; \
            swprintf_s(buffer, L"%hs[%u]", name, index); \
            obj->SetName(buffer); \
        } \
    } while(0)

// ============================================
// DX12_CHECK Macro - Wrap D3D12 calls with error checking
// ============================================
// In Debug mode: clears message queue, executes, checks result, prints errors with file/line
// In Release mode: just executes the expression

// Forward declarations for debug helpers (implemented in DX12Debug.cpp)
void DX12Debug_ClearMessages();
void DX12Debug_PrintMessages(const char* expr, const char* file, int line);

// DX12_CHECK for HRESULT-returning functions
#define DX12_CHECK(expr) \
    [&]() -> HRESULT { \
        DX12Debug_ClearMessages(); \
        HRESULT hr_ = (expr); \
        if (FAILED(hr_)) { \
            DX12Debug_PrintMessages(#expr, __FILE__, __LINE__); \
        } \
        return hr_; \
    }()

// DX12_CHECK_VOID for void functions (e.g., command list operations)
#define DX12_CHECK_VOID(expr) \
    do { \
        DX12Debug_ClearMessages(); \
        (expr); \
        DX12Debug_PrintMessages(#expr, __FILE__, __LINE__); \
    } while(0)

#else
// Release mode - no overhead
#define DX12_SET_DEBUG_NAME(obj, name) ((void)0)
#define DX12_SET_DEBUG_NAME_INDEXED(obj, name, index) ((void)0)
#define DX12_CHECK(expr) (expr)
#define DX12_CHECK_VOID(expr) (expr)
#endif

// ============================================
// Resource State Helpers
// ============================================

// Common resource state combinations
constexpr D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_SHADER_RESOURCE_ALL =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Check if a state transition is needed
inline bool NeedsTransition(D3D12_RESOURCE_STATES current, D3D12_RESOURCE_STATES target) {
    // Same state - no transition needed
    if (current == target) return false;

    // If target is a subset of current read states, no transition needed
    // This handles cases where resource is in combined read state
    if ((current & target) == target &&
        !(target & (D3D12_RESOURCE_STATE_RENDER_TARGET |
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                    D3D12_RESOURCE_STATE_DEPTH_WRITE |
                    D3D12_RESOURCE_STATE_COPY_DEST))) {
        return false;
    }

    return true;
}

// ============================================
// Alignment Helpers
// ============================================

// Align a value up to the specified alignment
template<typename T>
inline T AlignUp(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Constant buffer alignment (256 bytes for DX12)
constexpr uint32_t CONSTANT_BUFFER_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

// Texture data alignment
constexpr uint32_t TEXTURE_DATA_ALIGNMENT = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

// ============================================
// Subresource Calculation
// ============================================

// Calculate subresource index (matches D3D12CalcSubresource)
inline UINT CalcSubresource(UINT mipSlice, UINT arraySlice, UINT planeSlice, UINT mipLevels, UINT arraySize) {
    return mipSlice + arraySlice * mipLevels + planeSlice * mipLevels * arraySize;
}

// ============================================
// Format Conversion
// ============================================

// Forward declaration - will be implemented in DX12Utils.h
// DXGI_FORMAT ToDXGIFormat(ETextureFormat format);
// D3D12_RESOURCE_STATES ToD3D12ResourceState(EResourceState state);

} // namespace DX12
} // namespace RHI
