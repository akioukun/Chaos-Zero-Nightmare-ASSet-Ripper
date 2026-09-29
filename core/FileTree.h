#pragma once
#include "Core.h"
#include <cstdint>
#include <cstdio>
#include <string>

// Traversal and formatting helpers shared by the GUI and the headless CLI. Header-only so both targets pick them up without a CMake change, and free of any
// SDL, OpenGL, or application state so they stay usable from a console build.
namespace Core
{
    /**
     * Depth-first search for a node by its archive-relative full_path.
     * @param current The node to search from.
     * @param path The full_path to match, in the forward-slash form the tree stores.
     * @returns The matching node, or nullptr when nothing matches.
     */
    inline const FileNode* FindNodeByPath(const FileNode& current, const std::string& path)
    {
        if (current.full_path == path)
            return &current;

        if (std::holds_alternative<FolderInfo>(current.data))
        {
            for (const auto& child : std::get<FolderInfo>(current.data).children)
            {
                if (const FileNode* found = FindNodeByPath(child, path))
                    return found;
            }
        }
        return nullptr;
    }

    /**
     * Sums the stored sizes and counts the files under a node in a single walk.
     * @param node The node to measure.
     * @param out_bytes Accumulates the total bytes. Not reset, so callers must zero it first.
     * @param out_files Accumulates the file count. Not reset, so callers must zero it first.
     */
    inline void NodeStats(const FileNode& node, uint64_t& out_bytes, uint32_t& out_files)
    {
        if (std::holds_alternative<FileInfo>(node.data))
        {
            out_bytes += std::get<FileInfo>(node.data).size;
            ++out_files;
            return;
        }

        for (const auto& child : std::get<FolderInfo>(node.data).children)
            NodeStats(child, out_bytes, out_files);
    }

    /**
     * Sums the stored sizes of every file under a node.
     * @param node The node to measure.
     * @returns Total bytes, counting the node itself when it is a file.
     */
    inline uint64_t NodeTotalBytes(const FileNode& node)
    {
        uint64_t bytes = 0;
        uint32_t files = 0;
        NodeStats(node, bytes, files);
        return bytes;
    }

    /**
     * Counts the files under a node.
     * @param node The node to measure.
     * @returns File count, counting the node itself when it is a file.
     */
    inline uint32_t NodeFileCount(const FileNode& node)
    {
        uint64_t bytes = 0;
        uint32_t files = 0;
        NodeStats(node, bytes, files);
        return files;
    }

    /**
     * Formats a byte count as a short human readable string.
     * @param bytes The byte count.
     * @returns A string such as "1.42 GB".
     */
    inline std::string FormatSize(uint64_t bytes)
    {
        static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        double size = static_cast<double>(bytes);
        int unit = 0;
        while (size >= 1024.0 && unit < 4)
        {
            size /= 1024.0;
            ++unit;
        }

        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.2f %s", size, units[unit]);
        return buffer;
    }
}
