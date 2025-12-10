#pragma once
#include "RHI/RHIResources.h"
#include <string>

// Load texture using WIC (Windows Imaging Component)
// Returns RHI texture on success, nullptr on failure
// Caller takes ownership of the returned texture
RHI::ITexture* LoadTextureWIC(const std::wstring& path, bool srgb = false);
