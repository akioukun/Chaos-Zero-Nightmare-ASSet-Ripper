#pragma once
#include "ArchiveBase.h"
#include <string>
#include <vector>
#include <map>
#include <fstream>

class SSRArchive : public ArchiveBase {
public:
    SSRArchive(const std::wstring& manifest_path);
    ~SSRArchive() override = default;

    void Scan(std::atomic<float>& progress) override;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override;

private:
    struct ChunkInfo {
        uint64_t index;
        uint32_t chunk_id;
        uint16_t group_idx;
        std::string name;
        uint64_t size_bytes;
        uint64_t compressed_size;
        uint64_t global_offset;
    };

    struct SSRAFileInfo {
        uint64_t chunk_index;
        uint64_t offset;
        uint64_t size;
        uint64_t compressed_size;
        bool is_compressed;
        bool is_encrypted;
    };

    std::wstring manifest_path;
    std::wstring chunks_dir;
    
    std::vector<ChunkInfo> chunks;
    std::map<uint16_t, std::string> group_names;
    std::map<std::string, SSRAFileInfo> file_map;
    
    std::string GetStringFromTable(const std::vector<uint8_t>& string_table, uint64_t offset) const;
};
