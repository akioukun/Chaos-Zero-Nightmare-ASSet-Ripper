#pragma once
#include "core/Core.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <windows.h>
#include "ArchiveBase.h"

class DataPack : public ArchiveBase {
public:
    DataPack(const std::wstring& path);
    ~DataPack() override;
    DataPack(const DataPack&) = delete;
    DataPack& operator=(const DataPack&) = delete;

    void Scan(std::atomic<float>& progress) override;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override;

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
    void ScanLocalDirectory(std::atomic<float>& progress);
    
    std::vector<std::wstring> FindPackParts(const std::wstring& basePath);
    bool LoadPackPart(const std::wstring& path, size_t partIndex);

    bool EnsureWindow(PackPart& part, uint64_t offset, size_t needed) const;
    const uint8_t* GetDataAtOffset(uint64_t offset, size_t& outSize);
    size_t ReadBytes(uint64_t offset, void* dest, size_t count);

    std::vector<PackPart> parts;
    uint64_t total_file_size = 0;
};