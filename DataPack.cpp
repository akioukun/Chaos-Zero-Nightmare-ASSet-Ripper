#include "DataPack.h"
#include "SCTParser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include "DBParser.h"
#include "SCSPParser.h"
#include "Logger.h"

namespace
{
    uint32_t read_u32_le(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    std::string sanitize_pack_path(const std::string &raw)
    {
        // Trim trailing NUL bytes and normalize separators for stable tree building.
        size_t end = raw.find('\0');
        std::string path = (end == std::string::npos) ? raw : raw.substr(0, end);
        std::replace(path.begin(), path.end(), '\\', '/');
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
        return path;
    }

    bool is_likely_pack_path(const std::string &path)
    {
        if (path.size() < 3 || path.size() > 2048)
            return false;
        if (path.find('\0') != std::string::npos)
            return false;
        bool has_file_like_suffix = false;
        for (unsigned char c : path)
        {
            if (c < 0x20 && c != '\t')
                return false;
            if (c == '.' || c == '/' || c == '\\')
                has_file_like_suffix = true;
        }
        return has_file_like_suffix;
    }

    std::vector<std::string> split_path_parts(const std::string &path)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start < path.size())
        {
            size_t slash = path.find('/', start);
            size_t end = (slash == std::string::npos) ? path.size() : slash;
            if (end > start)
            {
                parts.push_back(path.substr(start, end - start));
            }
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        return parts;
    }
}

std::vector<std::wstring> DataPack::FindPackParts(const std::wstring &basePath)
{
    std::vector<std::wstring> parts;
    parts.push_back(basePath);

    // find all the parts
    for (int i = 1; i < 1000; ++i)
    {
        std::wstring partPath = basePath + L"~" + std::to_wstring(i);
        HANDLE hTest = CreateFileW(partPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hTest == INVALID_HANDLE_VALUE)
        {
            break;
        }
        CloseHandle(hTest);
        parts.push_back(partPath);
    }

    return parts;
}

bool DataPack::EnsureWindow(PackPart &part, uint64_t offset, size_t needed)
{
    // check if the window already covers the offset
    if (part.view.data &&
        offset >= part.view.offset &&
        (offset + needed) <= (part.view.offset + part.view.size))
    {
        return true;
    }

    // unmap previous view
    if (part.view.data)
    {
        UnmapViewOfFile(part.view.data);
        part.view.data = nullptr;
        part.view.size = 0;
    }

    uint64_t aligned_offset = (offset / alloc_granularity_) * alloc_granularity_;
    uint64_t adjustment = offset - aligned_offset;

    size_t window_size = WINDOW_SIZE;
    if (window_size < needed + static_cast<size_t>(adjustment))
    {
        window_size = needed + static_cast<size_t>(adjustment);
    }
    if (aligned_offset + window_size > part.fileSize)
    {
        window_size = static_cast<size_t>(part.fileSize - aligned_offset);
    }
    if (window_size == 0)
        return false;

    DWORD offset_high = static_cast<DWORD>(aligned_offset >> 32);
    DWORD offset_low = static_cast<DWORD>(aligned_offset & 0xFFFFFFFF);

    const uint8_t *mapped = (const uint8_t *)MapViewOfFile(
        part.hMapFile, FILE_MAP_READ, offset_high, offset_low, window_size);

    if (!mapped)
    {
        DWORD err = GetLastError();
        LogError("MapViewOfFile failed at offset " + std::to_string(aligned_offset) + " size " + std::to_string(window_size) + " error " + std::to_string(err));
        return false;
    }

    part.view.data = mapped;
    part.view.offset = aligned_offset;
    part.view.size = window_size;
    return true;
}

bool DataPack::LoadPackPart(const std::wstring &path, size_t partIndex)
{
    PackPart part;

    part.hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (part.hFile == INVALID_HANDLE_VALUE)
    {
        LogError("Failed to open file: " + std::filesystem::path(path).u8string());
        return false;
    }

    LARGE_INTEGER fs;
    if (!GetFileSizeEx(part.hFile, &fs))
    {
        LogError("Failed to get file size for: " + std::filesystem::path(path).u8string());
        CloseHandle(part.hFile);
        return false;
    }
    part.fileSize = fs.QuadPart;

    if (part.fileSize == 0)
    {
        LogError("Empty file, skipping: " + std::filesystem::path(path).u8string());
        CloseHandle(part.hFile);
        return false;
    }

    part.hMapFile = CreateFileMapping(part.hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (part.hMapFile == NULL)
    {
        LogError("Failed to create file mapping for: " + std::filesystem::path(path).u8string());
        CloseHandle(part.hFile);
        return false;
    }

    parts_.push_back(part);
    total_file_size_ += part.fileSize;
    return true;
}

const uint8_t *DataPack::GetDataAtOffset(uint64_t offset, size_t &outSize)
{
    if (parts_.empty())
        return nullptr;

    // Find which part contains this offset
    uint64_t currentPos = 0;
    for (auto &part : parts_)
    {
        if (offset < currentPos + part.fileSize)
        {
            uint64_t localOffset = offset - currentPos;
            size_t remaining = static_cast<size_t>(part.fileSize - localOffset);

            // Request at least 1 byte; EnsureWindow will map up to WINDOW_SIZE
            if (!EnsureWindow(part, localOffset, 1))
            {
                outSize = 0;
                return nullptr;
            }

            uint64_t offsetInView = localOffset - part.view.offset;
            size_t availableInView = part.view.size - static_cast<size_t>(offsetInView);
            outSize = (availableInView < remaining) ? availableInView : remaining;
            return part.view.data + offsetInView;
        }
        currentPos += part.fileSize;
    }

    return nullptr;
}

size_t DataPack::ReadBytes(uint64_t offset, void *dest, size_t count)
{
    size_t total_read = 0;
    uint8_t *dst = static_cast<uint8_t *>(dest);

    while (total_read < count)
    {
        size_t available = 0;
        const uint8_t *src = GetDataAtOffset(offset, available);
        if (!src || available == 0)
            break;

        size_t to_copy = count - total_read;
        if (to_copy > available)
            to_copy = available;

        memcpy(dst + total_read, src, to_copy);
        total_read += to_copy;
        offset += to_copy;
    }

    return total_read;
}

DataPack::DataPack(const std::wstring &path) : pack_path_(path), type_(PackType::Unknown)
{
    root_node_.name = "root";
    root_node_.data = Core::FolderInfo{};

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    alloc_granularity_ = si.dwAllocationGranularity;

    auto packParts = FindPackParts(path);
    if (packParts.empty())
    {
        LogError("No pack files found: " + std::filesystem::path(path).u8string());
        return;
    }

    // load all parts
    for (size_t i = 0; i < packParts.size(); ++i)
    {
        if (!LoadPackPart(packParts[i], i))
        {
            LogError("Failed to load pack part: " + std::filesystem::path(packParts[i]).u8string());
            // continue
        }
    }

    if (parts_.empty())
    {
        LogError("Failed to load any pack parts from: " + std::filesystem::path(path).u8string());
        return;
    }

    if (total_file_size_ < 5)
    {
        type_ = PackType::Unknown;
        return;
    }

    // check file type from first part
    uint8_t magic[5];
    if (ReadBytes(0, magic, 5) == 5)
    {
        if (memcmp(magic, "\x71\x40\xBD\x73\x93", 5) == 0)
            type_ = PackType::Encrypted;
        else if (memcmp(magic, "\x50\x4C\x50\x63\x4B", 5) == 0)
            type_ = PackType::Decrypted;
        else
            type_ = PackType::Unknown;
    }
}

DataPack::~DataPack()
{
    for (auto &part : parts_)
    {
        if (part.view.data)
        {
            UnmapViewOfFile(part.view.data);
            part.view.data = nullptr;
        }
        if (part.hMapFile)
            CloseHandle(part.hMapFile);
        if (part.hFile != INVALID_HANDLE_VALUE)
            CloseHandle(part.hFile);
    }
    parts_.clear();
}

std::vector<uint8_t> DataPack::GetFileData(const Core::FileNode &node)
{
    std::vector<uint8_t> data;

    if (!std::holds_alternative<Core::FileInfo>(node.data))
        return data;

    const auto &info = std::get<Core::FileInfo>(node.data);

    uint64_t file_end = static_cast<uint64_t>(info.offset) + static_cast<uint64_t>(info.size);
    if (static_cast<uint64_t>(info.offset) >= total_file_size_ ||
        file_end > total_file_size_)
    {
        LogError("Invalid file offset/size for: " + std::filesystem::path(node.name).u8string());
        return data;
    }

    try
    {
        data.resize(info.size);

        size_t bytes_read = ReadBytes(info.offset, data.data(), info.size);
        if (bytes_read != info.size)
        {
            LogError("Failed to read full file data for: " + std::filesystem::path(node.name).u8string() + " (read " + std::to_string(bytes_read) + " of " + std::to_string(info.size) + ")");
            data.clear();
            return data;
        }

        if (type_ == PackType::Encrypted)
        {
            Core::xor_buffer(data.data(), info.size, info.offset);
        }
    }
    catch (const std::exception &e)
    {
        LogError("Error reading file data: " + std::string(e.what()));
        data.clear();
    }

    return data;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void DataPack::Scan(std::atomic<float> &progress)
{
    auto &root_folder = std::get<Core::FolderInfo>(root_node_.data);
    root_folder.children.clear();

    try
    {
        if (type_ == PackType::Encrypted)
            ScanEncrypted(progress);
        else if (type_ == PackType::Decrypted)
            ScanDecrypted(progress);

        std::function<void(Core::FileNode &)> process_node = [&](Core::FileNode &node)
        {
            try
            {
                if (std::holds_alternative<Core::FileInfo>(node.data))
                {
                    const auto &info = std::get<Core::FileInfo>(node.data);
                }
                else if (std::holds_alternative<Core::FolderInfo>(node.data))
                {
                    auto &folder = std::get<Core::FolderInfo>(node.data);
                    for (auto &child : folder.children)
                    {
                        process_node(child);
                    }
                }
            }
            catch (const std::exception &e)
            {
                LogError("Error processing node: " + std::filesystem::path(node.name).u8string() + " - " + std::string(e.what()));
            }
        };

        process_node(root_node_);
    }
    catch (const std::exception &e)
    {
        LogError("Error during scan: " + std::filesystem::path(e.what()).u8string());
    }

    progress = 1.0f;
}

void DataPack::ScanEncrypted(std::atomic<float> &progress)
{
    std::array<uint8_t, Core::KEY_SIZE> key;
    uint32_t current = Core::INITIAL;
    for (size_t i = 0; i < Core::KEY_SIZE; ++i)
    {
        current = (current * Core::MULT) & 0x7FFFFFFF;
        key[i] = (current >> 16) & 0xFF;
    }

    uint64_t cursor = 4; // entries can't start before offset 4

    while (cursor < total_file_size_)
    {
        if ((cursor & 0xFFFFF) == 0)
        {
            progress = (float)cursor / total_file_size_;
        }

        size_t available = 0;
        const uint8_t *block = GetDataAtOffset(cursor, available);
        if (!block || available == 0)
        {
            cursor++;
            continue;
        }

        size_t candidate_pos = available;
        for (size_t pos = 0; pos < available && (cursor + pos) < total_file_size_; ++pos)
        {
            uint64_t abs_pos = cursor + pos;
            uint8_t decrypted_byte = block[pos] ^ key[abs_pos % Core::KEY_SIZE];
            if (decrypted_byte == 0x02)
            {
                candidate_pos = pos;
                break;
            }
        }

        // no candidate found in this block skip the whole block
        if (candidate_pos >= available)
        {
            cursor += available;
            continue;
        }

        uint64_t abs_pos = cursor + candidate_pos;
        uint64_t header_offset = abs_pos - 4;

        if (header_offset + 15 > total_file_size_)
        {
            cursor = abs_pos + 1;
            continue;
        }

        uint8_t header_buffer[15];
        if (ReadBytes(header_offset, header_buffer, 15) != 15)
        {
            cursor = abs_pos + 1;
            continue;
        }

        Core::xor_buffer(header_buffer, 15, header_offset);

        uint32_t container_len = read_u32_le(&header_buffer[0]);
        uint8_t path_len = header_buffer[5];
        uint32_t data_len = read_u32_le(&header_buffer[6]);

        if (container_len > total_file_size_ ||
            path_len == 0 ||
            path_len > 255 ||
            data_len > total_file_size_ ||
            container_len != path_len + data_len + 19)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (header_offset + 15 + path_len + data_len > total_file_size_)
        {
            cursor = abs_pos + 1;
            continue;
        }

        std::vector<uint8_t> path_buffer(path_len);
        if (ReadBytes(header_offset + 15, path_buffer.data(), path_len) != path_len)
        {
            cursor = abs_pos + 1;
            continue;
        }
        Core::xor_buffer(path_buffer.data(), path_len, header_offset + 15);
        std::string path_str = sanitize_pack_path(std::string((char *)path_buffer.data(), path_len));
        if (!is_likely_pack_path(path_str))
        {
            cursor = abs_pos + 1;
            continue;
        }

        uint64_t file_offset = header_offset + 15 + path_len;
        AddFileToTree(path_str, file_offset, static_cast<uint64_t>(data_len));

        cursor = header_offset + 4 + container_len;
    }
}

void DataPack::ScanDecrypted(std::atomic<float> &progress)
{
    uint64_t cursor = 0;

    while (cursor < total_file_size_)
    {
        if ((cursor & 0xFFFFF) == 0)
        {
            progress = (float)cursor / total_file_size_;
        }

        size_t available = 0;
        const uint8_t *block = GetDataAtOffset(cursor, available);
        if (!block || available == 0)
        {
            cursor++;
            continue;
        }

        const uint8_t *found = (const uint8_t *)memchr(block, 0x02, available);
        if (!found)
        {
            //no 0x02 in this block skip it entirely
            cursor += available;
            continue;
        }

        size_t candidate_pos = static_cast<size_t>(found - block);
        uint64_t abs_pos = cursor + candidate_pos;

        if (abs_pos < 4)
        {
            cursor = abs_pos + 1;
            continue;
        }

        uint64_t header_offset = abs_pos - 4;

        if (header_offset + 15 > total_file_size_)
        {
            cursor = abs_pos + 1;
            continue;
        }

        uint8_t header_buffer[15];
        if (ReadBytes(header_offset, header_buffer, 15) != 15)
        {
            cursor = abs_pos + 1;
            continue;
        }

        uint32_t container_len = read_u32_le(&header_buffer[0]);
        uint8_t path_len = header_buffer[5];
        uint32_t data_len = read_u32_le(&header_buffer[6]);

        if (container_len > total_file_size_ || path_len == 0 || path_len > 255 || data_len > total_file_size_)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (container_len != path_len + data_len + 19)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (header_offset + 15 + path_len > total_file_size_)
        {
            cursor = abs_pos + 1;
            continue;
        }

        std::vector<uint8_t> path_buffer(path_len);
        if (ReadBytes(header_offset + 15, path_buffer.data(), path_len) != path_len)
        {
            cursor = abs_pos + 1;
            continue;
        }

        std::string path_str = sanitize_pack_path(std::string((char *)path_buffer.data(), path_len));
        if (!is_likely_pack_path(path_str))
        {
            cursor = abs_pos + 1;
            continue;
        }

        uint64_t file_offset = header_offset + 15 + path_len;

        if (file_offset + data_len <= total_file_size_)
        {
            AddFileToTree(path_str, file_offset, static_cast<uint64_t>(data_len));
            cursor = file_offset + data_len;
        }
        else
        {
            cursor = abs_pos + 1;
        }
    }
}

void DataPack::AddFileToTree(const std::string &path, uint64_t offset, uint64_t size)
{
    try
    {
        std::string clean_path = sanitize_pack_path(path);
        if (clean_path.empty())
            return;

        std::vector<std::string> parts = split_path_parts(clean_path);
        if (parts.empty())
            return;

        auto *current_folder_info = &std::get<Core::FolderInfo>(root_node_.data);
        std::string current_path = "";

        for (size_t i = 0; i + 1 < parts.size(); ++i)
        {
            const std::string &part = parts[i];
            if (part.empty() || part == ".")
                continue;
            current_path += part + "/";

            auto it = std::find_if(current_folder_info->children.begin(), current_folder_info->children.end(), [&](const Core::FileNode &n) { return n.name == part; });

            if (it == current_folder_info->children.end())
            {
                Core::FileNode new_folder;
                new_folder.name = part;
                new_folder.full_path = current_path;
                new_folder.data = Core::FolderInfo{};
                current_folder_info->children.push_back(new_folder);
                current_folder_info = &std::get<Core::FolderInfo>(current_folder_info->children.back().data);
            }
            else
            {
                if (std::holds_alternative<Core::FolderInfo>(it->data))
                {
                    current_folder_info = &std::get<Core::FolderInfo>(it->data);
                }
            }
        }
        Core::FileNode file_node;
        file_node.name = parts.back();
        file_node.full_path = clean_path;

        std::string extension;
        size_t dot = file_node.name.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < file_node.name.size())
        {
            extension = file_node.name.substr(dot);
        }
        file_node.data = Core::FileInfo{offset, size, extension};
        current_folder_info->children.push_back(std::move(file_node));
    }
    catch (const std::exception &e)
    {
        LogError("Error adding file to tree: " + std::string(path) + " - " + std::string(e.what()));
    }
}

void DataPack::ExtractAll(const std::wstring &output_path, std::atomic<float> &progress, bool convert_sct_to_png, bool convert_db_to_json)
{
    LogInfo("ExtractAll started");
    Extract(root_node_, output_path, progress, convert_sct_to_png, convert_db_to_json);
    LogInfo("ExtractAll finished");
}

void DataPack::Extract(const Core::FileNode &node, const std::wstring &output_path, std::atomic<float> &progress, bool convert_sct_to_png, bool convert_db_to_json)
{
    uint64_t total_size_to_extract = 0;
    std::function<void(const Core::FileNode &)> F =
        [&](const Core::FileNode &n)
    {
        if (std::holds_alternative<Core::FileInfo>(n.data))
        {
            total_size_to_extract += std::get<Core::FileInfo>(n.data).size;
        }
        else if (std::holds_alternative<Core::FolderInfo>(n.data))
        {
            for (const auto &child : std::get<Core::FolderInfo>(n.data).children)
                F(child);
        }
    };
    F(node);
    if (total_size_to_extract == 0)
    {
        progress = 1.0f;
        return;
    }
    std::atomic<uint64_t> extracted_size = 0;
    LogInfo("Extract begin for node");
    ExtractNode(node, output_path, extracted_size, total_size_to_extract, progress, convert_sct_to_png, convert_db_to_json);
    progress = 1.0f;
    LogInfo("Extract end for node");
}

void DataPack::ExtractNode(const Core::FileNode &node, const std::wstring &current_path, std::atomic<uint64_t> &extracted_size, const uint64_t total_size, std::atomic<float> &progress, bool convert_sct_to_png, bool convert_db_to_json)
{

    try
    {
        if (std::holds_alternative<Core::FileInfo>(node.data))
        {
            const auto &info = std::get<Core::FileInfo>(node.data);
            std::filesystem::path final_path = std::filesystem::path(current_path) / node.name;
            LogInfo(std::string("Extracting file: ") + node.name + " size=" + std::to_string(info.size));

            std::string ext_lower = info.format;
            std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
            bool is_sct = (ext_lower == ".sct" || ext_lower == ".sct2");
            bool is_db = (ext_lower == ".db");
            bool is_scsp = (ext_lower == ".scsp");
            bool is_atlas = (ext_lower == ".atlas");

            if (is_sct && convert_sct_to_png)
            {
                final_path.replace_extension(".png");
            }

            if (is_db && convert_db_to_json)
            {
                final_path.replace_extension(".json");
            }

            if (is_scsp && convert_db_to_json)
            {
                final_path.replace_extension(".json");
            }

            std::filesystem::create_directories(final_path.parent_path());

            std::vector<uint8_t> buffer = GetFileData(node);

            if (!buffer.empty())
            {

                if (is_sct && convert_sct_to_png)
                {
                    try
                    {
                        LogInfo(std::string("Converting SCT to PNG: ") + node.name);
                        std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(buffer, false);
                        if (!png_data.empty())
                        {
                            buffer = std::move(png_data);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        LogError(std::string("SCT conversion failed for ") + node.name + ": " + e.what());
                    }
                }

                if (is_atlas && convert_sct_to_png)
                {
                    try
                    {
                        LogInfo(std::string("Rewriting atlas texture refs: ") + node.name);
                        std::string atlas_text(buffer.begin(), buffer.end());

                        size_t pos = 0;
                        while ((pos = atlas_text.find(".sct2", pos)) != std::string::npos)
                        {
                            atlas_text.replace(pos, 5, ".png");
                            pos += 4;
                        }

                        pos = 0;
                        while ((pos = atlas_text.find(".sct", pos)) != std::string::npos)
                        {
                            atlas_text.replace(pos, 4, ".png");
                            pos += 4;
                        }

                        buffer.assign(atlas_text.begin(), atlas_text.end());
                    }
                    catch (const std::exception &e)
                    {
                        LogError(std::string("Atlas rewrite failed for ") + node.name + ": " + e.what());
                    }
                }

                if (is_db && convert_db_to_json)
                {
                    try
                    {
                        LogInfo(std::string("Converting DB to JSON: ") + node.name);
                        std::ofstream out(final_path);
                        if (out.is_open())
                        {
                            bool ok = DBParser::ConvertToJsonToStream(buffer, out);
                            out.close();
                            if (!ok)
                            {
                                LogError(std::string("DB conversion returned false for ") + node.name);
                                std::ofstream out2(final_path);
                                out2 << "{}";
                                out2.close();
                            }
                        }
                        else
                        {
                            LogError(std::string("Failed to open output for DB JSON: ") + final_path.string());
                        }
                        buffer.clear();
                    }
                    catch (const std::exception &e)
                    {
                        LogError(std::string("DB conversion exception for ") + node.name + ": " + e.what());
                    }
                }

                if (is_scsp && convert_db_to_json)
                {
                    try
                    {
                        LogInfo(std::string("Converting SCSP to JSON: ") + node.name);
                        std::string json = SCSPParser::ConvertSCSPToJson(buffer);

                        std::ofstream out(final_path);
                        if (out.is_open())
                        {
                            out << json;
                            out.close();
                        }
                        else
                        {
                            LogError(std::string("Failed to open output for SCSP JSON: ") + final_path.string());
                        }
                        buffer.clear();
                    }
                    catch (const std::exception &e)
                    {
                        LogError(std::string("SCSP conversion exception for ") + node.name + ": " + e.what());
                    }
                }

                if (!buffer.empty())
                {
                    std::ofstream out(final_path, std::ios::binary);
                    if (out.is_open())
                    {
                        out.write((const char *)buffer.data(), buffer.size());
                        out.close();
                    }
                    else
                    {
                        LogError(std::string("Failed to open output for raw write: ") + final_path.string());
                    }
                }
            }
            else
            {
                LogError(std::string("Empty buffer for file: ") + node.name);
            }
            extracted_size += info.size;
            if (total_size > 0)
            {
                progress = (float)extracted_size / total_size;
                if ((extracted_size & 0x7FFFFF) == 0)
                {
                    LogInfo(std::string("Progress: ") + std::to_string((int)(progress * 100)) + "%");
                }
            }
        }
        else if (std::holds_alternative<Core::FolderInfo>(node.data))
        {
            const auto &info = std::get<Core::FolderInfo>(node.data);
            std::filesystem::path new_path = std::filesystem::path(current_path) / node.name;

            if (node.name != "root")
            {
                std::filesystem::create_directories(new_path);
            }
            else
            {
                new_path = current_path;
            }

            for (const auto &child : info.children)
            {
                ExtractNode(child, new_path, extracted_size, total_size, progress, convert_sct_to_png, convert_db_to_json);
            }
        }
    }
    catch (const std::exception &e)
    {
        LogError(std::string("Error extracting node: ") + node.name + " - " + e.what());
    }
}
