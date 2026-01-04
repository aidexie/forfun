#pragma once

#include <cstdint>

// Forward declaration - avoid including OIDN header in public interface
namespace oidn {
    class DeviceRef;
    class FilterRef;
}

// ============================================
// CLightmapDenoiser
// ============================================
// Intel Open Image Denoise (OIDN) wrapper for lightmap denoising.
// Uses RTLightmap filter optimized for path-traced lightmaps.
//
// Usage:
//   CLightmapDenoiser denoiser;
//   if (denoiser.Initialize()) {
//       denoiser.Denoise(colorBuffer, width, height);
//   }
//   denoiser.Shutdown();

class CLightmapDenoiser {
public:
    CLightmapDenoiser();
    ~CLightmapDenoiser();

    // Non-copyable
    CLightmapDenoiser(const CLightmapDenoiser&) = delete;
    CLightmapDenoiser& operator=(const CLightmapDenoiser&) = delete;

    // Initialize OIDN device and filter
    // Returns false if OIDN initialization fails
    bool Initialize();

    // Shutdown and release OIDN resources
    void Shutdown();

    // Check if denoiser is ready
    bool IsReady() const { return m_isReady; }

    // Denoise lightmap in-place
    // colorBuffer: RGB float buffer (width * height * 3 floats)
    // normalBuffer: Optional normal buffer for edge preservation (width * height * 3 floats)
    // albedoBuffer: Optional albedo buffer for better detail (width * height * 3 floats)
    // Returns true on success
    bool Denoise(
        float* colorBuffer,
        int width,
        int height,
        float* normalBuffer = nullptr,
        float* albedoBuffer = nullptr
    );

    // Get last error message (if any)
    const char* GetLastError() const { return m_lastError; }

private:
    void* m_device = nullptr;   // oidn::DeviceRef (opaque pointer)
    void* m_filter = nullptr;   // oidn::FilterRef (opaque pointer)
    bool m_isReady = false;
    const char* m_lastError = nullptr;
};
