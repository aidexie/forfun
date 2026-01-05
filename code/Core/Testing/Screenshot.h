#pragma once
#include "RHI/RHIResources.h"
#include <string>

class CRenderPipeline;

// Screenshot utilities for automated testing
// Captures rendered frames to PNG files for AI visual verification
class CScreenshot {
public:
    // Capture from an RHI texture to PNG file
    // texture: RHI texture (must be R8G8B8A8 format)
    // path: Output file path
    // Returns true on success
    static bool Capture(RHI::ITexture* texture, const std::string& path);

    // Capture from render pipeline offscreen target
    static bool CaptureFromPipeline(CRenderPipeline* pipeline, const std::string& path);

    // Capture for test cases with automatic naming
    static bool CaptureTest(CRenderPipeline* pipeline, const std::string& testName, int frame);

private:
    static bool EnsureDirectoryExists(const std::string& path);
};
