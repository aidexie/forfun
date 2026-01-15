#include "LUTLoader.h"
#include "Core/FFLog.h"
#include "Core/PathManager.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>

// Trim whitespace from both ends of a string
static std::string Trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

// Check if line is a comment or empty
static bool IsCommentOrEmpty(const std::string& line) {
    std::string trimmed = Trim(line);
    return trimmed.empty() || trimmed[0] == '#';
}

// Parse a line with 3 float values (RGB triplet)
static bool ParseRGBLine(const std::string& line, float& r, float& g, float& b) {
    std::istringstream iss(line);
    if (!(iss >> r >> g >> b)) {
        return false;
    }
    return true;
}

bool LoadCubeFile(const std::string& path, SLUTData& outLUT) {
    // Reset output
    outLUT.size = 0;
    outLUT.data.clear();
    outLUT.domainMin[0] = outLUT.domainMin[1] = outLUT.domainMin[2] = 0.0f;
    outLUT.domainMax[0] = outLUT.domainMax[1] = outLUT.domainMax[2] = 1.0f;

    // Get absolute path
    std::string absolutePath = FFPath::GetAbsolutePath(path);

    std::ifstream file(absolutePath);
    if (!file.is_open()) {
        CFFLog::Error("[LUTLoader] Failed to open file: %s", absolutePath.c_str());
        return false;
    }

    uint32_t lutSize = 0;
    std::vector<float> colorData;
    int lineNumber = 0;
    bool readingData = false;
    uint32_t expectedColors = 0;
    uint32_t readColors = 0;

    std::string line;
    while (std::getline(file, line)) {
        lineNumber++;

        // Skip comments and empty lines
        if (IsCommentOrEmpty(line)) {
            continue;
        }

        std::string trimmed = Trim(line);

        // Parse header keywords
        if (!readingData) {
            // Check for TITLE (optional, ignored)
            if (trimmed.find("TITLE") == 0) {
                continue;
            }

            // Check for DOMAIN_MIN
            if (trimmed.find("DOMAIN_MIN") == 0) {
                std::istringstream iss(trimmed.substr(10));
                if (!(iss >> outLUT.domainMin[0] >> outLUT.domainMin[1] >> outLUT.domainMin[2])) {
                    CFFLog::Warning("[LUTLoader] Invalid DOMAIN_MIN at line %d", lineNumber);
                }
                continue;
            }

            // Check for DOMAIN_MAX
            if (trimmed.find("DOMAIN_MAX") == 0) {
                std::istringstream iss(trimmed.substr(10));
                if (!(iss >> outLUT.domainMax[0] >> outLUT.domainMax[1] >> outLUT.domainMax[2])) {
                    CFFLog::Warning("[LUTLoader] Invalid DOMAIN_MAX at line %d", lineNumber);
                }
                continue;
            }

            // Check for LUT_3D_SIZE
            if (trimmed.find("LUT_3D_SIZE") == 0) {
                std::istringstream iss(trimmed.substr(11));
                if (!(iss >> lutSize)) {
                    CFFLog::Error("[LUTLoader] Invalid LUT_3D_SIZE at line %d", lineNumber);
                    return false;
                }

                // Validate size (common sizes: 17, 32, 33, 64, 65)
                if (lutSize < 2 || lutSize > 256) {
                    CFFLog::Error("[LUTLoader] Invalid LUT size %u (must be 2-256)", lutSize);
                    return false;
                }

                expectedColors = lutSize * lutSize * lutSize;
                colorData.reserve(expectedColors * 3);
                readingData = true;
                continue;
            }

            // Check for LUT_1D_SIZE (not supported)
            if (trimmed.find("LUT_1D_SIZE") == 0) {
                CFFLog::Error("[LUTLoader] 1D LUTs are not supported");
                return false;
            }
        }

        // Parse color data
        if (readingData) {
            float r, g, b;
            if (!ParseRGBLine(trimmed, r, g, b)) {
                CFFLog::Error("[LUTLoader] Invalid RGB data at line %d: %s", lineNumber, trimmed.c_str());
                return false;
            }

            // Clamp values to valid range with warning
            if (r < 0.0f || r > 1.0f || g < 0.0f || g > 1.0f || b < 0.0f || b > 1.0f) {
                // Only warn once per file to avoid log spam
                static std::string lastWarnedFile;
                if (lastWarnedFile != absolutePath) {
                    CFFLog::Warning("[LUTLoader] Color values outside [0,1] range in %s, clamping", path.c_str());
                    lastWarnedFile = absolutePath;
                }
                r = std::max(0.0f, std::min(1.0f, r));
                g = std::max(0.0f, std::min(1.0f, g));
                b = std::max(0.0f, std::min(1.0f, b));
            }

            colorData.push_back(r);
            colorData.push_back(g);
            colorData.push_back(b);
            readColors++;

            if (readColors >= expectedColors) {
                break;  // Done reading
            }
        }
    }

    // Validate we got all the data
    if (lutSize == 0) {
        CFFLog::Error("[LUTLoader] No LUT_3D_SIZE found in file");
        return false;
    }

    if (readColors != expectedColors) {
        CFFLog::Error("[LUTLoader] Expected %u colors, got %u", expectedColors, readColors);
        return false;
    }

    // Success
    outLUT.size = lutSize;
    outLUT.data = std::move(colorData);

    CFFLog::Info("[LUTLoader] Loaded %ux%ux%u LUT from: %s", lutSize, lutSize, lutSize, path.c_str());
    return true;
}

void GenerateIdentityLUT(uint32_t size, SLUTData& outLUT) {
    outLUT.size = size;
    outLUT.domainMin[0] = outLUT.domainMin[1] = outLUT.domainMin[2] = 0.0f;
    outLUT.domainMax[0] = outLUT.domainMax[1] = outLUT.domainMax[2] = 1.0f;

    uint32_t totalColors = size * size * size;
    outLUT.data.resize(totalColors * 3);

    // Generate identity LUT: output = input
    // .cube format uses R as fastest varying, then G, then B
    // So the order is: (0,0,0), (1,0,0), (2,0,0), ..., (0,1,0), (1,1,0), ...
    float scale = 1.0f / (size - 1);
    uint32_t idx = 0;

    for (uint32_t b = 0; b < size; ++b) {
        for (uint32_t g = 0; g < size; ++g) {
            for (uint32_t r = 0; r < size; ++r) {
                outLUT.data[idx++] = r * scale;
                outLUT.data[idx++] = g * scale;
                outLUT.data[idx++] = b * scale;
            }
        }
    }
}
