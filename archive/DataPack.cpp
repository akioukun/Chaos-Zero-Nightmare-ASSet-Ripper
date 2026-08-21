#include "DataPack.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include "core/Logger.h"

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

bool DataPack::EnsureWindow(PackPart &part, uint64_t offset, size_t needed) const
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

    uint64_t aligned_offset = (offset / alloc_granularity) * alloc_granularity;
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

    parts.push_back(part);
    total_file_size += part.fileSize;
    return true;
}

const uint8_t *DataPack::GetDataAtOffset(uint64_t offset, size_t &outSize)
{
    if (parts.empty())
        return nullptr;

    // Find which part contains this offset
    uint64_t currentPos = 0;
    for (auto &part : parts)
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

DataPack::DataPack(const std::wstring &path) 
{
    this->pack_path = path;
    this->type = PackType::Unknown;
    root_node.name = "root";
    root_node.data = Core::FolderInfo{};
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    alloc_granularity = sysInfo.dwAllocationGranularity;

    std::filesystem::path fs_path(path);
    if (std::filesystem::is_directory(fs_path))
    {
        type = PackType::LocalDirectory;
        total_file_size = 0;
        return;
    }

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

    if (parts.empty())
    {
        LogError("Failed to load any pack parts from: " + std::filesystem::path(path).u8string());
        return;
    }

    if (total_file_size < 5)
    {
        type = PackType::Unknown;
        return;
    }

    // check file type from first part
    uint8_t magic[5];
    if (ReadBytes(0, magic, 5) == 5)
    {
        if (memcmp(magic, "\x71\x40\xBD\x73\x93", 5) == 0)
            type = PackType::Encrypted;
        else if (memcmp(magic, "\x50\x4C\x50\x63\x4B", 5) == 0)
            type = PackType::Decrypted;
        else
            type = PackType::Unknown;
    }
}

DataPack::~DataPack()
{
    for (auto &part : parts)
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
    parts.clear();
}

std::vector<uint8_t> DataPack::GetFileData(const Core::FileNode &node)
{
    std::vector<uint8_t> data;

    if (!std::holds_alternative<Core::FileInfo>(node.data))
        return data;

    const auto &info = std::get<Core::FileInfo>(node.data);

    if (type == PackType::LocalDirectory)
    {
        std::filesystem::path full_path = std::filesystem::path(pack_path) / node.full_path;
        try
        {
            std::ifstream file(full_path, std::ios::binary);
            if (file)
            {
                data.resize(info.size);
                file.read(reinterpret_cast<char*>(data.data()), info.size);
            }
            else
            {
                LogError("Could not open local file: " + full_path.u8string());
            }
        }
        catch (const std::exception &e)
        {
            LogError("Error reading local directory file data: " + std::string(e.what()));
            data.clear();
        }
        return data;
    }

    uint64_t file_end = static_cast<uint64_t>(info.offset) + static_cast<uint64_t>(info.size);
    if (static_cast<uint64_t>(info.offset) >= total_file_size ||
        file_end > total_file_size)
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

        if (type == PackType::Encrypted)
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

void DataPack::Scan(std::atomic<float> &progress)
{
    auto &root_folder = std::get<Core::FolderInfo>(root_node.data);
    root_folder.children.clear();

    try
    {
        if (type == PackType::Encrypted)
            ScanEncrypted(progress);
        else if (type == PackType::Decrypted)
            ScanDecrypted(progress);
        else if (type == PackType::LocalDirectory)
            ScanLocalDirectory(progress);

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

        process_node(root_node);
    }
    catch (const std::exception &e)
    {
        LogError("Error during scan: " + std::filesystem::path(e.what()).u8string());
    }

    SortTree();
    progress = 1.0f;
}

void DataPack::ScanLocalDirectory(std::atomic<float>& progress)
{
    std::filesystem::path base_path(pack_path);
    uint32_t count = 0;
    uint64_t total = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(base_path, std::filesystem::directory_options::skip_permission_denied))
    {
        if (entry.is_regular_file())
        {
            std::filesystem::path rel_path = std::filesystem::relative(entry.path(), base_path);
            std::string rel_path_str = rel_path.u8string();
            std::replace(rel_path_str.begin(), rel_path_str.end(), '\\', '/');

            uint64_t size = entry.file_size();
            AddFileToTree(rel_path_str, 0, size);
            count++;
            total += size;
        }
    }

    parsed_file_count = count;
    parsed_total_size = total;
    progress = 1.0f;
}

void DataPack::ScanEncrypted(std::atomic<float> &progress)
{
    std::array<uint8_t, Core::KEY_SIZE> key{};
    uint32_t current = Core::INITIAL;
    for (size_t i = 0; i < Core::KEY_SIZE; ++i)
    {
        current = (current * Core::MULT) & 0x7FFFFFFF;
        key[i] = (current >> 16) & 0xFF;
    }

    uint64_t cursor = 4; // entries can't start before offset 4

    while (cursor < total_file_size)
    {
        if ((cursor & 0xFFFFF) == 0)
        {
            progress = (float)cursor / total_file_size;
        }

        size_t available = 0;
        const uint8_t *block = GetDataAtOffset(cursor, available);
        if (!block || available == 0)
        {
            cursor++;
            continue;
        }

        size_t candidate_pos = available;
        for (size_t pos = 0; pos < available && (cursor + pos) < total_file_size; ++pos)
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

        if (header_offset + 15 > total_file_size)
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

        if (container_len > total_file_size ||
            path_len == 0 ||
            path_len > 255 ||
            data_len > total_file_size ||
            container_len != path_len + data_len + 19)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (header_offset + 15 + path_len + data_len > total_file_size)
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

    while (cursor < total_file_size)
    {
        if ((cursor & 0xFFFFF) == 0)
        {
            progress = (float)cursor / total_file_size;
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

        if (header_offset + 15 > total_file_size)
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

        if (container_len > total_file_size || path_len == 0 || path_len > 255 || data_len > total_file_size)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (container_len != path_len + data_len + 19)
        {
            cursor = abs_pos + 1;
            continue;
        }

        if (header_offset + 15 + path_len > total_file_size)
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

        if (file_offset + data_len <= total_file_size)
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

