#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <array>
#include <variant>

namespace Core {
    
    static constexpr uint32_t INITIAL = 0x24D1C;
    static constexpr uint32_t MULT = 0x41C64E6D;
    static constexpr size_t KEY_SIZE = 0x81;

    
    struct FileNode;

    struct FileInfo {
        uint64_t offset;
        uint64_t size;
        std::string format;
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