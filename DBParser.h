
#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace DBParser
{
	std::string ConvertToJson(const std::vector<uint8_t> &data);
	bool ConvertToJsonToStream(const std::vector<uint8_t>& data, std::ostream& out) noexcept;
	std::string ConvertToJson(const std::vector<uint8_t> &decrypted);
	bool ConvertToJsonToStream(const std::vector<uint8_t>& data, std::ostream& out) noexcept;
}