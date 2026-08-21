#pragma once
#include "IArchive.h"
#include <vector>
#include <string>
#include <atomic>

class ArchiveBase : public IArchive {
public:
    ArchiveBase();
    virtual ~ArchiveBase() = default;

    PackType GetType() const override { return type; }
    const Core::FileNode& GetFileTree() const override { return root_node; }
    std::wstring GetPackPath() const override { return pack_path; }
    uint32_t GetParsedFileCount() const override { return parsed_file_count.load(); }
    uint64_t GetParsedTotalSize() const override { return parsed_total_size.load(); }

    void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false) override;
    void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png = false, bool convert_db_to_json = false) override;

    virtual void Scan(std::atomic<float>& progress) override = 0;
    virtual std::vector<uint8_t> GetFileData(const Core::FileNode& node) override = 0;

protected:
    void AddFileToTree(const std::string& path, uint64_t offset, uint64_t size, uint32_t archive_id = 0);
    void ExtractNode(const Core::FileNode& node, const std::wstring& current_path, std::atomic<uint64_t>& extracted_size, const uint64_t total_size, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json);

    std::wstring pack_path;
    std::atomic<uint32_t> parsed_file_count{0};
    std::atomic<uint64_t> parsed_total_size{0};
    PackType type{PackType::Unknown};
    Core::FileNode root_node;
};
