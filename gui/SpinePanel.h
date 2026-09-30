#pragma once

#include "gui/AppState.h"
#include <string>
#include <vector>

bool spine_category_has_search_match(const SpineCategory &cat, const std::vector<SpineEntry> &entries, const std::string &query_lower);
void draw_spine_category(nk_context *ctx, const SpineCategory &cat, const std::vector<SpineEntry> &entries, int depth);
int export_spine_atlas_and_images(const SpineEntry &entry, const std::string &dest);

void draw_spine_panel(nk_context *ctx, float content_height, int window_width);
