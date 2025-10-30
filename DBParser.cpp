#include "DBParser.h"
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <map>
#include "Logger.h"

namespace DBParser
{

    static constexpr const char *KEY_HEX = "91AE4ED4644F585162EC1BD5EF24ADDBAF838242AEF51E97804B134FFD8CE5BB4F6E3E6451147CDF56C318E5E964C999C0D95CC860822E6B418BE465D79A036DBF67AB3DA72AB1023A4561F444E5CE858D23EA10FEB4899151AD7E43FF3E2419A97B4DD3AF4EF5C829E5AF4ACE9436F6B6B6382E9DFD26642099011A4899089C9D4B9F80BBB00A4CC73255CE1F78646E91C9C12313F5D840DC51457010D37D19615BB69888B42B19E749F993C00337E9332F89B320C173A5653848788798A771739E72DBC84C7946597149BDDAE4E3BD1A17856C85A555CFA24F6352D005933B50042BE0BA4C708DE8EBB52059B2059C9BFE90D8923DF74B43911BBC00BB6BFA";

    std::vector<uint8_t> DecryptDB(const std::vector<uint8_t> &data)
    {
        std::vector<uint8_t> key;
        key.reserve(256);
        for (size_t i = 0; i < 256; ++i)
        {
            std::string byteStr(KEY_HEX + i * 2, 2);
            key.push_back(static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16)));
        }

        for (int i = 0; i < 256; ++i)
        {
            std::vector<uint8_t> cur_k(key.begin() + i, key.end());
            cur_k.insert(cur_k.end(), key.begin(), key.begin() + i);
            bool found = true;
            for (size_t j = 0; j < 5; ++j)
            {
                if ((data[j] ^ cur_k[j % cur_k.size()]) != (uint8_t)"PLPcK"[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
            {
                std::vector<uint8_t> result(data.size());
                for (size_t j = 0; j < data.size(); ++j)
                {
                    result[j] = data[j] ^ cur_k[j % cur_k.size()];
                }
                return result;
            }
        }
        return {};
    }

    std::string ConvertToJson(const std::vector<uint8_t> &data)
    {
        try {
            std::vector<uint8_t> decrypted = DecryptDB(data);
            std::stringstream ss;
            size_t pos = 0;
            std::map<std::string, std::vector<uint8_t>> entries;
            Header header;

            // Read header
            if (decrypted.size() < 0x26)
                return "{}"; // Minimum header size

            // Magic (5 bytes)
            header.magic.assign(decrypted.begin() + pos, decrypted.begin() + pos + 5);
            pos += 5;

            // Version (u8)
            header.version = decrypted[pos++];

            // Header size (u16)
            header.headerSize = *reinterpret_cast<const uint16_t *>(&decrypted[pos]);
            pos += 2;

            if (header.headerSize != 0x26)
                return "{}";

            // unk (u8)
            header.unk = decrypted[pos++];

            // unk1 (u64)
            header.unk1 = *reinterpret_cast<const uint64_t *>(&decrypted[pos]);
            pos += 8;

            // default_file_count (u32)
            header.defaultFileCount = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
            pos += 4;

            // hash_table_count (u32)
            header.hashTableCount = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
            pos += 4;

            // hash_table_offset (UInt40)
            uint8_t offsetHi = decrypted[pos++];
            uint32_t offsetLo = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
            pos += 4;
            header.hashTableOffset = offsetLo + (static_cast<uint64_t>(offsetHi) << 32);

            // unk5 (u64)
            header.unk5 = *reinterpret_cast<const uint64_t *>(&decrypted[pos]);
            pos += 8;

            // go to hash table offset
            pos = header.hashTableOffset;

            // Root entry (5 bytes)
            std::vector<uint8_t> rootEntry(decrypted.begin() + pos, decrypted.begin() + pos + 5);
            pos += 5;

            if (rootEntry[4] != 1)
                return "{}";
            uint32_t rootSize = *reinterpret_cast<const uint32_t *>(&rootEntry[0]);
            if (rootSize != 5 * (header.hashTableCount + 1))
                return "{}";

            // Hash table entries
            std::vector<HashTableEntry> hashTableEntries;
            // Process each entry in the hash table
            for (uint32_t i = 0; i < header.hashTableCount; i++)
            {
                uint8_t entryHi = decrypted[pos++];
                uint32_t entryLo = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
                pos += 4;
                HashTableEntry entry;
                entry.entryOffset = entryLo + (static_cast<uint64_t>(entryHi) << 32);
                hashTableEntries.push_back(entry);

                if (entry.entryOffset != 0)
                {
                    size_t currentPos = entry.entryOffset;

                    while (true)
                    {
                        // Read FileChunkHeader
                        FileChunkHeader chunk;
                        chunk.entrySize = *reinterpret_cast<const uint32_t *>(&decrypted[currentPos]);
                        currentPos += 4;

                        chunk.entryType = decrypted[currentPos++];
                        chunk.fileNameLength = decrypted[currentPos++];

                        chunk.fileSize = *reinterpret_cast<const uint32_t *>(&decrypted[currentPos]);
                        currentPos += 4;

                        uint8_t nextEntryHi = decrypted[currentPos++];
                        uint32_t nextEntryLo = *reinterpret_cast<const uint32_t *>(&decrypted[currentPos]);
                        currentPos += 4;
                        chunk.nextEntry = nextEntryLo + (static_cast<uint64_t>(nextEntryHi) << 32);

                        // Read file name
                        std::string fileName(reinterpret_cast<const char *>(&decrypted[currentPos]), chunk.fileNameLength);
                        currentPos += chunk.fileNameLength;

                        // Read file data
                        std::vector<uint8_t> fileData(decrypted.begin() + currentPos,
                                                      decrypted.begin() + currentPos + chunk.fileSize);
                        currentPos += chunk.fileSize;

                        // Save entry
                        entries[fileName] = fileData;

                        if (chunk.nextEntry == 0)
                            break;
                        currentPos = chunk.nextEntry;
                    }
                }
            }

            // Process the database data
            uint32_t rows = 0;
            uint32_t cols = 0;
            std::vector<std::string> colNames;

            // Get rows and cols
            auto rowsIt = entries.find("\trows");
            if (rowsIt != entries.end() && !rowsIt->second.empty())
            {
                rows = *reinterpret_cast<const uint32_t *>(rowsIt->second.data());
            }

            auto colsIt = entries.find("\tcols");
            if (colsIt != entries.end() && !colsIt->second.empty())
            {
                cols = *reinterpret_cast<const uint32_t *>(colsIt->second.data());
            }

            // Get column names
            for (uint32_t col = 0; col < cols; col++)
            {
                std::string key = "\t" + std::to_string(col);
                auto it = entries.find(key);
                if (it != entries.end())
                {
                    colNames.push_back(std::string(it->second.begin(), it->second.end()));
                }
            }

            // Build final JSON
            ss << "[\n";
            for (uint32_t row = 0; row < rows; row++)
            {
                std::string entryKey = "\t\t" + std::to_string(row);
                auto entryIt = entries.find(entryKey);

                if (entryIt != entries.end())
                {
                    auto actualEntryIt = entries.find(std::string(entryIt->second.begin(), entryIt->second.end()));
                    if (actualEntryIt != entries.end())
                    {
                        // Split data by column
                        std::vector<std::string> values;
                        size_t start = 0;
                        for (size_t i = 0; i < actualEntryIt->second.size(); i++)
                        {
                            if (actualEntryIt->second[i] == 0)
                            {
                                values.push_back(std::string(
                                    actualEntryIt->second.begin() + start,
                                    actualEntryIt->second.begin() + i));
                                start = i + 1;
                            }
                        }

                        // Generate JSON for the row
                        ss << "  {\n";
                        for (size_t i = 0; i < colNames.size() && i < values.size(); i++)
                        {
                            ss << "    \"" << colNames[i] << "\": \"" << values[i] << "\"";
                            if (i < colNames.size() - 1 && i < values.size() - 1)
                                ss << ",";
                            ss << "\n";
                        }
                        ss << "  }";
                        if (row < rows - 1)
                            ss << ",";
                        ss << "\n";
                    }
                }
            }
            ss << "]";

            return ss.str();
        } catch (const std::exception&) {
            return "{}";
        } catch (...) {
            return "{}";
        }
    }

    bool ConvertToJsonToStream(const std::vector<uint8_t>& data, std::ostream& out) noexcept
    {
        try {
            LogInfo("DB ConvertToJsonToStream begin");
            std::vector<uint8_t> decrypted = DecryptDB(data);
            size_t pos = 0;
            std::map<std::string, std::vector<uint8_t>> entries;
            Header header;

            if (decrypted.size() < 0x26) { LogError("DB decrypted too small"); return false; }

            header.magic.assign(decrypted.begin() + pos, decrypted.begin() + pos + 5);
            pos += 5;
            header.version = decrypted[pos++];
            header.headerSize = *reinterpret_cast<const uint16_t *>(&decrypted[pos]);
            pos += 2;
            if (header.headerSize != 0x26) { LogError("DB invalid header size"); return false; }
            header.unk = decrypted[pos++];
            header.unk1 = *reinterpret_cast<const uint64_t *>(&decrypted[pos]);
            pos += 8;
            header.defaultFileCount = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
            pos += 4;
            header.hashTableCount = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
            pos += 4;
            {
                uint8_t offsetHi = decrypted[pos++];
                uint32_t offsetLo = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
                pos += 4;
                header.hashTableOffset = offsetLo + (static_cast<uint64_t>(offsetHi) << 32);
            }
            header.unk5 = *reinterpret_cast<const uint64_t *>(&decrypted[pos]);
            pos += 8;
            pos = header.hashTableOffset;

            std::vector<uint8_t> rootEntry(decrypted.begin() + pos, decrypted.begin() + pos + 5);
            pos += 5;
            if (rootEntry[4] != 1) { LogError("DB rootEntry invalid"); return false; }
            uint32_t rootSize = *reinterpret_cast<const uint32_t *>(&rootEntry[0]);
            if (rootSize != 5 * (header.hashTableCount + 1)) { LogError("DB rootSize mismatch"); return false; }

            std::vector<HashTableEntry> hashTableEntries;
            for (uint32_t i = 0; i < header.hashTableCount; i++) {
                uint8_t entryHi = decrypted[pos++];
                uint32_t entryLo = *reinterpret_cast<const uint32_t *>(&decrypted[pos]);
                pos += 4;
                HashTableEntry entry;
                entry.entryOffset = entryLo + (static_cast<uint64_t>(entryHi) << 32);
                hashTableEntries.push_back(entry);

                if (entry.entryOffset != 0) {
                    size_t currentPos = entry.entryOffset;
                    while (true) {
                        FileChunkHeader chunk;
                        chunk.entrySize = *reinterpret_cast<const uint32_t *>(&decrypted[currentPos]);
                        currentPos += 4;
                        chunk.entryType = decrypted[currentPos++];
                        chunk.fileNameLength = decrypted[currentPos++];
                        chunk.fileSize = *reinterpret_cast<const uint32_t *>(&decrypted[currentPos]);
                        currentPos += 4;
                        uint8_t nextEntryHi = decrypted[currentPos++];
                        uint32_t nextEntryLo = *reinterpret_cast<const uint32_t *>(&decrypted[currentPos]);
                        currentPos += 4;
                        chunk.nextEntry = nextEntryLo + (static_cast<uint64_t>(nextEntryHi) << 32);
                        std::string fileName(reinterpret_cast<const char *>(&decrypted[currentPos]), chunk.fileNameLength);
                        currentPos += chunk.fileNameLength;
                        std::vector<uint8_t> fileData(decrypted.begin() + currentPos,
                                                      decrypted.begin() + currentPos + chunk.fileSize);
                        currentPos += chunk.fileSize;
                        entries[fileName] = std::move(fileData);
                        if (chunk.nextEntry == 0) break;
                        currentPos = chunk.nextEntry;
                    }
                }
            }

            uint32_t rows = 0, cols = 0;
            std::vector<std::string> colNames;
            auto rowsIt = entries.find("\trows");
            if (rowsIt != entries.end() && !rowsIt->second.empty()) rows = *reinterpret_cast<const uint32_t *>(rowsIt->second.data());
            auto colsIt = entries.find("\tcols");
            if (colsIt != entries.end() && !colsIt->second.empty()) cols = *reinterpret_cast<const uint32_t *>(colsIt->second.data());
            for (uint32_t col = 0; col < cols; col++) {
                std::string key = "\t" + std::to_string(col);
                auto it = entries.find(key);
                if (it != entries.end()) colNames.push_back(std::string(it->second.begin(), it->second.end()));
            }

            out << "[\n";
            for (uint32_t row = 0; row < rows; row++) {
                if ((row % 1000) == 0) LogInfo(std::string("DB JSON rows written: ") + std::to_string(row));
                std::string entryKey = "\t\t" + std::to_string(row);
                auto entryIt = entries.find(entryKey);
                if (entryIt != entries.end()) {
                    auto actualEntryIt = entries.find(std::string(entryIt->second.begin(), entryIt->second.end()));
                    if (actualEntryIt != entries.end()) {
                        std::vector<std::string> values;
                        size_t start = 0;
                        for (size_t i = 0; i < actualEntryIt->second.size(); i++) {
                            if (actualEntryIt->second[i] == 0) {
                                values.push_back(std::string(actualEntryIt->second.begin() + start,
                                                            actualEntryIt->second.begin() + i));
                                start = i + 1;
                            }
                        }
                        out << "  {\n";
                        for (size_t i = 0; i < colNames.size() && i < values.size(); i++) {
                            out << "    \"" << colNames[i] << "\": \"" << values[i] << "\"";
                            if (i < colNames.size() - 1 && i < values.size() - 1) out << ",";
                            out << "\n";
                        }
                        out << "  }";
                        if (row < rows - 1) out << ",";
                        out << "\n";
                    }
                }
            }
            out << "]";
            LogInfo("DB ConvertToJsonToStream end");
            return true;
        } catch (...) {
            LogError("DB ConvertToJsonToStream exception");
            return false;
        }
    }

}