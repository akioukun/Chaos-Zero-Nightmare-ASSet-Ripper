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

    PackType GetType() const { return type; }
    const Core::FileNode& GetFileTree() const { return root_node; }
    std::wstring GetPackPath() const { return pack_path; }
    uint32_t GetParsedFileCount() const { return parsed_file_count.load(); }
    uint64_t GetParsedTotalSize() const { return parsed_total_size.load(); }

    void Scan(std::atomic<float>& progress);
    void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false);
    void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false);
    std::vector<uint8_t> GetFileData(const Core::FileNode& node);

private:
    // this maps only a portion of file at a time.
    struct SlidingView {
        const uint8_t* data = nullptr;   // pointer returned by MapViewOfFile
        uint64_t       offset = 0;       // file offset this view starts at
        size_t         size = 0;         // number of bytes mapped in this view
    };

    struct PackPart {
        HANDLE       hFile = INVALID_HANDLE_VALUE;
        HANDLE       hMapFile = NULL;
        uint64_t     fileSize = 0;
        SlidingView  view;
    };

    // default sliding window size: 64 MB.
    static constexpr size_t WINDOW_SIZE = 64ULL * 1024 * 1024;

    // queried once in constructor
    DWORD alloc_granularity = 65536;

    void ScanEncrypted(std::atomic<float>& progress);
    void ScanDecrypted(std::atomic<float>& progress);
    void AddFileToTree(const std::string& path, uint64_t offset, uint64_t size);  
    void ExtractNode(const Core::FileNode& node, const std::wstring& current_path, std::atomic<uint64_t>& extracted_size, const uint64_t total_size, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json);
    
    std::vector<std::wstring> FindPackParts(const std::wstring& basePath);
    bool LoadPackPart(const std::wstring& path, size_t partIndex);

    bool EnsureWindow(PackPart& part, uint64_t offset, size_t needed);
    const uint8_t* GetDataAtOffset(uint64_t offset, size_t& outSize);
    size_t ReadBytes(uint64_t offset, void* dest, size_t count);

    std::wstring pack_path;
    std::atomic<uint32_t> parsed_file_count{0};
    std::atomic<uint64_t> parsed_total_size{0};

    std::vector<PackPart> parts;
    uint64_t total_file_size = 0;
    PackType type;
    Core::FileNode root_node;
};