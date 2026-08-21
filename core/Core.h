#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <array>
#include <variant>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Core {
    inline std::string WStringToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return "";
#ifdef _WIN32
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (size_needed <= 0) return "";
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
        return str;
#else
        return std::string(wstr.begin(), wstr.end());
#endif
    }

    inline std::wstring Utf8ToWString(const std::string& str) {
        if (str.empty()) return L"";
#ifdef _WIN32
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
        if (size_needed <= 0) return L"";
        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size_needed);
        return wstr;
#else
        return std::wstring(str.begin(), str.end());
#endif
    }

    inline std::string PathToUtf8(const std::filesystem::path& p) {
        return WStringToUtf8(p.wstring());
    }
    
    static constexpr uint32_t INITIAL = 0x24D1C;
    static constexpr uint32_t MULT = 0x41C64E6D;
    static constexpr size_t KEY_SIZE = 0x81;

    
    struct FileNode;

    struct FileInfo {
        uint64_t offset;
        uint64_t size;
        std::string format;
        uint32_t archive_id = 0;
    };

    struct FolderInfo {
        std::vector<FileNode> children;
    };

    struct FileNode {
        std::string name;
        std::string full_path;
        std::variant<FileInfo, FolderInfo> data;
    };

    inline void xor_buffer(uint8_t* buffer, size_t size, size_t file_offset) {
        // Generate key buffer
        std::array<uint8_t, KEY_SIZE> key;
        uint32_t current = INITIAL;
        for (size_t i = 0; i < KEY_SIZE; ++i) {
            current = (current * MULT) & 0x7FFFFFFF;
            key[i] = (current >> 16) & 0xFF;
        }

        // XOR buffer with cyclic key
        for (size_t i = 0; i < size; ++i) {
            buffer[i] ^= key[(file_offset + i) % KEY_SIZE];
        }
    }
}