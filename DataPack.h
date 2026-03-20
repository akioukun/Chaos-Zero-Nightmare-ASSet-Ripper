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
    void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false);
    void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false);
    std::vector<uint8_t> GetFileData(const Core::FileNode& node);

    std::wstring pack_path_;

private:
    struct PackPart {
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMapFile = NULL;
        const uint8_t* mapped_data = nullptr;
        size_t size = 0;
        static constexpr uint64_t PART_SIZE = 1073741824ULL; // 1GB
    };

    void ScanEncrypted(std::atomic<float>& progress);
    void ScanDecrypted(std::atomic<float>& progress);
    void AddFileToTree(const std::string& path, uint64_t offset, uint64_t size);  
    void ExtractNode(const Core::FileNode& node, const std::wstring& current_path, std::atomic<uint64_t>& extracted_size, const uint64_t total_size, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json);
    
    std::vector<std::wstring> FindPackParts(const std::wstring& basePath);
    bool LoadPackPart(const std::wstring& path, size_t partIndex);
    const uint8_t* GetDataAtOffset(uint64_t offset, size_t& outSize);

    std::vector<PackPart> parts_;
    uint64_t total_file_size_ = 0;
    PackType type_;
    Core::FileNode root_node_;
};