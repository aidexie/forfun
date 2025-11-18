#include "HdrLoader.h"
#include <fstream>
#include <cstring>
#include <cmath>

// RGBE to float conversion
static void RGBEToFloat(unsigned char* rgbe, float* rgb) {
    if (rgbe[3]) {  // Non-zero exponent
        float f = ldexp(1.0f, rgbe[3] - (128 + 8));
        rgb[0] = rgbe[0] * f;
        rgb[1] = rgbe[1] * f;
        rgb[2] = rgbe[2] * f;
    } else {
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
    }
}

bool LoadHdrFile(const std::string& path, HdrImage& outImage) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Read header
    char line[256];
    file.getline(line, 256);

    // Check magic number
    if (strcmp(line, "#?RADIANCE") != 0 && strcmp(line, "#?RGBE") != 0) {
        return false;
    }

    // Skip header lines until we find resolution
    int width = 0, height = 0;
    while (file.getline(line, 256)) {
        if (line[0] == '\0') continue;  // Skip empty lines

        // Look for resolution line: "-Y height +X width"
        if (line[0] == '-' && line[1] == 'Y') {
            if (sscanf(line, "-Y %d +X %d", &height, &width) == 2) {
                break;
            }
        }
    }

    if (width <= 0 || height <= 0) {
        return false;
    }

    outImage.width = width;
    outImage.height = height;
    outImage.data.resize(width * height * 3);

    // Read scanlines
    std::vector<unsigned char> scanline(width * 4);

    for (int y = 0; y < height; ++y) {
        // Read scanline header
        unsigned char rgbe[4];
        file.read((char*)rgbe, 4);

        // Check for new RLE format
        if (rgbe[0] == 2 && rgbe[1] == 2 && rgbe[2] < 128) {
            // New RLE format
            int scanline_width = (rgbe[2] << 8) | rgbe[3];
            if (scanline_width != width) {
                return false;
            }

            // Read each component separately (RLE compressed)
            for (int i = 0; i < 4; ++i) {
                int x = 0;
                while (x < width) {
                    unsigned char code;
                    file.read((char*)&code, 1);

                    if (code > 128) {  // Run
                        code &= 127;
                        unsigned char value;
                        file.read((char*)&value, 1);
                        for (int j = 0; j < code; ++j) {
                            scanline[x * 4 + i] = value;
                            ++x;
                        }
                    } else {  // Non-run
                        for (int j = 0; j < code; ++j) {
                            unsigned char value;
                            file.read((char*)&value, 1);
                            scanline[x * 4 + i] = value;
                            ++x;
                        }
                    }
                }
            }
        } else {
            // Old format (uncompressed)
            scanline[0] = rgbe[0];
            scanline[1] = rgbe[1];
            scanline[2] = rgbe[2];
            scanline[3] = rgbe[3];
            file.read((char*)&scanline[4], (width - 1) * 4);
        }

        // Convert RGBE to float
        for (int x = 0; x < width; ++x) {
            float* rgb = &outImage.data[(y * width + x) * 3];
            RGBEToFloat(&scanline[x * 4], rgb);
        }
    }

    return true;
}
