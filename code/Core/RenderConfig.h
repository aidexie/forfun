#pragma once
#include "RHI/RHICommon.h"
#include <string>

// ============================================
// Render Configuration
// ============================================

// Render pipeline selection
enum class ERenderPipeline {
    Forward,   // Forward+ rendering (clustered lighting)
    Deferred   // True deferred rendering
};

struct SRenderConfig {
    // Rendering backend selection
    RHI::EBackend backend = RHI::EBackend::DX11;

    // Render pipeline selection
    ERenderPipeline pipeline = ERenderPipeline::Forward;

    // Window settings
    uint32_t windowWidth = 1280;
    uint32_t windowHeight = 720;
    bool fullscreen = false;
    bool vsync = true;

    // Graphics settings
    uint32_t msaaSamples = 1;  // 1, 2, 4, 8
    bool enableValidation = false;  // DX12 debug layer, DX11 debug device

    // Depth buffer settings
    bool useReversedZ = true;  // Reversed-Z for better depth precision

    // Load configuration from file
    // Returns true if loaded successfully, false if file not found (uses defaults)
    static bool Load(const std::string& path, SRenderConfig& outConfig);

    // Save configuration to file
    static bool Save(const std::string& path, const SRenderConfig& config);

    // Get default config file path
    static std::string GetDefaultPath();
};

// ============================================
// Global Render Config Accessor
// ============================================
// Use this to access the global render config from anywhere in the engine.
// The config is stored in main.cpp and registered via SetGlobalRenderConfig().

const SRenderConfig& GetRenderConfig();
void SetGlobalRenderConfig(const SRenderConfig* config);

// Convenience function for reversed-Z check (most common usage)
inline bool UseReversedZ() {
    return GetRenderConfig().useReversedZ;
}

// Helper function for depth comparison function
// In reversed-Z: use Greater/GreaterEqual instead of Less/LessEqual
inline RHI::EComparisonFunc GetDepthComparisonFunc(bool orEqual = false) {
    if (UseReversedZ()) {
        return orEqual ? RHI::EComparisonFunc::GreaterEqual : RHI::EComparisonFunc::Greater;
    }
    return orEqual ? RHI::EComparisonFunc::LessEqual : RHI::EComparisonFunc::Less;
}
