#include "SpineDictionary.h"
#include "archive/IArchive.h"
#include "SCSPParser.h"
#include "core/Logger.h"
#include <algorithm>
#include <sstream>
#include <set>

namespace {
    std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    std::string get_extension(const std::string& name) {
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return "";
        return to_lower(name.substr(dot));
    }

    std::string strip_extension(const std::string& name) {
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return name;
        return name.substr(0, dot);
    }

    std::string get_directory(const std::string& path) {
        size_t sep = path.find_last_of('/');
        if (sep == std::string::npos) return "";
        return path.substr(0, sep);
    }

    std::string get_basename(const std::string& path) {
        size_t sep = path.find_last_of('/');
        if (sep == std::string::npos) return path;
        return path.substr(sep + 1);
    }

    std::string get_first_component(const std::string& path) {
        size_t sep = path.find('/');
        if (sep == std::string::npos) return path;
        return path.substr(0, sep);
    }

    std::string normalize_path(const std::string& path) {
        std::string result = path;
        for (auto& c : result) {
            if (c == '\\') c = '/';
        }
        return result;
    }

    // Split path into components: "a/b/c/d.scsp" -> ["a", "b", "c", "d.scsp"]
    std::vector<std::string> split_path(const std::string& path) {
        std::vector<std::string> parts;
        std::istringstream ss(path);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    }

    bool is_image_extension(const std::string& name) {
        std::string ext = get_extension(name);
        return ext == ".sct" || ext == ".sct2" || ext == ".png" || ext == ".jpg"
            || ext == ".jpeg" || ext == ".bmp" || ext == ".webp" || ext == ".tga";
    }

    // Build a display name from full path. Shows folder path + filename when needed.
    // "face/portrait/30084.scsp" -> "portrait/30084"
    // "model/30084.scsp" -> "30084"
    // "spine/characters/hero_01/model/model.scsp" -> "characters/hero_01/model"
    // Always appends the filename (without ext) when the last folder == filename base,
    // or when there's only one folder level to avoid all entries showing the same name.
    std::string build_display_name(const std::string& full_path) {
        auto parts = split_path(full_path);
        if (parts.size() <= 1) return strip_extension(get_basename(full_path));

        std::string filename = strip_extension(parts.back());
        size_t start = 1;  // skip category (first component)
        size_t end = parts.size() - 1;  // skip filename

        if (start >= end) {
            // Only category + filename, no middle path
            return filename;
        }

        std::string result;
        for (size_t i = start; i < end; i++) {
            if (!result.empty()) result += "/";
            result += parts[i];
        }

        // Always append the filename if the folder path alone is too generic
        // (i.e., only one folder component between category and filename)
        // This prevents "face/portrait/30084.scsp" from showing as just "portrait"
        if (end - start <= 1 && filename != parts[end - 1]) {
            result += "/" + filename;
        }

        return result;
    }
}

void SpineDictionary::Clear() {
    entries.clear();
    root_category.subcategories.clear();
    root_category.entry_indices.clear();
    scsp_files.clear();
    atlas_files.clear();
    all_files.clear();
    built = false;
}

void SpineDictionary::CollectFiles(const Core::FileNode& node) {
    if (std::holds_alternative<Core::FileInfo>(node.data)) {
        std::string path = normalize_path(node.full_path);
        std::string ext = get_extension(node.name);
        all_files[path] = &node;

        if (ext == ".scsp") {
            scsp_files[path] = &node;
        } else if (ext == ".atlas") {
            atlas_files[path] = &node;
        }
    } else {
        const auto& folder = std::get<Core::FolderInfo>(node.data);
        for (const auto& child : folder.children) {
            CollectFiles(child);
        }
    }
}

const Core::FileNode* SpineDictionary::FindSiblingByName(const std::string& scsp_path, const std::string& filename) {
    std::string dir = get_directory(scsp_path);
    std::string target = dir.empty() ? filename : dir + "/" + filename;
    auto it = all_files.find(normalize_path(target));
    if (it != all_files.end()) return it->second;
    return nullptr;
}

std::vector<std::string> SpineDictionary::ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data) const {
    std::vector<std::string> textures;
    std::string content(atlas_data.begin(), atlas_data.end());
    std::istringstream ss(content);
    std::string line;
    bool after_blank = true;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty()) {
            after_blank = true;
            continue;
        }

        if (after_blank && !line.empty() && line[0] != ' ' && line[0] != '\t') {
            if (line.find('.') != std::string::npos) {
                textures.push_back(line);
            }
        }
        after_blank = false;
    }

    return textures;
}

void SpineDictionary::MatchEntries(IArchive& pack) {
    int skipped_no_atlas = 0;

    std::unordered_map<std::string, std::vector<const Core::FileNode*>> atlases_by_dir;
    for (auto& [apath, anode] : atlas_files) {
        atlases_by_dir[get_directory(apath)].push_back(anode);
    }

    for (auto& [scsp_path, scsp_node] : scsp_files) {
        SpineEntry entry;
        entry.name = strip_extension(scsp_node->name);
        entry.scsp_node = scsp_node;
        entry.atlas_node = nullptr;

        std::string norm_path = normalize_path(scsp_node->full_path);
        
        // Display name = filename without extension (e.g. "30084" or "model")
        entry.display_name = strip_extension(scsp_node->name);
        // Category = full directory path, drives the tree hierarchy
        entry.category = get_directory(norm_path);

        std::string dir = get_directory(norm_path);
        std::string base_name = strip_extension(scsp_node->name);

        std::string atlas_candidate = dir.empty() ? base_name + ".atlas" : dir + "/" + base_name + ".atlas";
        auto atlas_it = atlas_files.find(normalize_path(atlas_candidate));
        if (atlas_it != atlas_files.end()) {
            entry.atlas_node = atlas_it->second;
        }

        if (!entry.atlas_node) {
            auto dir_it = atlases_by_dir.find(dir);
            if (dir_it != atlases_by_dir.end() && dir_it->second.size() == 1) {
                entry.atlas_node = dir_it->second[0];
            }
        }

        if (!entry.atlas_node) {
            skipped_no_atlas++;
            continue;
        }

        entries.push_back(std::move(entry));
    }

    LogInfo("SpineDictionary: " + std::to_string(entries.size()) + " entries with atlas, "
            + std::to_string(skipped_no_atlas) + " skipped (no atlas)");

    // Sort by category path first, then by display name within each folder
    std::sort(entries.begin(), entries.end(),
        [](const SpineEntry& a, const SpineEntry& b) {
            if (a.category != b.category) return a.category < b.category;
            return a.display_name < b.display_name;
        });
}

void SpineDictionary::EnsureDetailsLoaded(IArchive& pack, const SpineEntry& entry) const {
    if (entry.details_loaded) return;
    entry.details_loaded = true;

    try {
        std::vector<uint8_t> data = pack.GetFileData(*entry.scsp_node);
        if (!data.empty()) {
            SCSPParser::HeaderInfo hdr = SCSPParser::ExtractHeader(data);
            entry.header_info = hdr;
        }
    } catch (...) {}

    try {
        std::vector<uint8_t> atlas_data = pack.GetFileData(*entry.atlas_node);
        if (!atlas_data.empty()) {
            auto tex_names = ParseAtlasTextureNames(atlas_data);
            std::string scsp_path = normalize_path(entry.scsp_node->full_path);
            
            for (const auto& tex_name : tex_names) {
                std::string dir = get_directory(scsp_path);
                std::string target = dir.empty() ? tex_name : dir + "/" + tex_name;
                auto it = all_files.find(normalize_path(target));
                const Core::FileNode* img = (it != all_files.end()) ? it->second : nullptr;

                if (img && is_image_extension(img->name)) {
                    entry.image_nodes.push_back(img);
                }

                if (!img && !entry.header_info.images_path.empty()) {
                    std::string img_path = normalize_path(entry.header_info.images_path);
                    if (!img_path.empty() && img_path.back() != '/') img_path += "/";
                    img_path += tex_name;
                    std::string scsp_dir = get_directory(scsp_path);
                    std::string full_img = scsp_dir.empty() ? img_path : scsp_dir + "/" + img_path;
                    auto it2 = all_files.find(normalize_path(full_img));
                    if (it2 != all_files.end() && is_image_extension(it2->second->name)) {
                        entry.image_nodes.push_back(it2->second);
                    }
                }
            }
        }
    } catch (...) {}
}

void SpineDictionary::BuildCategories() {
    root_category.name = "Root";
    root_category.full_path = "";
    root_category.entry_indices.clear();
    root_category.subcategories.clear();

    for (size_t i = 0; i < entries.size(); i++) {
        const std::string& cat_path = entries[i].category;
        
        if (cat_path.empty()) {
            root_category.entry_indices.push_back(i);
        } else {
            auto parts = split_path(cat_path);
            SpineCategory* current = &root_category;
            std::string cur_path = "";
            for (const auto& part : parts) {
                if (!cur_path.empty()) cur_path += "/";
                cur_path += part;
                
                auto& sub = current->subcategories[part];
                if (sub.name.empty()) {
                    sub.name = part;
                    sub.full_path = cur_path;
                }
                current = &sub;
            }
            current->entry_indices.push_back(i);
        }
    }
}

void SpineDictionary::Build(IArchive& pack, const Core::FileNode& root) {
    Clear();
    LogInfo("SpineDictionary: scanning file tree...");
    CollectFiles(root);
    LogInfo("SpineDictionary: found " + std::to_string(scsp_files.size()) + " .scsp, "
            + std::to_string(atlas_files.size()) + " .atlas, "
            + std::to_string(all_files.size()) + " total files");
    MatchEntries(pack);
    BuildCategories();
    LogInfo("SpineDictionary: built tree successfully");
    built = true;
}
