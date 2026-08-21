#include "SSRArchive.h"
#include "core/Logger.h"
#include "core/Core.h"
#include "libs/zstd/zstd.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

#pragma pack(push, 1)
struct SSRAHeader {
    char magic[4]; // "SSRA"
    uint32_t version;
    uint32_t unk0;
    uint32_t chunk_count;
    uint32_t file_count;
    uint32_t flags;
    uint64_t string_table_offset;
    uint64_t string_table_size;
    uint64_t chunk_table_offset;
    uint64_t file_table_offset;
    uint64_t extra;
};

struct SSRAChunkEntry {
    uint32_t chunk_id;        // 0x00: Chunk ID (e.g. 0, 1, 2, ...)
    uint16_t group_idx;       // 0x04: Group index (0 = base, 0xFFFF = hotfix)
    uint16_t unk_06;          // 0x06: Unknown
    uint64_t uncomp_sz;       // 0x08: Uncompressed size
    uint64_t comp_sz;         // 0x10: Compressed size
    uint64_t checksum;        // 0x18: Checksum
};

struct SSRAFileEntry {
    uint32_t path_hash;       // 0x00: Hash of path
    uint32_t unk_04;          // 0x04: Unknown
    uint64_t chunk_file_off;  // 0x08: Offset in chunk file
    uint32_t comp_sz;         // 0x10: Compressed/raw size in chunk
    uint32_t uncomp_sz;       // 0x14: Uncompressed size
    uint32_t unk_18;          // 0x18: Unknown
    uint32_t name_off;        // 0x1C: Byte offset into string table
    uint8_t  is_compressed;   // 0x20: 1 = compressed (Zstandard), 0 = uncompressed
    uint8_t  is_encrypted;    // 0x21: 1 = encrypted (AES-128), 0 = unencrypted
    uint16_t chunk_idx;       // 0x22: Chunk index
    uint32_t flags;           // 0x24: Flags (bit 0 = deleted/tombstone)
};
#pragma pack(pop)

SSRArchive::SSRArchive(const std::wstring& manifest_path) : manifest_path(manifest_path)
{
    this->pack_path = manifest_path;
    this->type = PackType::SSRA;
    std::filesystem::path p(manifest_path);
    chunks_dir = (p.parent_path() / L"chunks").wstring();
}

std::string SSRArchive::GetStringFromTable(const std::vector<uint8_t>& string_table, uint64_t offset) const
{
    if (offset >= string_table.size())
        return "";
    const char* str_ptr = reinterpret_cast<const char*>(string_table.data() + offset);
    size_t max_len = string_table.size() - offset;
    size_t len = 0;
    while (len < max_len && str_ptr[len] != '\0') {
        len++;
    }
    return std::string(str_ptr, len);
}

void SSRArchive::Scan(std::atomic<float>& progress)
{
    LogInfo("Scanning SSRA manifest: " + Core::WStringToUtf8(manifest_path));

    std::ifstream file(manifest_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LogError("Failed to open manifest.ssra: " + Core::WStringToUtf8(manifest_path));
        return;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size < sizeof(SSRAHeader)) {
        LogError("manifest.ssra too small");
        return;
    }

    std::vector<uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        LogError("Failed to read manifest.ssra");
        return;
    }

    SSRAHeader header;
    std::memcpy(&header, data.data(), sizeof(SSRAHeader));

    if (std::memcmp(header.magic, "SSRA", 4) != 0) {
        LogError("Invalid SSRA magic");
        return;
    }

    // Read string table
    std::vector<uint8_t> string_table;
    if (header.string_table_offset + header.string_table_size <= data.size()) {
        string_table.assign(data.begin() + header.string_table_offset,
                            data.begin() + header.string_table_offset + header.string_table_size);
    }

    // Read group table (GRPS) if present after the string table
    group_names.clear();
    uint64_t grps_offset = header.string_table_offset + header.string_table_size;
    if (grps_offset + 16 <= data.size()) {
        char grps_magic[4];
        std::memcpy(grps_magic, data.data() + grps_offset, 4);
        if (std::memcmp(grps_magic, "GRPS", 4) == 0) {
            uint32_t group_count = *reinterpret_cast<const uint32_t*>(data.data() + grps_offset + 4);
            uint32_t sec_str_size = *reinterpret_cast<const uint32_t*>(data.data() + grps_offset + 8);
            uint64_t entries_offset = grps_offset + 16;
            uint64_t sec_str_offset = entries_offset + (static_cast<uint64_t>(group_count) * 24);

            if (sec_str_offset + sec_str_size <= data.size()) {
                std::vector<uint8_t> sec_string_table(
                    data.begin() + sec_str_offset,
                    data.begin() + sec_str_offset + sec_str_size
                );

                for (uint32_t g = 0; g < group_count; ++g) {
                    uint64_t entry_addr = entries_offset + (g * 24);
                    uint16_t group_idx = *reinterpret_cast<const uint16_t*>(data.data() + entry_addr);
                    uint32_t name_off = *reinterpret_cast<const uint32_t*>(data.data() + entry_addr + 4);
                    std::string gname = GetStringFromTable(sec_string_table, name_off);
                    if (!gname.empty()) {
                        group_names[group_idx] = gname;
                        LogInfo("Discovered SSRA group " + std::to_string(group_idx) + " -> '" + gname + "'");
                    }
                }
            }
        }
    }

    // Read chunks
    chunks.clear();
    for (uint32_t i = 0; i < header.chunk_count; ++i) {
        uint64_t entry_off = header.chunk_table_offset + (i * sizeof(SSRAChunkEntry));
        if (entry_off + sizeof(SSRAChunkEntry) > data.size()) break;

        SSRAChunkEntry chunk_entry;
        std::memcpy(&chunk_entry, data.data() + entry_off, sizeof(SSRAChunkEntry));

        ChunkInfo c_info;
        c_info.index = i;
        c_info.chunk_id = chunk_entry.chunk_id;
        c_info.group_idx = chunk_entry.group_idx;
        char buf[64];
        if (chunk_entry.group_idx == 0xFFFF) {
            snprintf(buf, sizeof(buf), "hotfix_%04u.ssrc", chunk_entry.chunk_id);
        } else {
            auto it = group_names.find(chunk_entry.group_idx);
            if (it != group_names.end() && !it->second.empty()) {
                snprintf(buf, sizeof(buf), "%s_%04u.ssrc", it->second.c_str(), chunk_entry.chunk_id);
            } else {
                snprintf(buf, sizeof(buf), "group_%u_%04u.ssrc", chunk_entry.group_idx, chunk_entry.chunk_id);
            }
        }
        c_info.name = buf;
        c_info.size_bytes = chunk_entry.uncomp_sz;
        c_info.compressed_size = chunk_entry.comp_sz;
        c_info.global_offset = 0; // Will compute next
        chunks.push_back(c_info);
    }

    // Compute global offsets per group
    std::map<uint16_t, std::vector<ChunkInfo*>> group_chunks;
    for (auto& c : chunks) {
        group_chunks[c.group_idx].push_back(&c);
    }
    
    for (auto& [grp, list] : group_chunks) {
        std::sort(list.begin(), list.end(), [](ChunkInfo* a, ChunkInfo* b) {
            return a->chunk_id < b->chunk_id;
        });
        uint64_t current_offset = 0;
        for (ChunkInfo* c : list) {
            c->global_offset = current_offset;
            current_offset += c->compressed_size;
        }
    }

    // Read files
    file_map.clear();
    for (uint32_t i = 0; i < header.file_count; ++i) {
        uint64_t entry_off = header.file_table_offset + (i * sizeof(SSRAFileEntry));
        if (entry_off + sizeof(SSRAFileEntry) > data.size()) break;

        SSRAFileEntry file_entry;
        std::memcpy(&file_entry, data.data() + entry_off, sizeof(SSRAFileEntry));

        // Skip tombstone/deleted files
        if ((file_entry.flags & 1) != 0) {
            continue;
        }

        std::string filename = GetStringFromTable(string_table, file_entry.name_off);
        if (filename.empty()) {
            continue;
        }

        std::string clean_path = filename;
        size_t end = clean_path.find('\0');
        if (end != std::string::npos) clean_path = clean_path.substr(0, end);
        std::replace(clean_path.begin(), clean_path.end(), '\\', '/');
        while (!clean_path.empty() && clean_path.back() == '/') clean_path.pop_back();
        while (!clean_path.empty() && clean_path.front() == '/') clean_path.erase(clean_path.begin());
        if (clean_path.empty()) {
            continue;
        }
        
        SSRAFileInfo s_info;
        s_info.chunk_index = file_entry.chunk_idx;
        s_info.offset = file_entry.chunk_file_off;
        s_info.size = file_entry.uncomp_sz;
        s_info.compressed_size = file_entry.comp_sz;
        s_info.is_compressed = (file_entry.is_compressed != 0);
        s_info.is_encrypted = (file_entry.is_encrypted != 0);

        file_map[clean_path] = s_info;
        file_map[filename] = s_info;
        AddFileToTree(clean_path, file_entry.chunk_file_off, file_entry.uncomp_sz, 0);
        
        if (i % 100 == 0 || i == header.file_count - 1) {
            progress = static_cast<float>(i + 1) / header.file_count;
        }
    }
    progress = 1.0f;
}

std::vector<uint8_t> SSRArchive::GetFileData(const Core::FileNode& node)
{
    if (!std::holds_alternative<Core::FileInfo>(node.data)) {
        return {};
    }

    auto it = file_map.find(node.full_path);
    if (it == file_map.end()) {
        std::string alt_path = node.full_path;
        while (!alt_path.empty() && alt_path.front() == '/') alt_path.erase(alt_path.begin());
        it = file_map.find(alt_path);
    }
    if (it == file_map.end()) {
        it = file_map.find("/" + node.full_path);
    }
    if (it == file_map.end()) {
        LogError("File not found in SSRA map: " + node.full_path);
        return {};
    }

    const SSRAFileInfo& s_info_orig = it->second;
    uint16_t group_idx = s_info_orig.chunk_index; // actually group_idx
    uint64_t global_off = s_info_orig.offset;
    
    const ChunkInfo* target_chunk = nullptr;
    for (const auto& c : chunks) {
        if (c.group_idx == group_idx) {
            if (global_off >= c.global_offset && global_off < c.global_offset + c.compressed_size) {
                target_chunk = &c;
                break;
            }
        }
    }
    
    if (!target_chunk) {
        LogError("Failed to locate chunk for global offset " + std::to_string(global_off) + " in group " + std::to_string(group_idx));
        return {};
    }

    const ChunkInfo& c_info = *target_chunk;
    SSRAFileInfo s_info = s_info_orig;
    s_info.offset = global_off - c_info.global_offset; // local offset

    std::filesystem::path manifest_p(manifest_path);
    std::filesystem::path manifest_dir = manifest_p.parent_path();

    std::vector<std::filesystem::path> search_dirs = {
        manifest_dir / L"chunks",
        manifest_dir,
        manifest_dir.parent_path() / L"chunks",
        manifest_dir.parent_path(),
        manifest_dir.parent_path().parent_path() / L"chunks",
        manifest_dir.parent_path().parent_path(),
        std::filesystem::path(chunks_dir)
    };

    std::vector<std::string> candidate_names;
    candidate_names.push_back(c_info.name);

    char buf[64];
    auto git = group_names.find(c_info.group_idx);
    if (git != group_names.end() && !git->second.empty()) {
        snprintf(buf, sizeof(buf), "%s_%04u.ssrc", git->second.c_str(), c_info.chunk_id);
        candidate_names.push_back(buf);
        snprintf(buf, sizeof(buf), "%s_%u.ssrc", git->second.c_str(), c_info.chunk_id);
        candidate_names.push_back(buf);
        snprintf(buf, sizeof(buf), "%s%04u.ssrc", git->second.c_str(), c_info.chunk_id);
        candidate_names.push_back(buf);
    }
    snprintf(buf, sizeof(buf), "group_%u_%04u.ssrc", c_info.group_idx, c_info.chunk_id);
    candidate_names.push_back(buf);
    snprintf(buf, sizeof(buf), "chunk_%04u.ssrc", c_info.chunk_id);
    candidate_names.push_back(buf);
    snprintf(buf, sizeof(buf), "chunk_%u.ssrc", c_info.chunk_id);
    candidate_names.push_back(buf);
    snprintf(buf, sizeof(buf), "hotfix_%04u.ssrc", c_info.chunk_id);
    candidate_names.push_back(buf);
    snprintf(buf, sizeof(buf), "chunk_%04u.ssrc", (unsigned int)c_info.index);
    candidate_names.push_back(buf);
    snprintf(buf, sizeof(buf), "chunk_%u.ssrc", (unsigned int)c_info.index);
    candidate_names.push_back(buf);

    std::filesystem::path chunk_path;
    bool found = false;
    for (const auto& dir : search_dirs) {
        if (!std::filesystem::exists(dir)) continue;
        for (const auto& name : candidate_names) {
            std::filesystem::path test_path = dir / Core::Utf8ToWString(name);
            if (std::filesystem::exists(test_path)) {
                chunk_path = test_path;
                found = true;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        // Fallback: search directory for any matching .ssrc using exact file size and chunk_id
        for (const auto& dir : search_dirs) {
            if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) continue;
            try {
                for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == L".ssrc") {
                        std::error_code ec;
                        uint64_t file_size = std::filesystem::file_size(entry.path(), ec);
                        if (!ec && file_size == c_info.compressed_size) {
                            std::string stem = Core::PathToUtf8(entry.path().stem());
                            
                            // Verify that the filename contains the chunk_id as a standalone number
                            uint32_t current_num = 0;
                            bool in_num = false;
                            bool match_found = false;
                            
                            for (char c : stem) {
                                if (c >= '0' && c <= '9') {
                                    current_num = current_num * 10 + (c - '0');
                                    in_num = true;
                                } else {
                                    if (in_num && current_num == c_info.chunk_id) {
                                        match_found = true;
                                        break;
                                    }
                                    in_num = false;
                                    current_num = 0;
                                }
                            }
                            if (in_num && current_num == c_info.chunk_id) {
                                match_found = true;
                            }
                            
                            if (match_found) {
                                chunk_path = entry.path();
                                found = true;
                                break;
                            }
                        }
                    }
                }
            } catch (...) {}
            if (found) break;
        }
    }

    if (!found) {
        LogError("Failed to locate chunk file for index " + std::to_string(group_idx) + " (" + c_info.name + ") for file: " + node.full_path);
        return {};
    }

    std::ifstream file(chunk_path, std::ios::binary);
    if (!file.is_open()) {
        LogError("Failed to open chunk file: " + Core::PathToUtf8(chunk_path));
        return {};
    }

    file.seekg(0, std::ios::end);
    std::streamsize chunk_file_size = file.tellg();
    file.seekg(s_info.offset, std::ios::beg);
    
    size_t read_size = s_info.is_compressed ? s_info.compressed_size : s_info.size;
    std::vector<uint8_t> buffer(read_size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), read_size)) {
        LogError("Failed to read data from chunk " + c_info.name + " for file: " + node.full_path + 
                 " (offset=" + std::to_string(s_info.offset) + ", read_size=" + std::to_string(read_size) + 
                 ", chunk_file_size=" + std::to_string(chunk_file_size) + ")");
        return {};
    }

    if (s_info.is_encrypted) {
        LogError("Encrypted files are not yet supported for: " + node.full_path);
        return buffer;
    }

    if (s_info.is_compressed) {
        std::vector<uint8_t> decompressed(s_info.size);
        size_t dSize = ZSTD_decompress(decompressed.data(), decompressed.size(), buffer.data(), buffer.size());
        if (ZSTD_isError(dSize)) {
            LogError("ZSTD decompression failed for " + node.full_path + ": " + ZSTD_getErrorName(dSize));
            return buffer;
        }
        decompressed.resize(dSize);
        return decompressed;
    }

    return buffer;
}
