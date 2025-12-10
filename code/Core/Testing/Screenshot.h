#pragma once
#include <string>

class CForwardRenderPipeline;

// Screenshot utilities for automated testing
// Captures rendered frames to PNG files for AI visual verification
class CScreenshot {
public:
    // Capture from a texture to PNG file
    // texture: void* to native texture (ID3D11Texture2D* for D3D11)
    // path: Output file path
    // Returns true on success
    static bool Capture(void* texture, const std::string& path);

    // Capture from ForwardRenderPipeline offscreen target
    static bool CaptureFromPipeline(CForwardRenderPipeline* pipeline, const std::string& path);

    // Capture for test cases with automatic naming
    static bool CaptureTest(CForwardRenderPipeline* pipeline, const std::string& testName, int frame);

private:
    static bool EnsureDirectoryExists(const std::string& path);
};
