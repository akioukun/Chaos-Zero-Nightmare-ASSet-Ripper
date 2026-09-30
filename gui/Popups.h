#pragma once

#include "gui/AppState.h"

void save_options_to_ini();
void load_options_from_ini();

void draw_options_popup(nk_context *ctx, int window_width, int window_height);
void draw_credits_popup(nk_context *ctx, int window_width, int window_height);
void draw_feedback_popups(nk_context *ctx, int window_width, int window_height);
void draw_context_menu(nk_context *ctx);
