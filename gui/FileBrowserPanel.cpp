#include "gui/FileBrowserPanel.h"
#include "gui/PreviewPanel.h"
#include "gui/UIHelpers.h"
#include "core/Core.h"
#include "core/FileTree.h"
#include "core/DialogPaths.h"

#include "nlohmann/json.hpp"
#include "nuklear.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

const std::string FILEMAP_GENERATOR_ID = "Chaos-Zero-Nightmare-ASSet-Ripper";

template <typename NodeT, typename SetT>
static void update_multi_selection(const bool ctrl, SetT &selected_nodes, const NodeT *&selected_node, const NodeT *node)
{
    if (ctrl)
    {
        auto it = selected_nodes.find(node);
        if (it != selected_nodes.end())
            selected_nodes.erase(it);
        else
            selected_nodes.insert(node);
    }
    else
    {
        selected_nodes.clear();
        selected_nodes.insert(node);
    }
    selected_node = node;
}

void handle_node_click(const Core::FileNode *node, const bool is_folder)
{
    bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    Uint32 current_time = SDL_GetTicks();
    Uint32 time_diff = current_time - g_state.browser.last_click_time;

    if (time_diff < 250 && node == g_state.browser.last_clicked_node)
    {
        g_state.browser.last_click_time = current_time;
        return;
    }

    g_state.browser.click_count = 0;
    update_multi_selection(ctrl_pressed, g_state.browser.selected_nodes, g_state.browser.selected_node, node);

    if (!is_folder)
    {
        load_preview(*node);
    }
    else
    {
        clear_preview();
    }

    g_state.browser.last_click_time = current_time;
    g_state.browser.last_clicked_node = node;
}

void handle_node_right_click(const Core::FileNode *node, struct nk_vec2 pos)
{
    g_state.context_menu.node = node;
    g_state.context_menu.position = pos;
    g_state.context_menu.visible = true;
}

static nlohmann::ordered_json build_filemap(const Core::FileNode &node)
{
    nlohmann::ordered_json j;
    j["name"] = node.name;
    j["path"] = node.full_path;

    if (std::holds_alternative<Core::FileInfo>(node.data))
    {
        const auto &info = std::get<Core::FileInfo>(node.data);
        j["type"] = "file";
        j["size"] = info.size;
        j["offset"] = info.offset;
        j["format"] = info.format;
    }
    else
    {
        const auto &[folder] = std::get<Core::FolderInfo>(node.data);
        j["type"] = "folder";
        j["children"] = nlohmann::ordered_json::array();
        for (const auto &child : folder)
        {
            j["children"].push_back(build_filemap(child));
        }
    }

    return j;
}

static bool is_valid_filemap(const nlohmann::ordered_json &j)
{
    if (!j.is_object())
        return false;

    if (!j.contains("generator") || !j["generator"].is_string())
        return false;

    if (const std::string gen = j["generator"].get<std::string>(); gen != FILEMAP_GENERATOR_ID)
    {
        return false;
    }

    if (!j.contains("type") || j["type"] != "folder" || !j.contains("children") || !j["children"].is_array())
    {
        return false;
    }

    return true;
}

void export_to_json()
{
    try
    {
        if (const std::string save_path = DialogPaths::SaveFile("Export File Map", "filemap.json", {"JSON Files", "*.json", "All Files", "*.*"}); !save_path.empty())
        {
            if (std::ofstream out(save_path); out.is_open())
            {
                nlohmann::ordered_json tree = build_filemap(g_state.browser.data_pack->GetFileTree());
                nlohmann::ordered_json j;
                j["generator"] = FILEMAP_GENERATOR_ID;
                for (auto &el : tree.items())
                {
                    j[el.key()] = el.value();
                }
                out << j.dump(2);
                out.close();
                g_state.common.success_message = "File map exported successfully!";
                g_state.common.show_success_popup = true;
                g_state.tasks.status = "Exported to: " + save_path;
            }
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
    }
}

static void navigate_old_tree(const nlohmann::ordered_json &j, std::map<std::string, uint64_t> &out_map)
{
    if (j.contains("type") && j["type"] == "file")
    {
        if (j.contains("path") && j.contains("size"))
        {
            out_map[j["path"].get<std::string>()] = j["size"].get<uint64_t>();
        }
    }
    else if (j.contains("type") && j["type"] == "folder" && j.contains("children"))
    {
        for (const auto &child : j["children"])
        {
            navigate_old_tree(child, out_map);
        }
    }
}

static void navigate_new_tree(const Core::FileNode &node, std::map<std::string, uint64_t> &out_map)
{
    if (std::holds_alternative<Core::FileInfo>(node.data))
    {
        const auto &info = std::get<Core::FileInfo>(node.data);
        out_map[node.full_path] = info.size;
    }
    else
    {
        const auto &[folder] = std::get<Core::FolderInfo>(node.data);
        for (const auto &child : folder)
        {
            navigate_new_tree(child, out_map);
        }
    }
}

static void insert_diff_node(DiffNode *root, const std::string &path, const uint64_t size, const DiffStatus status)
{
    std::string norm_path = path;
    std::replace(norm_path.begin(), norm_path.end(), '\\', '/');

    std::vector<std::string> parts;
    std::stringstream ss(norm_path);
    std::string part;
    while (std::getline(ss, part, '/'))
    {
        if (!part.empty())
            parts.push_back(part);
    }

    DiffNode *current = root;
    std::string current_full_path;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (!current_full_path.empty())
            current_full_path += '/';
        current_full_path += parts[i];

        const bool is_last = (i == parts.size() - 1);

        auto it = std::find_if(current->children.begin(), current->children.end(), [&](const std::unique_ptr<DiffNode> &n){ return n->name == parts[i]; });

        if (it != current->children.end())
        {
            current = it->get();
            if (is_last)
            {
                if (status != DiffStatus::Unchanged)
                    current->status = status;
            }
            else
            {
                if (status != DiffStatus::Unchanged && current->status == DiffStatus::Unchanged)
                {
                    current->status = DiffStatus::Modified;
                }
            }
        }
        else
        {
            auto new_node = std::make_unique<DiffNode>();
            new_node->name = parts[i];
            new_node->full_path = current_full_path;
            new_node->is_folder = !is_last;

            if (is_last)
            {
                new_node->size = size;
                new_node->status = status;
                if (const size_t dot_pos = parts[i].find_last_of('.'); dot_pos != std::string::npos)
                {
                    new_node->format = parts[i].substr(dot_pos);
                }
                else
                {
                    new_node->format = "";
                }
            }
            else
            {
                new_node->size = 0;
                new_node->status = (status == DiffStatus::Unchanged) ? DiffStatus::Unchanged : DiffStatus::Modified;
            }

            current->children.push_back(std::move(new_node));
            current = current->children.back().get();
        }
    }
}

static void build_diff_tree(const nlohmann::ordered_json &old_json)
{
    std::map<std::string, uint64_t> old_map;
    navigate_old_tree(old_json, old_map);

    std::map<std::string, uint64_t> new_map;
    if (g_state.browser.data_pack)
    {
        navigate_new_tree(g_state.browser.data_pack->GetFileTree(), new_map);
    }

    g_state.diff.root = std::make_unique<DiffNode>();
    g_state.diff.root->name = "Diff Root";
    g_state.diff.root->full_path = "";
    g_state.diff.root->is_folder = true;
    g_state.diff.root->status = DiffStatus::Unchanged;

    for (const auto &[key, value] : old_map)
    {
        const std::string &path = key;
        const uint64_t old_size = value;

        auto it = new_map.find(path);
        if (it == new_map.end())
        {
            insert_diff_node(g_state.diff.root.get(), path, old_size, DiffStatus::Removed);
        }
        else
        {
            if (it->second != old_size)
            {
                insert_diff_node(g_state.diff.root.get(), path, it->second, DiffStatus::Modified);
            }
            else
            {
                insert_diff_node(g_state.diff.root.get(), path, it->second, DiffStatus::Unchanged);
            }
        }
    }

    for (const auto &[key, value] : new_map)
    {
        const std::string &path = key;
        const uint64_t new_size = value;
        if (old_map.find(path) == old_map.end())
        {
            insert_diff_node(g_state.diff.root.get(), path, new_size, DiffStatus::Added);
        }
    }

    std::function<void(DiffNode *)> sort_tree = [&](DiffNode *node)
    {
        std::sort(node->children.begin(), node->children.end(), [](const std::unique_ptr<DiffNode> &a, const std::unique_ptr<DiffNode> &b)
                  {
            if (a->is_folder != b->is_folder) return a->is_folder > b->is_folder;
            return a->name < b->name; });
        for (auto &child : node->children)
        {
            sort_tree(child.get());
        }
    };
    sort_tree(g_state.diff.root.get());

    g_state.diff.show_tree = true;
    g_state.diff.expanded_folders.clear();
    g_state.diff.expanded_folders.insert(g_state.diff.root.get());
    g_state.diff.selected_node = nullptr;
    g_state.diff.selected_nodes.clear();
}

void set_file_tree_mode()
{
    g_state.spine.show_window = false;
    g_state.diff.show_tree = false;
}

void set_spine_viewer_mode()
{
    g_state.diff.show_tree = false;
    g_state.spine.show_window = true;
}

bool load_diff_tree_from_filemap()
{
    try
    {
        const std::string picked_path = DialogPaths::OpenFile(DialogPaths::Slot::Export, "Select an older filemap.json", {"JSON Files", "*.json", "All Files", "*.*"});
        if (picked_path.empty())
        {
            return false;
        }

        std::ifstream in(picked_path);
        if (!in.is_open())
        {
            g_state.tasks.status = "Error opening filemap.json for diff.";
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        const nlohmann::ordered_json old_json = nlohmann::ordered_json::parse(content);
        if (!is_valid_filemap(old_json))
        {
            g_state.tasks.status = "Error: Selected file is not a valid filemap generated by this tool.";
            g_state.common.error_message = "Selected file is not a valid filemap generated by this tool. Please select a valid filemap.json.";
            g_state.common.show_error_popup = true;
            return false;
        }

        build_diff_tree(old_json);
        g_state.tasks.status = "Diff view generated.";
        return true;
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Error parsing JSON: " + std::string(e.what());
        g_state.common.error_message = "Error parsing JSON: " + std::string(e.what());
        g_state.common.show_error_popup = true;
        return false;
    }
}

bool activate_diff_viewer()
{
    if (!g_state.diff.root)
    {
        if (!load_diff_tree_from_filemap())
        {
            return false;
        }
    }

    g_state.spine.show_window = false;
    g_state.diff.show_tree = true;
    return true;
}

static std::string diff_status_label(const DiffStatus status)
{
    switch (status)
    {
    case DiffStatus::Added:
        return "[ADD] ";
    case DiffStatus::Modified:
        return "[MOD] ";
    case DiffStatus::Removed:
        return "[DEL] ";
    default:
        return "";
    }
}

static std::string diff_display_name(const DiffNode &node)
{
    return diff_status_label(node.status) + node.name;
}

void handle_diff_node_click(const DiffNode *node, const bool is_folder)
{
    const bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    const Uint32 current_time = SDL_GetTicks();

    if (const Uint32 time_diff = current_time - g_state.browser.last_click_time; time_diff < 250 && node == g_state.diff.last_clicked_node)
    {
        g_state.browser.last_click_time = current_time;
        return;
    }

    update_multi_selection(ctrl_pressed, g_state.diff.selected_nodes, g_state.diff.selected_node, node);

    if (!is_folder && g_state.browser.data_pack)
    {
        if (const Core::FileNode *file_node = Core::FindNodeByPath(g_state.browser.data_pack->GetFileTree(), node->full_path))
        {
            load_preview(*file_node);
        }
        else
        {
            clear_preview();
            g_state.preview.error = "File not found in current datapack (perhaps it was removed).";
        }
    }
    else
    {
        clear_preview();
    }

    g_state.browser.last_click_time = current_time;
    g_state.diff.last_clicked_node = node;
}

static int get_diff_file_count(const DiffNode &node)
{
    if (!node.is_folder)
        return 1;
    int count = 0;
    for (const auto &child : node.children)
    {
        count += get_diff_file_count(*child);
    }
    return count;
}

static uint64_t get_diff_folder_size(const DiffNode &node)
{
    if (!node.is_folder)
        return node.size;
    uint64_t size = 0;
    for (const auto &child : node.children)
    {
        size += get_diff_folder_size(*child);
    }
    return size;
}

static bool matches_diff_search(const DiffNode &node, const std::string &query)
{
    if (query.empty())
        return true;
    std::string name_lower = node.name;
    std::string query_lower = query;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
    return name_lower.find(query_lower) != std::string::npos;
}

static bool has_matching_diff_child(const DiffNode &node, const std::string &query)
{
    if (query.empty())
        return true;
    if (matches_diff_search(node, query))
        return true;
    if (node.is_folder)
    {
        for (const auto &child : node.children)
        {
            if (has_matching_diff_child(*child, query))
                return true;
        }
    }
    return false;
}

void draw_diff_node(nk_context *ctx, const DiffNode &node, int depth)
{
    try
    {
        if (node.is_folder)
        {
            if (!has_matching_diff_child(node, g_state.browser.search_query))
                return;

            g_state.diff.visible_nodes.push_back(&node);

            const bool is_expanded = g_state.diff.expanded_folders.find(&node) != g_state.diff.expanded_folders.end();
            const bool is_selected = (g_state.diff.selected_nodes.find(&node) != g_state.diff.selected_nodes.end()) || (g_state.diff.selected_node == &node);

            const int file_count = get_diff_file_count(node);
            const std::string info = std::to_string(file_count) + " items | " + Core::FormatSize(get_diff_folder_size(node));
            const struct nk_color text_color = get_diff_status_color(node.status, is_selected, true);

            bool toggle_expand = false;
            if (draw_tree_row(ctx, depth, true, is_expanded, is_selected, diff_display_name(node), info, text_color, toggle_expand))
            {
                handle_diff_node_click(&node, true);
            }

            if (toggle_expand)
            {
                if (is_expanded)
                    g_state.diff.expanded_folders.erase(&node);
                else
                    g_state.diff.expanded_folders.insert(&node);
            }

            if (is_expanded)
            {
                for (const auto &child : node.children)
                    draw_diff_node(ctx, *child, depth + 1);
            }
        }
        else
        {
            if (!matches_diff_search(node, g_state.browser.search_query))
                return;

            g_state.diff.visible_nodes.push_back(&node);

            const bool is_selected = (g_state.diff.selected_nodes.find(&node) != g_state.diff.selected_nodes.end()) || (g_state.diff.selected_node == &node);
            const std::string size_str = Core::FormatSize(node.size) + " | " + node.format;
            const nk_color text_color = get_diff_status_color(node.status, is_selected, false);

            if (bool toggle_expand = false; draw_tree_row(ctx, depth, false, false, is_selected, diff_display_name(node), size_str, text_color, toggle_expand))
            {
                handle_diff_node_click(&node, false);
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error drawing diff node: " << e.what() << std::endl;
    }
}

void draw_file_node(nk_context *ctx, const Core::FileNode &node, const int depth)
{
    try
    {
        if (!has_matching_child(node, g_state.browser.search_query))
            return;

        g_state.browser.visible_nodes.push_back(&node);

        if (std::holds_alternative<Core::FolderInfo>(node.data))
        {
            const auto &[folder] = std::get<Core::FolderInfo>(node.data);
            const bool is_expanded = g_state.browser.expanded_folders.find(&node) != g_state.browser.expanded_folders.end();
            const bool is_selected = (g_state.browser.selected_nodes.find(&node) != g_state.browser.selected_nodes.end()) || (g_state.browser.selected_node == &node);

            const bool highlight_match = !g_state.browser.search_query.empty() && matches_search(node, g_state.browser.search_query);
            const nk_color text_color = is_selected ? nk_rgb(255, 255, 255) : (highlight_match ? nk_rgb(100, 255, 100) : nk_rgb(220, 220, 220));

            uint64_t bytes = 0;
            uint32_t files = 0;
            Core::NodeStats(node, bytes, files);
            const std::string info = std::to_string(files) + " items | " + Core::FormatSize(bytes);

            bool toggle_expand = false;
            if (draw_tree_row(ctx, depth, true, is_expanded, is_selected, node.name, info, text_color, toggle_expand))
            {
                handle_node_click(&node, true);
            }

            if (toggle_expand)
            {
                if (is_expanded)
                    g_state.browser.expanded_folders.erase(&node);
                else
                    g_state.browser.expanded_folders.insert(&node);
            }

            if (is_expanded)
            {
                for (const auto &child : folder)
                    draw_file_node(ctx, child, depth + 1);
            }
        }
        else
        {
            if (!matches_search(node, g_state.browser.search_query))
                return;

            const auto &file_info = std::get<Core::FileInfo>(node.data);
            const bool is_selected = (g_state.browser.selected_nodes.find(&node) != g_state.browser.selected_nodes.end()) || (g_state.browser.selected_node == &node);

            const nk_color text_color = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(200, 200, 200);
            const std::string size_str = Core::FormatSize(file_info.size) + " | " + file_info.format;

            bool right_clicked = false;
            struct nk_vec2 mouse_pos = {0, 0};
            if (bool toggle_expand = false; draw_tree_row(ctx, depth, false, false, is_selected, node.name, size_str, text_color, toggle_expand, &right_clicked, &mouse_pos))
            {
                handle_node_click(&node, false);
            }

            if (right_clicked)
            {
                handle_node_right_click(&node, mouse_pos);
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error drawing node: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Unknown error drawing node" << std::endl;
    }
}

void draw_file_browser_panel(nk_context *ctx, float left_width, const bool tree_scanned, bool &scroll_to_selected)
{
    if (nk_group_begin(ctx, "FileTree", NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        g_state.browser.visible_nodes.clear();

        if (g_state.browser.data_pack && tree_scanned)
        {
            if (g_state.diff.show_tree && g_state.diff.root)
            {
                draw_diff_node(ctx, *g_state.diff.root);

                if (scroll_to_selected && g_state.diff.selected_node)
                {
                    if (const auto it = std::find(g_state.diff.visible_nodes.begin(), g_state.diff.visible_nodes.end(), g_state.diff.selected_node); it != g_state.diff.visible_nodes.end())
                    {
                        const int index = static_cast<int>(std::distance(g_state.diff.visible_nodes.begin(), it));
                        nk_uint current_x, current_y;
                        nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

                        const float row_height = 26.0f + ctx->style.window.spacing.y;
                        const float node_y = index * row_height;
                        const float view_h = nk_window_get_content_region(ctx).h;

                        const float top_margin = row_height * 2.0f;
                        const float bottom_margin = row_height * 2.0f;
                        const float visible_top = static_cast<float>(current_y) + top_margin;
                        const float visible_bottom = static_cast<float>(current_y) + view_h - bottom_margin;

                        if (node_y < visible_top)
                        {
                            float target = node_y - top_margin;
                            if (target < 0.0f)
                                target = 0.0f;
                            nk_group_set_scroll(ctx, "FileTree", current_x, (nk_uint)target);
                        }
                        else if (node_y + row_height > visible_bottom)
                        {
                            float target = node_y + row_height - view_h + bottom_margin;
                            if (target < 0.0f)
                                target = 0.0f;
                            nk_group_set_scroll(ctx, "FileTree", current_x, (nk_uint)target);
                        }
                    }
                    scroll_to_selected = false;
                }
            }
            else
            {
                draw_file_node(ctx, g_state.browser.data_pack->GetFileTree());

                if (scroll_to_selected && g_state.browser.selected_node)
                {
                    if (const auto it = std::find(g_state.browser.visible_nodes.begin(), g_state.browser.visible_nodes.end(), g_state.browser.selected_node); it != g_state.browser.visible_nodes.end())
                    {
                        const int index = static_cast<int>(std::distance(g_state.browser.visible_nodes.begin(), it));
                        nk_uint current_x, current_y;
                        nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

                        const float row_height = 26.0f + ctx->style.window.spacing.y;
                        const float node_y = index * row_height;
                        const float view_h = nk_window_get_content_region(ctx).h;

                        const float top_margin = row_height * 2.0f;
                        const float bottom_margin = row_height * 2.0f;
                        const float visible_top = static_cast<float>(current_y) + top_margin;
                        const float visible_bottom = static_cast<float>(current_y) + view_h - bottom_margin;

                        if (node_y < visible_top)
                        {
                            float target = node_y - top_margin;
                            if (target < 0.0f)
                                target = 0.0f;
                            nk_group_set_scroll(ctx, "FileTree", current_x, static_cast<nk_uint>(target));
                        }
                        else if (node_y + row_height > visible_bottom)
                        {
                            float target = node_y + row_height - view_h + bottom_margin;
                            if (target < 0.0f)
                                target = 0.0f;
                            nk_group_set_scroll(ctx, "FileTree", current_x, static_cast<nk_uint>(target));
                        }
                    }
                    scroll_to_selected = false;
                }
            }
        }
        else if (g_state.browser.data_pack && g_state.tasks.running)
        {
            nk_layout_row_begin(ctx, NK_STATIC, 26, 4);
            nk_layout_row_push(ctx, 10.0f);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 24.0f);
            nk_style_button expand_style = ctx->style.button;
            expand_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
            expand_style.hover = nk_style_item_color(nk_rgb(60, 60, 65));
            expand_style.text_normal = nk_rgb(150, 150, 150);
            expand_style.rounding = 3.0f;

            nk_widget_disable_begin(ctx);
            nk_button_label_styled(ctx, &expand_style, "+");
            nk_widget_disable_end(ctx);

            nk_layout_row_push(ctx, 370.0f);
            nk_style_button button_style = ctx->style.button;
            button_style.normal = nk_style_item_color(nk_rgb(35, 35, 38));
            button_style.hover = nk_style_item_color(nk_rgb(35, 35, 38));
            button_style.active = nk_style_item_color(nk_rgb(35, 35, 38));
            button_style.text_normal = nk_rgb(220, 220, 220);
            button_style.text_alignment = NK_TEXT_LEFT;
            button_style.padding = nk_vec2(8, 4);
            button_style.rounding = 3.0f;
            nk_button_label_styled(ctx, &button_style, g_state.browser.data_pack->GetFileTree().name.c_str());

            nk_layout_row_push(ctx, 200.0f);
            const std::string info = "0 items | " + Core::FormatSize(0);
            nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));
            nk_layout_row_end(ctx);
        }
        else if (g_state.browser.data_pack)
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "Click 'Scan Tree' to load files...", NK_TEXT_CENTERED);
        }
        else
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "No pack file loaded.", NK_TEXT_CENTERED);
        }
        nk_group_end(ctx);
    }
}
