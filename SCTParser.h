#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <map>
#include <tuple>
#include <astcenc.h>

namespace SCTParser {
    std::vector<uint8_t> ConvertToPNG(const std::vector<uint8_t>& data, bool verbose = false);
}
