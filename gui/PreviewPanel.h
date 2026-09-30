#pragma once

#include "gui/AppState.h"
#include <SDL.h>
#include <string>

void clear_preview();

void load_preview(const Core::FileNode &node);

void open_image_preview_window(const Core::FileNode &node);
void render_image_window();

void export_file_as_png(const Core::FileNode &node);
void export_file_as_sct(const Core::FileNode &node);
void export_db_as_json_file(const Core::FileNode &node);
void export_scsp_as_json_file(const Core::FileNode &node);

void draw_preview_panel(nk_context *ctx, float right_width, float content_height);
