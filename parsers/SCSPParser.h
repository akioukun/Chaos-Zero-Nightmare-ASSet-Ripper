#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace SCSPParser {
    std::string ConvertToJson(const std::vector<uint8_t>& scsp_data);

    struct HeaderInfo {
        float width = 0;
        float height = 0;
        std::string version;
        std::string images_path;
        std::string audio_path;
        std::string hash;
    };

    HeaderInfo ExtractHeader(const std::vector<uint8_t>& scsp_data);
}
