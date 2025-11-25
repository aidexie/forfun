#pragma once
#include <string>

struct ID3D11Texture2D;
class CMainPass;

// Screenshot utilities for automated testing
// Captures rendered frames to PNG files for AI visual verification
class CScreenshot {
public:
    // Capture from a D3D11 texture to PNG file
    // texture: Source texture (must be R8G8B8A8_UNORM or R8G8B8A8_UNORM_SRGB)
    // path: Output file path (e.g., "E:/forfun/debug/screenshots/test.png")
    // Returns true on success
    static bool Capture(ID3D11Texture2D* texture, const std::string& path);

    // Capture from MainPass offscreen target (convenience wrapper)
    // mainPass: The main rendering pass
    // path: Output file path
    static bool CaptureFromMainPass(CMainPass* mainPass, const std::string& path);

    // Capture for test cases with automatic naming
    // mainPass: The main rendering pass
    // testName: Name of the test (e.g., "TestRayCast")
    // frame: Frame number
    // Saves to: E:/forfun/debug/screenshots/{testName}_frame{frame}.png
    static bool CaptureTest(CMainPass* mainPass, const std::string& testName, int frame);

private:
    static bool EnsureDirectoryExists(const std::string& path);
};
