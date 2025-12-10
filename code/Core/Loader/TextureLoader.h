#pragma once
#include <string>

// Load texture using WIC (Windows Imaging Component)
// device: void* to ID3D11Device
// outSRV: void** to receive ID3D11ShaderResourceView*
// Returns true on success
bool LoadTextureWIC(void* device, const std::wstring& path, void** outSRV, bool srgb = false);
