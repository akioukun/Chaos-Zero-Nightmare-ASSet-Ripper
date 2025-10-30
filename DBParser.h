
#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace DBParser
{

	struct Header
	{
		std::vector<uint8_t> magic; // 5 bytes
		uint8_t version;
		uint16_t headerSize;
		uint8_t unk;
		uint64_t unk1;
		uint32_t defaultFileCount;
		uint32_t hashTableCount;
		uint64_t hashTableOffset; // UInt40
		uint64_t unk5;
	};

	struct HashTableEntry
	{
		uint64_t entryOffset; // UInt40
	};

	struct FileChunkHeader
	{
		uint32_t entrySize;
		uint8_t entryType;
		uint8_t fileNameLength;
		uint32_t fileSize;
		uint64_t nextEntry; // UInt40
	};

	std::vector<uint8_t> DecryptDB(const std::vector<uint8_t> &data);
	std::string ConvertToJson(const std::vector<uint8_t> &decrypted);
	bool ConvertToJsonToStream(const std::vector<uint8_t>& data, std::ostream& out) noexcept;
}