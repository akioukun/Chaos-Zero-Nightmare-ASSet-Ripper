#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Core {
    inline std::string WStringToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return "";
#ifdef _WIN32
        const int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        if (size_needed <= 0) return "";
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, nullptr, nullptr);
        return str;
#else
        return std::string(wstr.begin(), wstr.end());
#endif
    }

    inline std::wstring Utf8ToWString(const std::string& str) {
        if (str.empty()) return L"";
#ifdef _WIN32
        const int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
        if (size_needed <= 0) return L"";
        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstr[0], size_needed);
        return wstr;
#else
        return std::wstring(str.begin(), str.end());
#endif
    }

    inline std::string PathToUtf8(const std::filesystem::path& p) {
        return WStringToUtf8(p.wstring());
    }
    
    constexpr uint32_t INITIAL  = 0x24D1C;
    constexpr uint32_t MULT     = 0x41C64E6D;
    constexpr size_t   KEY_SIZE = 0x81;

    
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

    inline void XorBuffer(uint8_t* buffer, size_t size, size_t file_offset) {
        // Generate key buffer
        std::array<uint8_t, KEY_SIZE> key{};
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

    /**
     * Returns a copy of @p s with every ASCII character converted to lower-case.
     * Centralises the repeated std::transform + ::tolower idiom that was
     * previously duplicated across every is_*_format helper.
     */
    inline std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    /**
     * Returns @p name with its file extension replaced by @p new_ext.
     * The extension is the last '.' and everything that follows it; when the
     * name has no extension @p new_ext is simply appended.
     * Example: ReplaceExtension("foo.sct", ".png") == "foo.png".
     */
    inline std::string ReplaceExtension(const std::string& name, const std::string& new_ext)
    {
        const auto dot = name.find_last_of('.');
        return (dot != std::string::npos ? name.substr(0, dot) : name) + new_ext;
    }
}
