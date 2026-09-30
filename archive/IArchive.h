#pragma once
#include "core/Core.h"
#include <vector>
#include <string>
#include <atomic>
#include <memory>

class IArchive {
public:
    enum class PackType { Unknown, Encrypted, Decrypted, LocalDirectory, Composite, SSRA };

    virtual ~IArchive() = default;

    [[nodiscard]] virtual PackType GetType() const = 0;
    [[nodiscard]] virtual const Core::FileNode& GetFileTree() const = 0;
    [[nodiscard]] virtual std::wstring GetPackPath() const = 0;
    [[nodiscard]] virtual uint32_t GetParsedFileCount() const = 0;
    [[nodiscard]] virtual uint64_t GetParsedTotalSize() const = 0;

    /**
     * Factory function that creates the appropriate IArchive implementation for the given path.
     * Picks SSRArchive for .ssra / manifest.ssra, CompositeArchive if a gameres directory exists,
     * or DataPack otherwise.
     */
    static std::unique_ptr<IArchive> Create(const std::wstring& wpath);

    virtual void Scan(std::atomic<float>& progress) = 0;
    virtual void Extract(const Core::FileNode& node, const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) = 0;
    virtual void ExtractAll(const std::wstring& output_path, std::atomic<float>& progress, bool convert_sct_to_png, bool convert_db_to_json) = 0;
    virtual std::vector<uint8_t> GetFileData(const Core::FileNode& node) = 0;
};
