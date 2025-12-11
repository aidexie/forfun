#pragma once
#include "RHIResources.h"

// ============================================
// RHI Helper Functions
//
// These functions provide access to native handles for special use cases
// like ImGui rendering where we need direct access to ID3D11ShaderResourceView.
// ============================================

namespace RHI {

// Get native SRV handle for ImGui rendering
// Returns nullptr if texture is null or doesn't support SRV
void* GetNativeSRV(ITexture* texture);

// Get native SRV for a specific array slice (for cubemap face display)
void* GetNativeSRVSlice(ITexture* texture, uint32_t arraySlice, uint32_t mipLevel = 0);

// Get native RTV handle for render target binding
void* GetNativeRTV(ITexture* texture);

// Get native DSV handle for depth stencil binding
void* GetNativeDSV(ITexture* texture);

} // namespace RHI
