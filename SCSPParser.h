#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <map>

namespace SCSPParser {
    std::string ConvertSCSPToJson(const std::vector<uint8_t>& scsp_data);
}
