#pragma once

#include "gui/AppState.h"
#include <SDL.h>
#include <string>

// Preview lifecycle
void clear_preview();

// Image surface loading (with SCT conversion)
SDL_Surface *load_surface_from_node(const Core::FileNode &node, std::string &out_error);

// Preview loaders
void load_preview(const Core::FileNode &node);
inline void load_image_preview(const Core::FileNode &node) { load_preview(node); }

void load_json_preview(const Core::FileNode &node, const std::string &content = "");
void load_db_preview(const Core::FileNode &node);
void load_scsp_preview(const Core::FileNode &node);
void load_text_preview(const Core::FileNode &node);

// Detached preview window
void open_image_preview_window(const Core::FileNode &node);
void render_image_window();

// Single-node export actions
void export_file_as_png(const Core::FileNode &node);
void export_file_as_sct(const Core::FileNode &node);
void export_db_as_json_file(const Core::FileNode &node);
void export_scsp_as_json_file(const Core::FileNode &node);
void export_json_file(const Core::FileNode &node);

// Preview panel widget rendering
void draw_preview_panel(struct nk_context *ctx, float right_width, float content_height);
