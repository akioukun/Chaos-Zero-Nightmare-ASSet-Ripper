#pragma once
#include "Core.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <windows.h>

class DataPack {
public:
    enum class PackType { Unknown, Encrypted, Decrypted };

    DataPack(const std::wstring& path);
    ~DataPack();

    DataPack(const DataPack&) = delete;
    DataPack& operator=(const DataPack&) = delete;

    PackType GetType() const { return type_; }
    const Core::FileNode& GetFileTree() const { return root_node_; }

    void Scan(std::atomic<float>& progress);
    void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress);
    void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress);

    std::vector<uint8_t> GetFileData(const Core::FileNode& node);

    std::wstring pack_path_;

private:
    void ScanEncrypted(std::atomic<float>& progress);
    void ScanDecrypted(std::atomic<float>& progress);
    void AddFileToTree(const std::string& path, uint32_t offset, uint32_t size);
    void ExtractNode(const Core::FileNode& node, const std::wstring& current_path,
        std::atomic<uint64_t>& extracted_size, const uint64_t total_size,
        std::atomic<float>& progress);

    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMapFile_ = NULL;
    const uint8_t* mapped_data_ = nullptr;
    size_t file_size_ = 0;
    PackType type_;
    Core::FileNode root_node_;
};
