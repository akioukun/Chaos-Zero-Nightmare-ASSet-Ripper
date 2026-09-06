#include "ArchiveFactory.h"
#include "DataPack.h"
#include "SSRArchive.h"
#include "CompositeArchive.h"
#include "core/Logger.h"
#include <filesystem>

std::unique_ptr<IArchive> CreateArchive(const std::wstring& wpath) {
    std::filesystem::path p(wpath);
    if (p.filename() == L"manifest.ssra" || p.extension() == L".ssra") {
        return std::make_unique<SSRArchive>(wpath);
    }
    std::filesystem::path dir = std::filesystem::is_directory(p) ? p : p.parent_path();
    std::filesystem::path gameres_path = dir / L"gameres";

    if (std::filesystem::exists(gameres_path) && std::filesystem::is_directory(gameres_path)) {
        auto composite = std::make_unique<CompositeArchive>(wpath);
        composite->AddArchive(std::make_unique<DataPack>(wpath));

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(gameres_path)) {
                if (entry.is_regular_file() && entry.path().filename() == L"manifest.ssra") {
                    composite->AddArchive(std::make_unique<SSRArchive>(entry.path().wstring()));
                }
            }
        } catch (const std::exception& e) {
            LogError("Error scanning gameres directory: " + std::string(e.what()));
        }
        return composite;
    }
    return std::make_unique<DataPack>(wpath);
}
