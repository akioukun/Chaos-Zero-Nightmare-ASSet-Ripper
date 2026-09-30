#pragma once
#include <vector>
#include <cstdint>

namespace SCTParser {
    struct RGBAImage {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
    };

    RGBAImage ConvertToRGBA(const std::vector<uint8_t> &data);
    std::vector<uint8_t> ConvertToPNG(const std::vector<uint8_t> &data);
}
