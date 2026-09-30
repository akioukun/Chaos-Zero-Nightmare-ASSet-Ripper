#pragma once

#include "gui/AppState.h"

// Node click handling
void handle_node_click(const Core::FileNode *node, bool is_folder);
void handle_node_right_click(const Core::FileNode *node, struct nk_vec2 pos);
void handle_diff_node_click(const DiffNode *node, bool is_folder);

// Diff mode transitions and helpers
void set_file_tree_mode();
void set_spine_viewer_mode();
bool activate_diff_viewer();
bool load_diff_tree_from_filemap();
void export_to_json();

// Tree node rendering
void draw_file_node(nk_context *ctx, const Core::FileNode &node, int depth = 0);
void draw_diff_node(nk_context *ctx, const DiffNode &node, int depth = 0);

// File browser panel container widget (tree, status, and auto-scroll)
void draw_file_browser_panel(nk_context *ctx, float left_width, bool tree_scanned, bool &scroll_to_selected);
