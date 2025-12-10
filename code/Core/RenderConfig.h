#pragma once
#include "RHI/RHICommon.h"
#include <string>

// ============================================
// Render Configuration
// ============================================

struct SRenderConfig {
    // Rendering backend selection
    RHI::EBackend backend = RHI::EBackend::DX11;

    // Window settings
    uint32_t windowWidth = 1280;
    uint32_t windowHeight = 720;
    bool fullscreen = false;
    bool vsync = true;

    // Graphics settings
    uint32_t msaaSamples = 1;  // 1, 2, 4, 8
    bool enableValidation = false;  // DX12 debug layer, DX11 debug device

    // Load configuration from file
    // Returns true if loaded successfully, false if file not found (uses defaults)
    static bool Load(const std::string& path, SRenderConfig& outConfig);

    // Save configuration to file
    static bool Save(const std::string& path, const SRenderConfig& config);

    // Get default config file path
    static std::string GetDefaultPath();
};
