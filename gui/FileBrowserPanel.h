#pragma once

#include "gui/AppState.h"

void handle_node_click(const Core::FileNode *node, bool is_folder);

void set_file_tree_mode();
void set_spine_viewer_mode();
bool set_diff_viewer_mode();
void export_to_json();

void draw_file_browser_panel(nk_context *ctx, bool tree_scanned, bool &scroll_to_selected);
