#include "SSRArchive.h"
#include "core/Logger.h"
#include "core/Core.h"
#include "libs/zstd/zstd.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace {
    constexpr uint8_t kZstdMagic = 0x28;
    constexpr uint8_t kLegacyZstdMagicFirst = 0x25;
    constexpr uint8_t kLegacyZstdMagicLast = 0x27;

    bool NormalizeLegacyZstdFrame(std::vector<uint8_t>& buffer)
    {
        if (buffer.size() < 4 || buffer[1] != 0xB5 || buffer[2] != 0x2F || buffer[3] != 0xFD) {
            return false;
        }

        if (buffer[0] < kLegacyZstdMagicFirst || buffer[0] > kLegacyZstdMagicLast) {
            return false;
        }
        buffer[0] = kZstdMagic;
        return true;
    }
}

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
            current_offset += c->size_bytes;
        }
    }

    const std::filesystem::path chunks_path = std::filesystem::path(chunks_dir);
    std::vector<std::filesystem::path> physical_files;
    std::map<uint64_t, std::vector<std::filesystem::path>> physical_files_by_size;
    if (std::filesystem::is_directory(chunks_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(chunks_path)) {
            if (entry.is_regular_file() && entry.path().extension() == L".ssrc") {
                physical_files.push_back(entry.path());
                physical_files_by_size[std::filesystem::file_size(entry.path())].push_back(entry.path());
            }
        }
    }

    for (auto& chunk : chunks) {
        std::vector<std::string> names = {chunk.name};
        auto group_name = group_names.find(chunk.group_idx);
        if (group_name != group_names.end() && !group_name->second.empty()) {
            char name[64];
            snprintf(name, sizeof(name), "%s_b%02u_0.ssrc", group_name->second.c_str(), chunk.chunk_id);
            names.emplace_back(name);
        }

        for (const auto& physical : physical_files) {
            const std::string physical_name = Core::PathToUtf8(physical.filename());
            const uint64_t physical_size = std::filesystem::file_size(physical);
            if (physical_size != chunk.compressed_size) {
                continue;
            }
            bool name_match = false;
            for (const auto& name : names) {
                if (physical_name == name) {
                    name_match = true;
                    break;
                }
            }
            if (name_match) {
                chunk.physical_path = physical.wstring();
                break;
            }
        }

        if (chunk.physical_path.empty()) {
            const auto size_match = physical_files_by_size.find(chunk.compressed_size);
            if (size_match != physical_files_by_size.end() && size_match->second.size() == 1) {
                chunk.physical_path = size_match->second.front().wstring();
            }
        }

        if (chunk.physical_path.empty()) {
            LogInfo("Skipping unavailable SSRA chunk: " + chunk.name);
        }
    }

    size_t unavailable_chunk_count = 0;
    for (const auto& chunk : chunks) {
        if (chunk.physical_path.empty()) {
            ++unavailable_chunk_count;
        }
    }
    if (unavailable_chunk_count != 0) {
        LogInfo("SSRA unavailable chunks skipped: " + std::to_string(unavailable_chunk_count));
    }

    // Read files
    file_map.clear();
    for (uint32_t i = 0; i < header.file_count; ++i) {
        uint64_t entry_off = header.file_table_offset + (i * sizeof(SSRAFileEntry));
        if (entry_off + sizeof(SSRAFileEntry) > data.size()) break;

        SSRAFileEntry file_entry;
        std::memcpy(&file_entry, data.data() + entry_off, sizeof(SSRAFileEntry));

        bool chunk_available = false;
        for (const auto& chunk : chunks) {
            if (chunk.group_idx == file_entry.chunk_idx &&
                file_entry.chunk_file_off >= chunk.global_offset &&
                file_entry.chunk_file_off < chunk.global_offset + chunk.size_bytes &&
                file_entry.chunk_file_off + file_entry.comp_sz <= chunk.global_offset + chunk.size_bytes &&
                !chunk.physical_path.empty()) {
                chunk_available = true;
                break;
            }
        }
        if (!chunk_available) {
            continue;
        }

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
    SortTree();
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
            if (global_off >= c.global_offset && global_off < c.global_offset + c.size_bytes) {
                target_chunk = &c;
                break;
            }
        }
    }
    
    if (!target_chunk) {
        LogError("Failed to locate chunk for global offset " + std::to_string(global_off) + " in group " + std::to_string(group_idx));
        return {};
    }

    SSRAFileInfo s_info = s_info_orig;

    size_t read_size = s_info.is_compressed ? s_info.compressed_size : s_info.size;
    std::vector<uint8_t> buffer(read_size);
    std::vector<const ChunkInfo*> group_chunks;
    for (const auto& chunk : chunks) {
        if (chunk.group_idx == group_idx) {
            group_chunks.push_back(&chunk);
        }
    }
    std::sort(group_chunks.begin(), group_chunks.end(), [](const ChunkInfo* left, const ChunkInfo* right) {
        return left->global_offset < right->global_offset;
    });

    uint64_t virtual_offset = global_off;
    size_t bytes_remaining = read_size;
    size_t buffer_offset = 0;
    while (bytes_remaining != 0) {
        const ChunkInfo* current_chunk = nullptr;
        for (const ChunkInfo* chunk : group_chunks) {
            if (virtual_offset >= chunk->global_offset && virtual_offset < chunk->global_offset + chunk->size_bytes) {
                current_chunk = chunk;
                break;
            }
        }
        if (current_chunk == nullptr) {
            LogError("Failed to locate chunk for virtual offset " + std::to_string(virtual_offset) + " in file: " + node.full_path);
            return {};
        }

        const uint64_t local_offset = virtual_offset - current_chunk->global_offset;
        const size_t available = static_cast<size_t>(current_chunk->size_bytes - local_offset);
        const size_t segment_size = (std::min)(bytes_remaining, available);
        if (current_chunk->physical_path.empty()) {
            LogError("Failed to locate chunk file for " + current_chunk->name + " and file: " + node.full_path);
            return {};
        }

        std::ifstream file(current_chunk->physical_path, std::ios::binary);
        if (!file.is_open()) {
            LogError("Failed to open chunk file: " + Core::WStringToUtf8(current_chunk->physical_path));
            return {};
        }
        file.seekg(static_cast<std::streamoff>(local_offset), std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buffer.data() + buffer_offset), static_cast<std::streamsize>(segment_size))) {
            LogError("Failed to read data from chunk " + current_chunk->name + " for file: " + node.full_path);
            return {};
        }
        virtual_offset += segment_size;
        buffer_offset += segment_size;
        bytes_remaining -= segment_size;
    }

    if (s_info.is_encrypted) {
        LogError("Encrypted files are not yet supported for: " + node.full_path);
        return buffer;
    }

    if (s_info.is_compressed) {
        const bool legacy_frame = NormalizeLegacyZstdFrame(buffer);
        std::vector<uint8_t> decompressed(s_info.size);
        size_t dSize = ZSTD_decompress(decompressed.data(), decompressed.size(), buffer.data(), buffer.size());
        if (ZSTD_isError(dSize)) {
            LogError("ZSTD decompression failed for " + node.full_path +
                     (legacy_frame ? " (legacy frame normalized): " : ": ") + ZSTD_getErrorName(dSize));
            return buffer;
        }
        decompressed.resize(dSize);
        return decompressed;
    }

    return buffer;
}
