#include "CompositeArchive.h"
#include <functional>
#include <thread>
#include <chrono>

CompositeArchive::CompositeArchive(const std::wstring& base_pack_path)
{
    this->pack_path = base_pack_path;
    this->type = PackType::Composite;
}

void CompositeArchive::AddArchive(std::unique_ptr<IArchive> archive)
{
    archives.push_back(std::move(archive));
}

void CompositeArchive::Scan(std::atomic<float>& progress)
{
    if (archives.empty()) {
        progress = 1.0f;
        return;
    }

    float weight = 1.0f / archives.size();

    for (size_t i = 0; i < archives.size(); ++i) {
        std::atomic<float> sub_progress{0.0f};
        std::atomic<bool> done{false};
        std::thread monitor([&]() {
            while (!done.load(std::memory_order_relaxed)) {
                progress = (static_cast<float>(i) + sub_progress.load(std::memory_order_relaxed)) * weight;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });

        archives[i]->Scan(sub_progress);
        done = true;
        if (monitor.joinable()) {
            monitor.join();
        }
        
        // Merge tree
        std::function<void(const Core::FileNode&)> merge_func = [&](const Core::FileNode& n) {
            if (std::holds_alternative<Core::FileInfo>(n.data)) {
                const auto& info = std::get<Core::FileInfo>(n.data);
                // We preserve the offset and size, but set the archive_id to our child's index
                AddFileToTree(n.full_path, info.offset, info.size, static_cast<uint32_t>(i));
            } else if (std::holds_alternative<Core::FolderInfo>(n.data)) {
                const auto& folder_info = std::get<Core::FolderInfo>(n.data);
                for (const auto& child : folder_info.children) {
                    merge_func(child);
                }
            }
        };

        merge_func(archives[i]->GetFileTree());
        progress = (i + 1) * weight;
    }
    SortTree();
    progress = 1.0f;
}

std::vector<uint8_t> CompositeArchive::GetFileData(const Core::FileNode& node)
{
    if (std::holds_alternative<Core::FileInfo>(node.data)) {
        const auto& info = std::get<Core::FileInfo>(node.data);
        if (info.archive_id < archives.size()) {
            // Find the node in the child archive
            // But wait, the child archive's GetFileData takes a FileNode.
            // We can just pass it directly if we give it our node.
            // However, our node has the composite's archive_id.
            // The child archive might not care about archive_id since it only has one ID anyway.
            // Let's create a temporary node for the child.
            Core::FileNode child_node = node;
            return archives[info.archive_id]->GetFileData(child_node);
        }
    }
    return {};
}
