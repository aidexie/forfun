#pragma once
#include <string>
#include <vector>

// HDR image data (Radiance RGBE format)
struct HdrImage {
    int width = 0;
    int height = 0;
    std::vector<float> data;  // RGB floats (width * height * 3)
};

// Load HDR file (.hdr, Radiance RGBE format)
bool LoadHdrFile(const std::string& path, HdrImage& outImage);
