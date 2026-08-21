#pragma once
#include "ArchiveBase.h"
#include <memory>
#include <vector>

class CompositeArchive : public ArchiveBase {
public:
    CompositeArchive(const std::wstring& base_pack_path);
    ~CompositeArchive() override = default;

    void AddArchive(std::unique_ptr<IArchive> archive);

    void Scan(std::atomic<float>& progress) override;
    std::vector<uint8_t> GetFileData(const Core::FileNode& node) override;

private:
    std::vector<std::unique_ptr<IArchive>> archives;
};
