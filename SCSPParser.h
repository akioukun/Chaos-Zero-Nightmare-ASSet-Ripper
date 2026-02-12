#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <map>

namespace SCSPParser {

    struct Header {
        uint32_t string_offset;
        uint32_t string_length;
        uint32_t hdr_version;
        float width;
        float height;
        std::string hash;
        std::string version;
        std::string images_path;
        std::string audio_path;
    };

    std::vector<uint8_t> DecompressSCSP(const std::vector<uint8_t>& data);

    std::string ParseSCSPToJson(const std::vector<uint8_t>& decompressed_data);

    std::string ConvertSCSPToJson(const std::vector<uint8_t>& scsp_data);

}
