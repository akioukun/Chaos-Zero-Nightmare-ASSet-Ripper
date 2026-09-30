#pragma once

#include <string>
#include "nuklear.h"
#include "core/Core.h"
#include "gui/AppState.h"

// Format detection helpers
bool is_sct_format(const std::string &ext);
bool is_db_file(const std::string &ext);
bool is_scsp_file(const std::string &ext);
bool is_previewable_format(const std::string &ext);
bool is_animated_webp(const std::string &ext);
bool is_atlas_file(const std::string &ext);
bool is_json_file(const std::string &ext);
bool is_text_file(const std::string &ext);

// Tree search helpers
bool matches_search(const Core::FileNode &node, const std::string &query);
bool has_matching_child(const Core::FileNode &node, const std::string &query);

// Tree widget styling helpers
struct nk_style_button make_tree_button_style(nk_context *ctx, bool is_selected, int depth, struct nk_color text_color);
struct nk_style_button make_tree_expand_style(nk_context *ctx);
struct nk_color get_diff_status_color(DiffStatus status, bool is_selected, bool is_folder);

// Tree row rendering helper
bool draw_tree_row(nk_context *ctx, int depth, bool is_folder, bool is_expanded,
                   bool is_selected, const std::string &label, const std::string &info,
                   struct nk_color text_color, bool &out_toggle_expand,
                   bool *out_right_clicked = nullptr, struct nk_vec2 *out_mouse_pos = nullptr);
