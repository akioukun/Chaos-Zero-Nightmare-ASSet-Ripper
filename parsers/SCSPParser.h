#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <map>

namespace SCSPParser {
    std::string ConvertSCSPToJson(const std::vector<uint8_t>& scsp_data);

    struct HeaderInfo {
        float width = 0;
        float height = 0;
        std::string version;
        std::string images_path;
        std::string audio_path;
        std::string hash;
    };

    // Lightweight: decompresses and reads only the SCSP header fields.
    HeaderInfo ExtractHeader(const std::vector<uint8_t>& scsp_data);
}
