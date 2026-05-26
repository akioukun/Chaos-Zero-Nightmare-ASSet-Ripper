#pragma once
#include "Core.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include "SCSPParser.h"

class DataPack;

struct SpineEntry {
    std::string name;                    // raw .scsp filename minus extension
    std::string display_name;            // parent folder name (e.g. "e_terra_underlab_1")
    std::string category;                // top-level folder (e.g. "spine")
    const Core::FileNode* scsp_node;     // the .scsp skeleton file
    const Core::FileNode* atlas_node;    // the matching .atlas file (if found)
    mutable std::vector<const Core::FileNode*> image_nodes; // texture images referenced by atlas

    // Header info extracted from .scsp
    mutable SCSPParser::HeaderInfo header_info;
    mutable bool details_loaded = false;
};

struct SpineCategory {
    std::string name;
    std::string full_path;
    std::vector<size_t> entry_indices; // indices into flat entries vector
    std::map<std::string, SpineCategory> subcategories;
};

class SpineDictionary {
public:
    void Build(DataPack& pack, const Core::FileNode& root);
    void Clear();
    void EnsureDetailsLoaded(DataPack& pack, const SpineEntry& entry) const;

    const std::vector<SpineEntry>& GetEntries() const { return entries; }
    const SpineCategory& GetRootCategory() const { return root_category; }
    bool IsBuilt() const { return built; }

private:
    void CollectFiles(const Core::FileNode& node);
    void MatchEntries(DataPack& pack);
    void BuildCategories();
    std::vector<std::string> ParseAtlasTextureNames(const std::vector<uint8_t>& atlas_data) const;
    const Core::FileNode* FindSiblingByName(const std::string& scsp_path, const std::string& filename);

    std::vector<SpineEntry> entries;
    SpineCategory root_category;
    bool built = false;

    // Lookup tables built during scan
    std::unordered_map<std::string, const Core::FileNode*> scsp_files;
    std::unordered_map<std::string, const Core::FileNode*> atlas_files;
    std::unordered_map<std::string, const Core::FileNode*> all_files;
};
