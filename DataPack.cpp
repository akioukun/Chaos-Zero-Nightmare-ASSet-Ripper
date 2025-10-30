#include "DataPack.h"
#include "SCTParser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <array>
#include "DBParser.h"
#include "Logger.h"
DataPack::DataPack(const std::wstring& path) : pack_path_(path), type_(PackType::Unknown) {
    root_node_.name = "root";
    root_node_.data = Core::FolderInfo{};

    hFile_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile_ == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Failed to open file: " << path << std::endl;
        return;
    }

    LARGE_INTEGER fs;
    if (!GetFileSizeEx(hFile_, &fs)) {
        std::wcerr << L"Failed to get file size for: " << path << std::endl;
        CloseHandle(hFile_);
        hFile_ = INVALID_HANDLE_VALUE;
        return;
    }
    file_size_ = fs.QuadPart;

    hMapFile_ = CreateFileMapping(hFile_, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hMapFile_ == NULL) {
        std::wcerr << L"Failed to create file mapping for: " << path << std::endl;
        CloseHandle(hFile_);
        hFile_ = INVALID_HANDLE_VALUE;
        return;
    }

    mapped_data_ = (const uint8_t*)MapViewOfFile(hMapFile_, FILE_MAP_READ, 0, 0, 0);
    if (mapped_data_ == nullptr) {
        std::wcerr << L"Failed to map view of file: " << path << std::endl;
        return;
    }

    if (file_size_ < 5) {
        type_ = PackType::Unknown;
        return;
    }

    if (memcmp(mapped_data_, "\x71\x40\xBD\x73\x93", 5) == 0) type_ = PackType::Encrypted;
    else if (memcmp(mapped_data_, "\x50\x4C\x50\x63\x4B", 5) == 0) type_ = PackType::Decrypted;
    else type_ = PackType::Unknown;
}

DataPack::~DataPack() {
    if (mapped_data_) UnmapViewOfFile(mapped_data_);
    if (hMapFile_) CloseHandle(hMapFile_);
    if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
}

std::vector<uint8_t> DataPack::GetFileData(const Core::FileNode& node) {
    std::vector<uint8_t> data;

    if (!std::holds_alternative<Core::FileInfo>(node.data)) return data;

    const auto& info = std::get<Core::FileInfo>(node.data);

    uint64_t file_end = static_cast<uint64_t>(info.offset) + static_cast<uint64_t>(info.size);
    if (static_cast<uint64_t>(info.offset) >= static_cast<uint64_t>(file_size_) ||
        file_end > static_cast<uint64_t>(file_size_)) {
        std::cerr << "Invalid file offset/size for: " << node.name << std::endl;
        return data;
    }

    try {
        data.resize(info.size);
        memcpy(data.data(), mapped_data_ + info.offset, info.size);

        if (type_ == PackType::Encrypted) {
            Core::xor_buffer(data.data(), info.size, info.offset);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error reading file data: " << e.what() << std::endl;
        data.clear();
    }

    return data;
}

void DataPack::Scan(std::atomic<float>& progress) {
    auto& root_folder = std::get<Core::FolderInfo>(root_node_.data);
    root_folder.children.clear();

    try {
        if (type_ == PackType::Encrypted) ScanEncrypted(progress);
        else if (type_ == PackType::Decrypted) ScanDecrypted(progress);

        std::function<void(Core::FileNode&)> process_node = [&](Core::FileNode& node) {
            try {
                if (std::holds_alternative<Core::FileInfo>(node.data)) {
                    const auto& info = std::get<Core::FileInfo>(node.data);
                }
                else if (std::holds_alternative<Core::FolderInfo>(node.data)) {
                    auto& folder = std::get<Core::FolderInfo>(node.data);
                    for (auto& child : folder.children) {
                        process_node(child);
                    }
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Error processing node: " << node.name << " - " << e.what() << std::endl;
            }
            };

        process_node(root_node_);
    }
    catch (const std::exception& e) {
        std::cerr << "Error during scan: " << e.what() << std::endl;
    }

    progress = 1.0f;
}

void DataPack::ScanEncrypted(std::atomic<float>& progress) {
    size_t cursor = 0;
    uint8_t header_buffer[15];

    std::array<uint8_t, Core::KEY_SIZE> key;
    uint32_t current = Core::INITIAL;
    for (size_t i = 0; i < Core::KEY_SIZE; ++i) {
        current = (current * Core::MULT) & 0x7FFFFFFF;
        key[i] = (current >> 16) & 0xFF;
    }

    while (cursor < file_size_) {
        if ((cursor & 0xFFFF) == 0) {
            progress = (float)cursor / file_size_;
        }

        if (cursor + 1 > file_size_) break;

        if ((mapped_data_[cursor] ^ key[cursor % Core::KEY_SIZE]) == 0x02) {
            size_t header_offset = cursor - 4;

            if (header_offset < 0 || header_offset + 15 > file_size_) {
                cursor++;
                continue;
            }

            memcpy(header_buffer, &mapped_data_[header_offset], 15);
            Core::xor_buffer(header_buffer, 15, header_offset);

            uint32_t container_len = *(uint32_t*)&header_buffer[0];
            uint8_t path_len = header_buffer[5];
            uint32_t data_len = *(uint32_t*)&header_buffer[6];

            if (container_len > file_size_ || path_len == 0 || path_len > 1024 || data_len > file_size_) {
                cursor++;
                continue;
            }

            if (container_len == path_len + data_len + 19) {
                if (header_offset + 15 + path_len > file_size_) {
                    cursor++;
                    continue;
                }

                std::vector<uint8_t> path_buffer(path_len);
                memcpy(path_buffer.data(), &mapped_data_[header_offset + 15], path_len);
                Core::xor_buffer(path_buffer.data(), path_len, header_offset + 15);
                std::string path_str((char*)path_buffer.data(), path_len);

                uint32_t file_offset = header_offset + 15 + path_len;

                if (file_offset + data_len <= file_size_) {
                    AddFileToTree(path_str, file_offset, data_len);
                    cursor = file_offset + data_len;
                    continue;
                }
            }
        }
        cursor++;
    }
}

void DataPack::ScanDecrypted(std::atomic<float>& progress) {
    size_t cursor = 0;
    while (cursor < file_size_) {
        progress = (float)cursor / file_size_;

        const void* found = memchr(&mapped_data_[cursor], 0x02, file_size_ - cursor);
        if (!found) break;

        cursor = (const uint8_t*)found - mapped_data_;

        if (cursor < 4) {
            cursor++;
            continue;
        }

        size_t header_offset = cursor - 4;

        if (header_offset + 15 > file_size_) {
            cursor++;
            continue;
        }

        uint32_t container_len = *(uint32_t*)&mapped_data_[header_offset];
        uint8_t path_len = mapped_data_[header_offset + 5];
        uint32_t data_len = *(uint32_t*)&mapped_data_[header_offset + 6];

        if (container_len > file_size_ || path_len == 0 || path_len > 1024 || data_len > file_size_) {
            cursor++;
            continue;
        }

        if (container_len == path_len + data_len + 19) {
            if (header_offset + 15 + path_len > file_size_) {
                cursor++;
                continue;
            }

            std::string path_str((char*)&mapped_data_[header_offset + 15], path_len);
            uint32_t file_offset = header_offset + 15 + path_len;

            if (file_offset + data_len <= file_size_) {
                AddFileToTree(path_str, file_offset, data_len);
                cursor = file_offset + data_len;
            }
            else {
                cursor++;
            }
        }
        else {
            cursor++;
        }
    }
}

void DataPack::AddFileToTree(const std::string& path, uint32_t offset, uint32_t size) {
    try {
        std::filesystem::path p(path);
        auto* current_folder_info = &std::get<Core::FolderInfo>(root_node_.data);
        std::string current_path = "";

        for (const auto& part : p.parent_path()) {
            if (part.empty() || part.string() == "/") continue;
            current_path += part.string() + "/";

            auto it = std::find_if(current_folder_info->children.begin(), current_folder_info->children.end(),
                [&](const Core::FileNode& n) { return n.name == part.string(); });

            if (it == current_folder_info->children.end()) {
                Core::FileNode new_folder;
                new_folder.name = part.string();
                new_folder.full_path = current_path;
                new_folder.data = Core::FolderInfo{};
                current_folder_info->children.push_back(new_folder);
                current_folder_info = &std::get<Core::FolderInfo>(current_folder_info->children.back().data);
            }
            else {
                if (std::holds_alternative<Core::FolderInfo>(it->data)) {
                    current_folder_info = &std::get<Core::FolderInfo>(it->data);
                }
            }
        }
        Core::FileNode file_node;
        file_node.name = p.filename().string();
        file_node.full_path = path;
        file_node.data = Core::FileInfo{ offset, size, p.extension().string() };
        current_folder_info->children.push_back(std::move(file_node));
    }
    catch (const std::exception& e) {
        std::cerr << "Error adding file to tree: " << path << " - " << e.what() << std::endl;
    }
}

void DataPack::ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) {
	LogInfo("ExtractAll started");
	Extract(root_node_, output_path, progress, convert_sct_to_png, convert_db_to_json);
	LogInfo("ExtractAll finished");
}

void DataPack::Extract(const Core::FileNode& node, const std::wstring& output_path,
    std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) {
	uint64_t total_size_to_extract = 0;
	std::function<void(const Core::FileNode&)> F =
		[&](const Core::FileNode& n) {
		if (std::holds_alternative<Core::FileInfo>(n.data)) {
			total_size_to_extract += std::get<Core::FileInfo>(n.data).size;
		}
		else if (std::holds_alternative<Core::FolderInfo>(n.data)) {
			for (const auto& child : std::get<Core::FolderInfo>(n.data).children) F(child);
		}
		};
	F(node);
	if (total_size_to_extract == 0) {
		progress = 1.0f;
		return;
	}
	std::atomic<uint64_t> extracted_size = 0;
	LogInfo("Extract begin for node");
	ExtractNode(node, output_path, extracted_size, total_size_to_extract, progress, convert_sct_to_png, convert_db_to_json);
	progress = 1.0f;
	LogInfo("Extract end for node");
}

void DataPack::ExtractNode(const Core::FileNode& node, const std::wstring& current_path,
    std::atomic<uint64_t>& extracted_size, const uint64_t total_size,
    std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) {

    try {
        if (std::holds_alternative<Core::FileInfo>(node.data)) {
            const auto& info = std::get<Core::FileInfo>(node.data);
            std::filesystem::path final_path = std::filesystem::path(current_path) / node.name;
			LogInfo(std::string("Extracting file: ") + node.name + " size=" + std::to_string(info.size));

            std::string ext_lower = info.format;
            std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
            bool is_sct = (ext_lower == ".sct" || ext_lower == ".sct2");
            bool is_db = (ext_lower == ".db");


            if (is_sct && convert_sct_to_png) {
                final_path.replace_extension(".png");
            }

            if (is_db && convert_db_to_json) {
                final_path.replace_extension(".json");
            }

            std::filesystem::create_directories(final_path.parent_path());

            std::vector<uint8_t> buffer = GetFileData(node);

            if (!buffer.empty()) {

                if (is_sct && convert_sct_to_png) {
                    try {
                        LogInfo(std::string("Converting SCT to PNG: ") + node.name);
                        std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(buffer, false);
                        if (!png_data.empty()) {
                            buffer = std::move(png_data);
                        }
                    } catch (const std::exception& e) {
                        LogError(std::string("SCT conversion failed for ") + node.name + ": " + e.what());
                    }
                }

                if (is_db && convert_db_to_json) {
                    try {
                        LogInfo(std::string("Converting DB to JSON: ") + node.name);
                        std::ofstream out(final_path);
                        if (out.is_open()) {
                            bool ok = DBParser::ConvertToJsonToStream(buffer, out);
                            out.close();
                            if (!ok) {
                                LogError(std::string("DB conversion returned false for ") + node.name);
                                std::ofstream out2(final_path);
                                out2 << "{}";
                                out2.close();
                            }
                        } else {
                            LogError(std::string("Failed to open output for DB JSON: ") + final_path.string());
                        }
                        buffer.clear();
                    } catch (const std::exception& e) {
                        LogError(std::string("DB conversion exception for ") + node.name + ": " + e.what());
                    }
                }

                if (!buffer.empty()) {
                    std::ofstream out(final_path, std::ios::binary);
                    if (out.is_open()) {
                        out.write((const char*)buffer.data(), buffer.size());
                        out.close();
                    } else {
                        LogError(std::string("Failed to open output for raw write: ") + final_path.string());
                    }
                }
            } else {
                LogError(std::string("Empty buffer for file: ") + node.name);
            }
            extracted_size += info.size;
            if (total_size > 0) {
                progress = (float)extracted_size / total_size;
				if ((extracted_size & 0x7FFFFF) == 0) {
					LogInfo(std::string("Progress: ") + std::to_string((int)(progress * 100)) + "%");
				}
            }
        }
        else if (std::holds_alternative<Core::FolderInfo>(node.data)) {
            const auto& info = std::get<Core::FolderInfo>(node.data);
            std::filesystem::path new_path = std::filesystem::path(current_path) / node.name;

            if (node.name != "root") {
                std::filesystem::create_directories(new_path);
            }
            else {
                new_path = current_path;
            }

            for (const auto& child : info.children) {
               ExtractNode(child, new_path, extracted_size, total_size, progress, convert_sct_to_png, convert_db_to_json);
            }
        }
    }
    catch (const std::exception& e) {
        LogError(std::string("Error extracting node: ") + node.name + " - " + e.what());
        std::cerr << "Error extracting node: " << node.name << " - " << e.what() << std::endl;
    }
}
