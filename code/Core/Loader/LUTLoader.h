#pragma once
#include <string>
#include <vector>

// 3D LUT data (parsed from .cube file)
struct SLUTData {
    uint32_t size = 0;                  // Cube dimension (e.g., 32 for 32x32x32)
    std::vector<float> data;            // RGB floats (size^3 * 3)
    float domainMin[3] = {0, 0, 0};     // Input domain minimum
    float domainMax[3] = {1, 1, 1};     // Input domain maximum
};

// Load .cube LUT file (Adobe/Resolve format)
// Returns true on success, false on error
// outLUT.size will be 0 on failure
bool LoadCubeFile(const std::string& path, SLUTData& outLUT);

// Generate identity (neutral) LUT data
// size: cube dimension (typically 32)
void GenerateIdentityLUT(uint32_t size, SLUTData& outLUT);
