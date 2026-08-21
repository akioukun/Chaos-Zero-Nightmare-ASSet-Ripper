#include "ArchiveBase.h"
#include "parsers/SCTParser.h"
#include "parsers/DBParser.h"
#include "parsers/SCSPParser.h"
#include "core/Logger.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <functional>
#include <variant>

ArchiveBase::ArchiveBase()
{
    root_node.name = "/";
    root_node.full_path = "/";
    root_node.data = Core::FolderInfo{};
}

void ArchiveBase::AddFileToTree(const std::string& path, uint64_t offset, uint64_t size, uint32_t archive_id)
{
    try
    {
        std::string clean_path = path;
        size_t end = clean_path.find('\0');
        if (end != std::string::npos) clean_path = clean_path.substr(0, end);
        std::replace(clean_path.begin(), clean_path.end(), '\\', '/');
        while (!clean_path.empty() && clean_path.back() == '/') clean_path.pop_back();
        while (!clean_path.empty() && clean_path.front() == '/') clean_path.erase(clean_path.begin());

        if (clean_path.empty()) return;

        std::vector<std::string> parts;
        size_t start = 0;
        while (start < clean_path.size())
        {
            size_t slash = clean_path.find('/', start);
            size_t end = (slash == std::string::npos) ? clean_path.size() : slash;
            if (end > start)
            {
                parts.push_back(clean_path.substr(start, end - start));
            }
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }

        if (parts.empty())
            return;

        Core::FileNode* current = &root_node;
        std::string current_path = "";

        for (size_t i = 0; i < parts.size() - 1; ++i)
        {
            current_path += (current_path.empty() ? "" : "/") + parts[i];
            
            if (!std::holds_alternative<Core::FolderInfo>(current->data)) {
                LogError("Cannot add file to tree: intermediate node is not a folder.");
                return;
            }
            auto& folder_info = std::get<Core::FolderInfo>(current->data);
            
            auto it = std::find_if(folder_info.children.begin(), folder_info.children.end(),
                                   [&](const Core::FileNode& n) { return n.name == parts[i]; });
            if (it == folder_info.children.end())
            {
                Core::FileNode new_folder;
                new_folder.name = parts[i];
                new_folder.full_path = current_path;
                new_folder.data = Core::FolderInfo{};
                folder_info.children.push_back(new_folder);
                current = &folder_info.children.back();
            }
            else
            {
                current = &(*it);
            }
        }

        if (!std::holds_alternative<Core::FolderInfo>(current->data)) {
            LogError("Cannot add file to tree: parent node is not a folder.");
            return;
        }

        auto& folder_info = std::get<Core::FolderInfo>(current->data);
        const std::string& filename = parts.back();

        auto existing_it = std::find_if(folder_info.children.begin(), folder_info.children.end(),
                                        [&](const Core::FileNode& n) { return n.name == filename; });
        if (existing_it != folder_info.children.end())
        {
            // If the file already exists, we might be overwriting from another archive. Update it!
            if (std::holds_alternative<Core::FileInfo>(existing_it->data))
            {
                auto& existing_info = std::get<Core::FileInfo>(existing_it->data);
                existing_info.offset = offset;
                existing_info.size = size;
                existing_info.archive_id = archive_id;
            }
            return;
        }

        Core::FileNode new_file;
        new_file.name = filename;
        new_file.full_path = (current_path.empty() ? "" : current_path + "/") + filename;

        Core::FileInfo info;
        info.offset = offset;
        info.size = size;
        info.archive_id = archive_id;

        size_t dot_pos = filename.find_last_of('.');
        if (dot_pos != std::string::npos)
        {
            info.format = filename.substr(dot_pos);
        }
        else
        {
            info.format = "";
        }

        new_file.data = info;
        folder_info.children.push_back(new_file);

        parsed_file_count.fetch_add(1, std::memory_order_relaxed);
        parsed_total_size.fetch_add(size, std::memory_order_relaxed);
    }
    catch (const std::exception& e)
    {
        LogError("Error adding file to tree: " + std::string(path) + " - " + std::string(e.what()));
    }
}

void ArchiveBase::ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json)
{
    LogInfo("ExtractAll started");
    Extract(root_node, output_path, progress, convert_sct_to_png, convert_db_to_json);
    LogInfo("ExtractAll finished");
}

void ArchiveBase::Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json)
{
    uint64_t total_size_to_extract = 0;
    std::function<void(const Core::FileNode&)> F = [&](const Core::FileNode& n)
    {
        if (std::holds_alternative<Core::FileInfo>(n.data))
        {
            total_size_to_extract += std::get<Core::FileInfo>(n.data).size;
        }
        else if (std::holds_alternative<Core::FolderInfo>(n.data))
        {
            for (const auto& child : std::get<Core::FolderInfo>(n.data).children)
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

void ArchiveBase::ExtractNode(const Core::FileNode& node, const std::wstring& current_path, std::atomic<uint64_t>& extracted_size, const uint64_t total_size, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json)
{
    try
    {
        if (std::holds_alternative<Core::FileInfo>(node.data))
        {
            const auto& info = std::get<Core::FileInfo>(node.data);
            std::filesystem::path final_path = std::filesystem::path(current_path) / node.name;
            LogInfo(std::string("Extracting file: ") + node.name + " size=" + std::to_string(info.size));

            std::string ext_lower = info.format;
            std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
            bool is_sct = (ext_lower == ".sct" || ext_lower == ".sct2");
            bool is_db = (ext_lower == ".db");
            bool is_scsp = (ext_lower == ".scsp");
            bool is_atlas = (ext_lower == ".atlas");

            if (is_sct && convert_sct_to_png)
                final_path.replace_extension(".png");
            if (is_db && convert_db_to_json)
                final_path.replace_extension(".json");
            if (is_scsp)
                final_path.replace_extension(".json");

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
                    catch (const std::exception& e)
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
                    catch (const std::exception& e)
                    {
                        LogError(std::string("Atlas rewrite failed for ") + node.name + ": " + e.what());
                    }
                }

                if (is_db && convert_db_to_json)
                {
                    try
                    {
                        LogInfo(std::string("Converting DB to JSON: ") + node.name);
                        std::string json_str = DBParser::ConvertToJson(buffer);
                        buffer.assign(json_str.begin(), json_str.end());
                    }
                    catch (const std::exception& e)
                    {
                        LogError(std::string("DB to JSON conversion failed for ") + node.name + ": " + e.what());
                    }
                }

                if (is_scsp)
                {
                    try
                    {
                        LogInfo(std::string("Converting SCSP to JSON: ") + node.name);
                        std::string json_str = SCSPParser::ConvertSCSPToJson(buffer);
                        buffer.assign(json_str.begin(), json_str.end());
                    }
                    catch (const std::exception& e)
                    {
                        LogError(std::string("SCSP to JSON conversion failed for ") + node.name + ": " + e.what());
                    }
                }

                std::ofstream out(final_path, std::ios::binary);
                if (out.is_open())
                {
                    out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                }
                else
                {
                    LogError(std::string("Failed to open file for writing: ") + Core::PathToUtf8(final_path));
                }
            }

            extracted_size.fetch_add(info.size, std::memory_order_relaxed);
            progress = static_cast<float>(extracted_size.load()) / total_size;
        }
        else if (std::holds_alternative<Core::FolderInfo>(node.data))
        {
            const auto& folder_info = std::get<Core::FolderInfo>(node.data);
            std::filesystem::path new_path = current_path;
            if (node.name != "/")
            {
                new_path /= node.name;
            }
            for (const auto& child : folder_info.children)
            {
                ExtractNode(child, new_path.wstring(), extracted_size, total_size, progress, convert_sct_to_png, convert_db_to_json);
            }
        }
    }
    catch (const std::exception& e)
    {
        LogError("Error extracting node: " + node.name + " - " + std::string(e.what()));
    }
}
