#pragma once
#include "core/Core.h"
#include <vector>
#include <string>
#include <atomic>

class IArchive {
public:
    enum class PackType { Unknown, Encrypted, Decrypted, LocalDirectory, Composite, SSRA };

    virtual ~IArchive() = default;

    virtual PackType GetType() const = 0;
    virtual const Core::FileNode& GetFileTree() const = 0;
    virtual std::wstring GetPackPath() const = 0;
    virtual uint32_t GetParsedFileCount() const = 0;
    virtual uint64_t GetParsedTotalSize() const = 0;

    virtual void Scan(std::atomic<float>& progress) = 0;
    virtual void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false) = 0;
    virtual void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false) = 0;
    virtual std::vector<uint8_t> GetFileData(const Core::FileNode& node) = 0;
};
