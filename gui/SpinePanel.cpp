#include "parsers/SpineRenderer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <GL/glew.h>
#include <SDL.h>

#include "core/Core.h"
#include "core/FileTree.h"
#include "core/DialogPaths.h"
#include "parsers/SCTParser.h"
#include "parsers/SCSPParser.h"
#include "parsers/SpineDictionary.h"
#include "gui/AppState.h"
#include "gui/UIHelpers.h"
#include "gui/SpinePanel.h"

#include "json.hpp"
#include "nuklear.h"

namespace {
    bool spine_category_has_search_match(const SpineCategory &cat, const std::vector<SpineEntry> &entries, const std::string &query_lower)
    {
        if (query_lower.empty())
            return true;
        for (const size_t idx : cat.entry_indices)
        {
            std::string dn = entries[idx].display_name;
            std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
            if (dn.find(query_lower) != std::string::npos)
                return true;
            std::string cp = entries[idx].category;
            std::transform(cp.begin(), cp.end(), cp.begin(), ::tolower);
            if (cp.find(query_lower) != std::string::npos)
                return true;
        }
        for (const auto &[name, sub] : cat.subcategories)
        {
            if (spine_category_has_search_match(sub, entries, query_lower))
                return true;
        }
        return false;
    }

    void draw_spine_category(nk_context *ctx, const SpineCategory &cat, const std::vector<SpineEntry> &entries, const int depth)
    {
        std::string query_lower = g_state.spine.search_query;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

        for (const auto &[subname, sub] : cat.subcategories)
        {
            if (!spine_category_has_search_match(sub, entries, query_lower))
                continue;

            bool expanded = g_state.spine.expanded_categories.count(sub.full_path) > 0;
            if (!query_lower.empty())
                expanded = true;

            std::function<int(const SpineCategory &)> count_entries = [&](const SpineCategory &c) -> int
            {
                int n = static_cast<int>(c.entry_indices.size());
                for (const auto &[k, sc] : c.subcategories)
                    n += count_entries(sc);
                return n;
            };
            int total = count_entries(sub);

            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            const float indent = depth * 16.0f;
            if (indent > 0)
            {
                nk_layout_row_push(ctx, indent);
                nk_spacing(ctx, 1);
            }

            nk_layout_row_push(ctx, 300.0f - indent);
            nk_style_button cbtn = ctx->style.button;
            cbtn.text_alignment = NK_TEXT_LEFT;
            cbtn.padding = nk_vec2(6, 3);
            cbtn.rounding = 2.0f;
            const int shade = 50 + (depth % 3) * 5;
            cbtn.normal = nk_style_item_color(nk_rgb(shade, shade + 5, shade + 15));
            cbtn.hover = nk_style_item_color(nk_rgb(shade + 10, shade + 15, shade + 25));
            cbtn.text_normal = nk_rgb(180, 200, 230);
            cbtn.text_hover = nk_rgb(220, 230, 255);

            std::string folder_label = (expanded ? "- " : "+ ") + sub.name + " (" + std::to_string(total) + ")";
            if (nk_button_label_styled(ctx, &cbtn, folder_label.c_str()))
            {
                if (expanded)
                    g_state.spine.expanded_categories.erase(sub.full_path);
                else
                    g_state.spine.expanded_categories.insert(sub.full_path);
            }
            nk_layout_row_end(ctx);

            if (!expanded)
                continue;

            for (size_t idx : sub.entry_indices)
            {
                const auto &ent = entries[idx];

                if (!query_lower.empty())
                {
                    std::string dn = ent.display_name;
                    std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
                    std::string cp = ent.category;
                    std::transform(cp.begin(), cp.end(), cp.begin(), ::tolower);
                    if (dn.find(query_lower) == std::string::npos &&
                        cp.find(query_lower) == std::string::npos)
                        continue;
                }

                g_state.spine.visible_indices.push_back(static_cast<int>(idx));

                nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
                float entry_indent = (depth + 1) * 16.0f;
                nk_layout_row_push(ctx, entry_indent);
                nk_spacing(ctx, 1);

                nk_layout_row_push(ctx, 300.0f - entry_indent);
                const bool isSel = (static_cast<int>(idx) == g_state.spine.selected_index);
                struct nk_style_button ebtn = ctx->style.button;
                ebtn.text_alignment = NK_TEXT_LEFT;
                ebtn.padding = nk_vec2(6, 3);
                ebtn.rounding = 2.0f;
                if (isSel)
                {
                    ebtn.normal = nk_style_item_color(nk_rgb(55, 80, 120));
                    ebtn.hover = nk_style_item_color(nk_rgb(65, 90, 130));
                    ebtn.text_normal = nk_rgb(255, 255, 255);
                }
                else
                {
                    ebtn.normal = nk_style_item_color(nk_rgb(38, 38, 42));
                    ebtn.hover = nk_style_item_color(nk_rgb(50, 50, 55));
                    ebtn.text_normal = nk_rgb(190, 190, 190);
                }
                ebtn.text_hover = nk_rgb(255, 255, 255);

                if (nk_button_label_styled(ctx, &ebtn, ent.display_name.c_str()))
                {
                    if (g_state.spine.selected_index != static_cast<int>(idx))
                    {
                        g_state.spine.selected_index = static_cast<int>(idx);
                        g_state.spine.selected_animation = 0;
                        g_state.spine.selected_skin = 0;
                        g_state.spine.last_tick = 0;
                        g_state.spine.edit_mode = false;
                        if (!g_state.spine.viewer)
                            g_state.spine.viewer = std::make_unique<SpineViewer>();
                        g_state.spine.viewer->loadSkeleton(g_state.spine.dictionary, *g_state.browser.data_pack, ent);
                        g_state.spine.viewer->setFlipX(g_state.spine.flip_x);
                        g_state.spine.viewer->setFlipY(g_state.spine.flip_y);
                    }
                }
                nk_layout_row_end(ctx);
            }

            draw_spine_category(ctx, sub, entries, depth + 1);
        }

        if (depth == 0)
        {
            for (const size_t idx : cat.entry_indices)
            {
                const auto &ent = entries[idx];

                if (!query_lower.empty())
                {
                    std::string dn = ent.display_name;
                    std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
                    if (dn.find(query_lower) == std::string::npos)
                        continue;
                }

                g_state.spine.visible_indices.push_back(static_cast<int>(idx));

                nk_layout_row_dynamic(ctx, 24, 1);
                const bool isSel = (static_cast<int>(idx) == g_state.spine.selected_index);
                nk_style_button ebtn = ctx->style.button;
                ebtn.text_alignment = NK_TEXT_LEFT;
                ebtn.padding = nk_vec2(16, 3);
                ebtn.rounding = 2.0f;
                if (isSel)
                {
                    ebtn.normal = nk_style_item_color(nk_rgb(55, 80, 120));
                    ebtn.hover = nk_style_item_color(nk_rgb(65, 90, 130));
                    ebtn.text_normal = nk_rgb(255, 255, 255);
                }
                else
                {
                    ebtn.normal = nk_style_item_color(nk_rgb(38, 38, 42));
                    ebtn.hover = nk_style_item_color(nk_rgb(50, 50, 55));
                    ebtn.text_normal = nk_rgb(190, 190, 190);
                }
                ebtn.text_hover = nk_rgb(255, 255, 255);

                if (nk_button_label_styled(ctx, &ebtn, ent.display_name.c_str()))
                {
                    if (g_state.spine.selected_index != static_cast<int>(idx))
                    {
                        g_state.spine.selected_index = static_cast<int>(idx);
                        g_state.spine.selected_animation = 0;
                        g_state.spine.selected_skin = 0;
                        g_state.spine.last_tick = 0;
                        g_state.spine.edit_mode = false;
                        if (!g_state.spine.viewer)
                            g_state.spine.viewer = std::make_unique<SpineViewer>();
                        g_state.spine.viewer->loadSkeleton(g_state.spine.dictionary, *g_state.browser.data_pack, ent);
                        g_state.spine.viewer->setFlipX(g_state.spine.flip_x);
                        g_state.spine.viewer->setFlipY(g_state.spine.flip_y);
                    }
                }
            }
        }
    }

    int export_spine_atlas_and_images(const SpineEntry &entry, const std::string &dest)
    {
        int exported = 0;
        if (entry.atlas_node)
        {
            std::vector<uint8_t> data = g_state.browser.data_pack->GetFileData(*entry.atlas_node);
            std::string atlas_str(data.begin(), data.end());
            size_t p = 0;
            while ((p = atlas_str.find(".sct", p)) != std::string::npos)
            {
                atlas_str.replace(p, 4, ".png");
                p += 4;
            }
            std::ofstream out(dest + "/" + entry.atlas_node->name, std::ios::binary);
            out << atlas_str;
            exported++;
        }

        g_state.spine.dictionary.EnsureDetailsLoaded(*g_state.browser.data_pack, entry);
        for (const auto *img : entry.image_nodes)
        {
            const auto &fi = std::get<Core::FileInfo>(img->data);
            std::vector<uint8_t> data = g_state.browser.data_pack->GetFileData(*img);
            if (is_sct_format(fi.format))
            {
                std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(data);
                if (!png_data.empty())
                {
                    std::string out_name = Core::ReplaceExtension(img->name, ".png");
                    std::ofstream out(dest + "/" + out_name, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(png_data.data()), png_data.size());
                    exported++;
                }
            }
            else
            {
                std::ofstream out(dest + "/" + img->name, std::ios::binary);
                out.write(reinterpret_cast<const char *>(data.data()), data.size());
                exported++;
            }
        }
        return exported;
    }
}

void draw_spine_panel(nk_context *ctx, float content_height, int window_width)
{
    if (g_state.spine.viewer && g_state.spine.viewer->isLoaded())
    {
        float dt = 0;
        if (g_state.spine.playing)
        {
            Uint64 now = SDL_GetPerformanceCounter();
            if (g_state.spine.last_tick > 0)
            {
                dt = static_cast<float>(now - g_state.spine.last_tick) / static_cast<float>(SDL_GetPerformanceFrequency());
                dt *= g_state.spine.speed;
            }
            g_state.spine.last_tick = now;
        }
        else
        {
            g_state.spine.last_tick = 0;
        }
        g_state.spine.viewer->update(dt);
    }

    if (g_state.spine.building)
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, "Building Spine dictionary...", NK_TEXT_CENTERED);
    }
    else if (g_state.spine.dictionary.IsBuilt())
    {
        const auto &spine_entries_inline = g_state.spine.dictionary.GetEntries();
        const auto &root_cat = g_state.spine.dictionary.GetRootCategory();

        nk_layout_row_begin(ctx, NK_STATIC, 28, 3);
        nk_layout_row_push(ctx, 60);
        nk_label(ctx, "Search:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 250);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, g_state.spine.search_buffer, sizeof(g_state.spine.search_buffer), nk_filter_default);
        g_state.spine.search_query = g_state.spine.search_buffer;
        nk_layout_row_push(ctx, 200);
        std::string sstats = std::to_string(spine_entries_inline.size()) + " skeletons";
        nk_label_colored(ctx, sstats.c_str(), NK_TEXT_LEFT, nk_rgb(150, 200, 255));
        nk_layout_row_end(ctx);

        auto sw = static_cast<float>(window_width);
        float sh = content_height - 30.0f;
        float iListW = sw * 0.22f;
        float iEditorW = g_state.spine.edit_mode ? sw * 0.30f : 0;
        float iViewerW = sw - iListW - iEditorW - 50.0f;

        nk_layout_row_begin(ctx, NK_STATIC, sh, g_state.spine.edit_mode ? 3 : 2);

        g_state.spine.visible_indices.clear();
        nk_layout_row_push(ctx, iListW);
        if (nk_group_begin(ctx, "SpineListInline", NK_WINDOW_BORDER))
        {
            draw_spine_category(ctx, root_cat, spine_entries_inline, 0);
            nk_group_end(ctx);
        }

        nk_layout_row_push(ctx, iViewerW);
        if (nk_group_begin(ctx, "SpineViewInline", NK_WINDOW_BORDER))
        {
            if (g_state.spine.viewer && g_state.spine.viewer->isLoaded())
            {
                nk_layout_row_begin(ctx, NK_STATIC, 28, 8);

                nk_layout_row_push(ctx, 50);
                nk_label(ctx, "Anim:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 160);
                auto anim_names = g_state.spine.viewer->getAnimationNames();
                if (!anim_names.empty())
                {
                    g_state.spine.selected_animation = g_state.spine.viewer->getCurrentAnimIndex();
                    if (g_state.spine.selected_animation >= static_cast<int>(anim_names.size()))
                        g_state.spine.selected_animation = 0;
                    if (nk_combo_begin_label(ctx, anim_names[g_state.spine.selected_animation].c_str(), nk_vec2(200, 300)))
                    {
                        nk_layout_row_dynamic(ctx, 22, 1);
                        for (int a = 0; a < static_cast<int>(anim_names.size()); a++)
                        {
                            if (nk_combo_item_label(ctx, anim_names[a].c_str(), NK_TEXT_LEFT))
                            {
                                if (a != g_state.spine.selected_animation)
                                {
                                    g_state.spine.selected_animation = a;
                                    g_state.spine.viewer->setAnimation(anim_names[a], true);
                                }
                            }
                        }
                        nk_combo_end(ctx);
                    }
                }

                nk_layout_row_push(ctx, 45);
                nk_label(ctx, "Skin:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 120);
                if (auto skin_names = g_state.spine.viewer->getSkinNames(); !skin_names.empty())
                {
                    if (g_state.spine.selected_skin >= static_cast<int>(skin_names.size()))
                        g_state.spine.selected_skin = 0;
                    if (nk_combo_begin_label(ctx, skin_names[g_state.spine.selected_skin].c_str(), nk_vec2(160, 300)))
                    {
                        nk_layout_row_dynamic(ctx, 22, 1);
                        for (int s = 0; s < static_cast<int>(skin_names.size()); s++)
                        {
                            if (nk_combo_item_label(ctx, skin_names[s].c_str(), NK_TEXT_LEFT))
                            {
                                if (s != g_state.spine.selected_skin)
                                {
                                    g_state.spine.selected_skin = s;
                                    g_state.spine.viewer->setSkin(skin_names[s]);
                                }
                            }
                        }
                        nk_combo_end(ctx);
                    }
                }

                nk_layout_row_push(ctx, 60);
                if (nk_button_label(ctx, g_state.spine.playing ? "Pause" : "Play"))
                {
                    g_state.spine.playing = !g_state.spine.playing;
                    g_state.spine.viewer->setPlaying(g_state.spine.playing);
                    if (g_state.spine.playing)
                        g_state.spine.last_tick = SDL_GetPerformanceCounter();
                }

                nk_layout_row_end(ctx);

                nk_layout_row_begin(ctx, NK_STATIC, 28, 13);

                nk_layout_row_push(ctx, 50);
                nk_label(ctx, "Speed:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 120);
                nk_slider_float(ctx, 0.1f, &g_state.spine.speed, 3.0f, 0.1f);
                nk_layout_row_push(ctx, 40);
                char speed_label[16];
                snprintf(speed_label, sizeof(speed_label), "%.1fx", g_state.spine.speed);
                nk_label(ctx, speed_label, NK_TEXT_LEFT);

                nk_layout_row_push(ctx, 45);
                nk_label(ctx, "Zoom:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 100);
                g_state.spine.zoom = g_state.spine.viewer->getZoom();
                nk_slider_float(ctx, 0.1f, &g_state.spine.zoom, 5.0f, 0.1f);
                nk_layout_row_push(ctx, 40);
                char zoom_label[16];
                snprintf(zoom_label, sizeof(zoom_label), "%.1fx", g_state.spine.zoom);
                nk_label(ctx, zoom_label, NK_TEXT_LEFT);
                g_state.spine.viewer->setZoom(g_state.spine.zoom);

                nk_layout_row_push(ctx, 60);
                {
                    nk_style_button flip_style = ctx->style.button;
                    flip_style.rounding = 3.0f;
                    if (g_state.spine.flip_x)
                    {
                        flip_style.normal = nk_style_item_color(nk_rgb(56, 120, 74));
                        flip_style.hover = nk_style_item_color(nk_rgb(66, 138, 86));
                    }
                    else
                    {
                        flip_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                        flip_style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                    }
                    flip_style.text_normal = nk_rgb(220, 220, 220);
                    flip_style.text_hover = nk_rgb(255, 255, 255);
                    if (nk_button_label_styled(ctx, &flip_style, "Flip X"))
                    {
                        g_state.spine.flip_x = !g_state.spine.flip_x;
                        g_state.spine.viewer->setFlipX(g_state.spine.flip_x);
                    }
                }

                nk_layout_row_push(ctx, 60);
                {
                    nk_style_button flip_style = ctx->style.button;
                    flip_style.rounding = 3.0f;
                    if (g_state.spine.flip_y)
                    {
                        flip_style.normal = nk_style_item_color(nk_rgb(56, 120, 74));
                        flip_style.hover = nk_style_item_color(nk_rgb(66, 138, 86));
                    }
                    else
                    {
                        flip_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                        flip_style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                    }
                    flip_style.text_normal = nk_rgb(220, 220, 220);
                    flip_style.text_hover = nk_rgb(255, 255, 255);
                    if (nk_button_label_styled(ctx, &flip_style, "Flip Y"))
                    {
                        g_state.spine.flip_y = !g_state.spine.flip_y;
                        g_state.spine.viewer->setFlipY(g_state.spine.flip_y);
                    }
                }

                nk_layout_row_push(ctx, 45);
                {
                    nk_style_button edit_style = ctx->style.button;
                    edit_style.rounding = 3.0f;
                    if (g_state.spine.edit_mode)
                    {
                        edit_style.normal = nk_style_item_color(nk_rgb(120, 80, 40));
                        edit_style.hover = nk_style_item_color(nk_rgb(140, 95, 50));
                    }
                    else
                    {
                        edit_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                        edit_style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                    }
                    edit_style.text_normal = nk_rgb(220, 220, 220);
                    edit_style.text_hover = nk_rgb(255, 255, 255);
                    if (nk_button_label_styled(ctx, &edit_style, "Edit"))
                    {
                        g_state.spine.edit_mode = !g_state.spine.edit_mode;
                    }
                }

                nk_layout_row_push(ctx, 55);
                if (nk_button_label(ctx, "Reset"))
                {
                    g_state.spine.zoom = 1.0f;
                    g_state.spine.viewer->resetView();
                }

                nk_layout_row_push(ctx, 100);
                if (g_state.spine.selected_index >= 0 && g_state.spine.selected_index < static_cast<int>(spine_entries_inline.size()))
                {
                    if (nk_button_label(ctx, "Export All"))
                    {
                        const auto &entry = spine_entries_inline[g_state.spine.selected_index];
                        try
                        {
                            const std::string dest = DialogPaths::SelectFolder(DialogPaths::Slot::Extract, "Select destination folder");
                            if (!dest.empty())
                            {
                                int exported = 0;
                                {
                                    std::vector<uint8_t> data = g_state.browser.data_pack->GetFileData(*entry.scsp_node);
                                    std::string json_str = SCSPParser::ConvertToJson(data);
                                    if (!json_str.empty())
                                    {
                                        try
                                        {
                                            nlohmann::json parsed = nlohmann::json::parse(json_str);
                                            json_str = parsed.dump(2);
                                        }
                                        catch (...)
                                        {
                                        }
                                        std::ofstream out(dest + "/" + entry.display_name + ".json");
                                        out << json_str;
                                        exported++;
                                    }
                                }
                                exported += export_spine_atlas_and_images(entry, dest);
                                g_state.tasks.status = "Exported " + std::to_string(exported) + " files for '" + entry.display_name + "'";
                            }
                        }
                        catch (const std::exception &e)
                        {
                            g_state.tasks.status = "Export error: " + std::string(e.what());
                        }
                    }
                }

                nk_layout_row_end(ctx);

                nk_layout_row_begin(ctx, NK_STATIC, 24, 7);

                nk_layout_row_push(ctx, 80);
                {
                    nk_style_button ab = ctx->style.button;
                    ab.rounding = 3.0f;
                    ab.normal = nk_style_item_color(g_state.spine.autoplay ? nk_rgb(56, 120, 74) : nk_rgb(60, 60, 65));
                    ab.hover = nk_style_item_color(g_state.spine.autoplay ? nk_rgb(66, 138, 86) : nk_rgb(75, 75, 80));
                    ab.text_normal = nk_rgb(220, 220, 220);
                    if (nk_button_label_styled(ctx, &ab, g_state.spine.autoplay ? "Auto: ON" : "Auto: OFF"))
                    {
                        g_state.spine.autoplay = !g_state.spine.autoplay;
                        g_state.spine.viewer->setAutoplayNext(g_state.spine.autoplay);
                    }
                }

                nk_layout_row_push(ctx, 50);
                if (nk_button_label(ctx, "Next"))
                {
                    g_state.spine.viewer->nextAnimation();
                    g_state.spine.selected_animation = g_state.spine.viewer->getCurrentAnimIndex();
                }

                nk_layout_row_push(ctx, 20);
                nk_spacing(ctx, 1);

                nk_layout_row_push(ctx, 80);
                {
                    nk_style_button pb = ctx->style.button;
                    pb.rounding = 3.0f;
                    pb.normal = nk_style_item_color(g_state.spine.pma_blend ? nk_rgb(70, 90, 120) : nk_rgb(60, 60, 65));
                    pb.hover = nk_style_item_color(nk_rgb(80, 100, 130));
                    pb.text_normal = nk_rgb(200, 200, 200);
                    if (nk_button_label_styled(ctx, &pb, g_state.spine.pma_blend ? "PMA: ON" : "PMA: OFF"))
                    {
                        g_state.spine.pma_blend = !g_state.spine.pma_blend;
                        g_state.spine.viewer->setUsePMA(g_state.spine.pma_blend);
                        g_state.spine.viewer->setPremultiplyTextures(false);
                        if (g_state.spine.selected_index >= 0 && g_state.spine.selected_index < static_cast<int>(spine_entries_inline.size()))
                        {
                            g_state.spine.viewer->loadSkeleton(g_state.spine.dictionary, *g_state.browser.data_pack, spine_entries_inline[g_state.spine.selected_index]);
                        }
                    }
                }

                nk_layout_row_push(ctx, 10);
                nk_spacing(ctx, 1);

                nk_layout_row_push(ctx, 80);
                {
                    nk_style_button bb = ctx->style.button;
                    bb.rounding = 3.0f;
                    bb.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                    bb.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                    bb.text_normal = nk_rgb(200, 200, 200);
                    if (const char *bg_labels[] = {"BG: None", "BG: Dark", "BG: Gray", "BG: White"}; nk_button_label_styled(ctx, &bb, bg_labels[g_state.spine.bg_preset]))
                    {
                        constexpr float bg_colors[][3] = {
                            {0, 0, 0}, {0.12f, 0.12f, 0.14f}, {0.35f, 0.35f, 0.38f}, {1.0f, 1.0f, 1.0f}};
                        g_state.spine.bg_preset = (g_state.spine.bg_preset + 1) % 4;
                        g_state.spine.viewer->setBgColor(
                            bg_colors[g_state.spine.bg_preset][0],
                            bg_colors[g_state.spine.bg_preset][1],
                            bg_colors[g_state.spine.bg_preset][2]);
                    }
                }

                nk_layout_row_end(ctx);

                float vpH = sh - 120.0f;
                if (vpH < 100)
                    vpH = 100;
                nk_layout_row_dynamic(ctx, vpH, 1);
                struct nk_rect vb = nk_widget_bounds(ctx);
                int vw = static_cast<int>(vb.w), vh = static_cast<int>(vb.h);

                {
                    nk_input *inp = &ctx->input;
                    if (bool popup_active = (ctx->current && ctx->current->popup.win); !popup_active && nk_input_is_mouse_hovering_rect(inp, vb))
                    {
                        float scr = inp->mouse.scroll_delta.y;
                        float mdx = inp->mouse.delta.x, mdy = inp->mouse.delta.y;
                        Uint32 km = SDL_GetModState();

                        if (scr != 0 && !(km & KMOD_CTRL))
                        {
                            g_state.spine.viewer->zoomBy(scr > 0 ? 1.15f : 1.0f / 1.15f);
                            g_state.spine.zoom = g_state.spine.viewer->getZoom();
                        }
                        if (nk_input_is_mouse_down(inp, NK_BUTTON_MIDDLE) ||
                            nk_input_is_mouse_down(inp, NK_BUTTON_RIGHT) ||
                            (nk_input_is_mouse_down(inp, NK_BUTTON_LEFT) && (!g_state.spine.edit_mode || (km & KMOD_SHIFT))))
                        {
                            if (mdx != 0 || mdy != 0)
                            {
                                float s = g_state.spine.viewer->getZoom() > 0 ? static_cast<float>(vw) / g_state.spine.viewer->getZoom() / vw : 1;
                                g_state.spine.viewer->pan(mdx * s, -mdy * s);
                            }
                        }
                        if (g_state.spine.edit_mode && !g_state.spine.selected_bone.empty() && scr != 0 && (km & KMOD_CTRL))
                        {
                            auto bl = g_state.spine.viewer->getBoneList();
                            for (auto &b : bl)
                            {
                                if (b.name == g_state.spine.selected_bone)
                                {
                                    BoneOverride o;
                                    o.x = b.x;
                                    o.y = b.y;
                                    o.rotation = b.rotation;
                                    o.scaleX = b.scaleX + scr * 0.05f;
                                    o.scaleY = b.scaleY + scr * 0.05f;
                                    o.shearX = b.shearX;
                                    o.shearY = b.shearY;
                                    g_state.spine.viewer->setBoneOverride(b.name, o);
                                    break;
                                }
                            }
                        }
                        if (g_state.spine.edit_mode)
                        {
                            auto ag = static_cast<SpineViewer::GizmoHandle>(g_state.spine.active_gizmo);
                            float lx = inp->mouse.pos.x - vb.x, ly = inp->mouse.pos.y - vb.y;
                            if (nk_input_is_mouse_pressed(inp, NK_BUTTON_LEFT) && !(km & KMOD_SHIFT))
                            {
                                ag = g_state.spine.viewer->hitTestGizmo(lx, ly, vw, vh);
                                if (ag == SpineViewer::GizmoHandle::None)
                                {
                                    g_state.spine.selected_bone = g_state.spine.viewer->hitTestBone(lx, ly, vw, vh);
                                    if (!g_state.spine.selected_bone.empty())
                                    {
                                        ag = SpineViewer::GizmoHandle::Move;
                                        g_state.spine.scroll_to_bone = true;
                                    }
                                }
                            }
                            if (!nk_input_is_mouse_down(inp, NK_BUTTON_LEFT))
                                ag = SpineViewer::GizmoHandle::None;
                            if (ag != SpineViewer::GizmoHandle::None && !g_state.spine.selected_bone.empty() && nk_input_is_mouse_down(inp, NK_BUTTON_LEFT) && (mdx != 0 || mdy != 0))
                            {
                                float s = g_state.spine.viewer->getZoom() > 0 ? static_cast<float>(vw) / g_state.spine.viewer->getZoom() / vw : 1;
                                auto bl = g_state.spine.viewer->getBoneList();
                                for (auto &b : bl)
                                {
                                    if (b.name != g_state.spine.selected_bone)
                                        continue;
                                    BoneOverride o;
                                    o.x = b.x;
                                    o.y = b.y;
                                    o.rotation = b.rotation;
                                    o.scaleX = b.scaleX;
                                    o.scaleY = b.scaleY;
                                    o.shearX = b.shearX;
                                    o.shearY = b.shearY;
                                    if (ag == SpineViewer::GizmoHandle::Move)
                                    {
                                        o.x += mdx * s;
                                        o.y -= mdy * s;
                                    }
                                    else if (ag == SpineViewer::GizmoHandle::Rotate)
                                    {
                                        o.rotation += mdx * 0.5f;
                                    }
                                    else
                                    {
                                        o.scaleX += mdx * 0.005f;
                                        o.scaleY -= mdy * 0.005f;
                                    }
                                    g_state.spine.viewer->setBoneOverride(b.name, o);
                                    break;
                                }
                            }
                            g_state.spine.active_gizmo = static_cast<int>(ag);
                        }
                    }
                }

                if (vw > 0 && vh > 0)
                {
                    g_state.spine.viewer->render(vw, vh);
                    GLuint ft = g_state.spine.viewer->getFBOTexture();
                    if (ft)
                    {
                        struct nk_image fimg = nk_image_id(static_cast<int>(ft));
                        nk_draw_image(nk_window_get_canvas(ctx), vb, &fimg, nk_rgb(255, 255, 255));
                    }
                }
            }
            else if (g_state.spine.viewer && !g_state.spine.viewer->getError().empty())
            {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label_colored(ctx, "Error:", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, g_state.spine.viewer->getError().c_str(), NK_TEXT_CENTERED);
            }
            else
            {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, "Select a skeleton from the list", NK_TEXT_CENTERED);
            }
            nk_group_end(ctx);
        }

        if (g_state.spine.edit_mode && g_state.spine.viewer && g_state.spine.viewer->isLoaded())
        {
            nk_layout_row_push(ctx, iEditorW);
            if (nk_group_begin(ctx, "BoneEditor", NK_WINDOW_BORDER))
            {
                nk_layout_row_dynamic(ctx, 24, 3);
                if (nk_button_label(ctx, "Reset All"))
                {
                    g_state.spine.viewer->resetBoneEdits();
                    g_state.spine.selected_bone = "";
                }

                if (nk_button_label(ctx, "Export Modified"))
                {
                    g_state.spine.export_pending = true;
                }

                auto texList = g_state.spine.viewer->getTextureList();
                if (texList.empty())
                {
                    nk_spacing(ctx, 1);
                }

                if (g_state.spine.export_pending)
                {
                    g_state.spine.export_pending = false;
                    try
                    {
                        const std::string dest = DialogPaths::SelectFolder(DialogPaths::Slot::Extract, "Select destination folder");
                        if (!dest.empty() && g_state.spine.selected_index >= 0)
                        {
                            const auto &entry = spine_entries_inline[g_state.spine.selected_index];
                            int exported = 0;
                            std::string modJson = g_state.spine.viewer->getModifiedSkeletonJson();
                            if (!modJson.empty())
                            {
                                std::ofstream out(dest + "/" + entry.display_name + "_modified.json");
                                out << modJson;
                                exported++;
                            }
                            exported += export_spine_atlas_and_images(entry, dest);
                            g_state.tasks.status = "Exported " + std::to_string(exported) + " modified files";
                        }
                    }
                    catch (...)
                    {
                    }
                }

                nk_layout_row_begin(ctx, NK_STATIC, 20, 2);
                nk_layout_row_push(ctx, 50);
                nk_label(ctx, "Filter:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, iEditorW - 70);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, g_state.spine.bone_search_buf, sizeof(g_state.spine.bone_search_buf), nk_filter_default);
                nk_layout_row_end(ctx);
                std::string bone_query = g_state.spine.bone_search_buf;
                std::transform(bone_query.begin(), bone_query.end(), bone_query.begin(), ::tolower);

                nk_layout_row_dynamic(ctx, sh - 105, 1);
                nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(2, 0));
                nk_style_push_vec2(ctx, &ctx->style.window.group_padding, nk_vec2(2, 2));
                if (nk_group_begin(ctx, "BoneList", NK_WINDOW_BORDER))
                {
                    auto bones = g_state.spine.viewer->getBoneList();
                    float scroll_target_y = -1;

                    std::unordered_set<std::string> has_children;
                    for (auto &b : bones)
                    {
                        if (!b.parentName.empty())
                            has_children.insert(b.parentName);
                    }

                    for (size_t bi_idx = 0; bi_idx < bones.size(); bi_idx++)
                    {
                        auto &bi = bones[bi_idx];

                        if (!bone_query.empty())
                        {
                            std::string lower_name = bi.name;
                            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                            if (lower_name.find(bone_query) == std::string::npos)
                                continue;
                        }

                        if (bone_query.empty())
                        {
                            bool ancestor_collapsed = false;
                            std::string check = bi.parentName;
                            int safety = 0;
                            while (!check.empty() && safety++ < 20)
                            {
                                if (g_state.spine.collapsed_bones.count(check))
                                {
                                    ancestor_collapsed = true;
                                    break;
                                }
                                bool found = false;
                                for (auto &pb : bones)
                                {
                                    if (pb.name == check)
                                    {
                                        check = pb.parentName;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found)
                                    break;
                            }
                            if (ancestor_collapsed)
                                continue;
                        }

                        bool is_sel = (bi.name == g_state.spine.selected_bone);
                        bool is_parent = has_children.count(bi.name) > 0;
                        bool is_collapsed = g_state.spine.collapsed_bones.count(bi.name) > 0;

                        if (float indent_px = bi.depth * 10.0f; indent_px > 0)
                        {
                            int cols = 4;
                            nk_layout_row_begin(ctx, NK_STATIC, 16, cols);
                            nk_layout_row_push(ctx, indent_px);
                            struct nk_rect sp_bounds = nk_widget_bounds(ctx);
                            nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                            float line_x = sp_bounds.x + indent_px - 6;
                            nk_stroke_line(canvas, line_x, sp_bounds.y, line_x, sp_bounds.y + sp_bounds.h, 1.0f, nk_rgb(60, 65, 75));
                            nk_stroke_line(canvas, line_x, sp_bounds.y + sp_bounds.h * 0.5f, sp_bounds.x + indent_px, sp_bounds.y + sp_bounds.h * 0.5f, 1.0f, nk_rgb(60, 65, 75));
                            nk_spacing(ctx, 1);
                            float remaining = iEditorW - indent_px - 60;
                            nk_layout_row_push(ctx, remaining > 40 ? remaining : 40);
                        }
                        else
                        {
                            nk_layout_row_begin(ctx, NK_STATIC, 16, 3);
                            float remaining = iEditorW - 60;
                            nk_layout_row_push(ctx, remaining > 40 ? remaining : 40);
                        }

                        if (is_sel && g_state.spine.scroll_to_bone)
                        {
                            struct nk_rect wb = nk_widget_bounds(ctx);
                            scroll_target_y = wb.y;
                        }

                        struct nk_style_button bone_btn = ctx->style.button;
                        bone_btn.text_alignment = NK_TEXT_LEFT;
                        bone_btn.padding = nk_vec2(3, 0);
                        bone_btn.rounding = 1.0f;
                        bone_btn.border = 0;
                        bone_btn.normal = nk_style_item_color(is_sel ? nk_rgb(50, 70, 110) : nk_rgb(35, 35, 40));
                        bone_btn.hover = nk_style_item_color(nk_rgb(55, 65, 80));
                        bone_btn.active = bone_btn.hover;
                        bone_btn.text_normal = bi.hidden        ? nk_rgb(100, 100, 100)
                                               : bi.hasOverride ? nk_rgb(255, 200, 80)
                                               : is_sel         ? nk_rgb(100, 200, 255)
                                                                : nk_rgb(180, 180, 180);
                        bone_btn.text_hover = nk_rgb(255, 255, 255);
                        std::string bone_label = bi.name;
                        if (is_parent)
                            bone_label = (is_collapsed ? "+ " : "- ") + bone_label;
                        if (nk_button_label_styled(ctx, &bone_btn, bone_label.c_str()))
                        {
                            if (is_parent && bi.name == g_state.spine.selected_bone)
                            {
                                if (is_collapsed)
                                    g_state.spine.collapsed_bones.erase(bi.name);
                                else
                                    g_state.spine.collapsed_bones.insert(bi.name);
                            }
                            g_state.spine.selected_bone = bi.name;
                            g_state.spine.viewer->selectedBoneIndex = static_cast<int>(bi_idx);
                            g_state.spine.bone_just_reset = false;
                        }

                        nk_layout_row_push(ctx, 24);
                        {
                            struct nk_style_button hb = ctx->style.button;
                            hb.rounding = 1.0f;
                            hb.border = 0;
                            hb.padding = nk_vec2(0, 0);
                            hb.normal = nk_style_item_color(bi.hidden ? nk_rgb(120, 50, 50) : nk_rgb(45, 45, 50));
                            hb.hover = nk_style_item_color(nk_rgb(80, 60, 60));
                            hb.text_normal = nk_rgb(200, 200, 200);
                            if (nk_button_label_styled(ctx, &hb, bi.hidden ? "H" : "V"))
                            {
                                g_state.spine.viewer->toggleBoneHidden(bi.name);
                            }
                        }

                        nk_layout_row_push(ctx, 24);
                        {
                            struct nk_style_button rb = ctx->style.button;
                            rb.padding = nk_vec2(0, 0);
                            rb.border = 0;
                            rb.rounding = 1.0f;
                            if (nk_button_label_styled(ctx, &rb, "R"))
                            {
                                g_state.spine.viewer->resetBone(bi.name);
                                g_state.spine.bone_just_reset = true;
                            }
                        }
                        nk_layout_row_end(ctx);

                        if (is_sel)
                        {
                            if (!bi.hasOverride && !g_state.spine.bone_just_reset)
                            {
                                BoneOverride ovr;
                                ovr.x = bi.setupX;
                                ovr.y = bi.setupY;
                                ovr.rotation = bi.setupRot;
                                ovr.scaleX = bi.setupSX;
                                ovr.scaleY = bi.setupSY;
                                ovr.shearX = bi.setupShX;
                                ovr.shearY = bi.setupShY;
                                g_state.spine.viewer->setBoneOverride(bi.name, ovr);
                            }
                            if (bi.hasOverride)
                                g_state.spine.bone_just_reset = false;

                            const char *labels[] = {"X", "Y", "Rot", "SclX", "SclY", "ShrX", "ShrY"};
                            float anim[7] = {bi.animX, bi.animY, bi.animRot, bi.animSX, bi.animSY, bi.animShX, bi.animShY};
                            float setup[7] = {bi.setupX, bi.setupY, bi.setupRot, bi.setupSX, bi.setupSY, bi.setupShX, bi.setupShY};

                            nk_layout_row_dynamic(ctx, 16, 2);
                            nk_label_colored(ctx, "Editing:", NK_TEXT_LEFT, nk_rgb(255, 200, 80));
                            {
                                struct nk_style_button lb = ctx->style.button;
                                lb.rounding = 1.0f;
                                lb.padding = nk_vec2(2, 0);
                                lb.border = 0;
                                lb.normal = nk_style_item_color(g_state.spine.link_scale ? nk_rgb(56, 120, 74) : nk_rgb(60, 60, 65));
                                lb.hover = nk_style_item_color(g_state.spine.link_scale ? nk_rgb(66, 138, 86) : nk_rgb(75, 75, 80));
                                lb.text_normal = nk_rgb(200, 200, 200);
                                if (nk_button_label_styled(ctx, &lb, g_state.spine.link_scale ? "Scale: Linked" : "Scale: Free"))
                                {
                                    g_state.spine.link_scale = !g_state.spine.link_scale;
                                }
                            }

                            float vals[7] = {bi.x, bi.y, bi.rotation, bi.scaleX, bi.scaleY, bi.shearX, bi.shearY};
                            float oldSclX = vals[3], oldSclY = vals[4];
                            for (int f = 0; f < 7; f++)
                            {
                                float pxStep[7] = {0.5f, 0.5f, 0.5f, 0.01f, 0.01f, 0.1f, 0.1f};
                                float steps[7] = {1.0f, 1.0f, 1.0f, 0.05f, 0.05f, 0.5f, 0.5f};
                                float range = fmaxf(fmaxf(fabsf(vals[f]), fabsf(setup[f])) * 3.0f, 10.0f);
                                nk_layout_row_dynamic(ctx, 18, 1);
                                vals[f] = nk_propertyf(ctx, labels[f], -range, vals[f], range, steps[f], pxStep[f]);
                            }

                            if (g_state.spine.link_scale)
                            {
                                float dsx = vals[3] - oldSclX, dsy = vals[4] - oldSclY;
                                if (dsx != 0 && dsy == 0)
                                    vals[4] += dsx;
                                if (dsy != 0 && dsx == 0)
                                    vals[3] += dsy;
                            }

                            BoneOverride ovr;
                            ovr.x = vals[0];
                            ovr.y = vals[1];
                            ovr.rotation = vals[2];
                            ovr.scaleX = vals[3];
                            ovr.scaleY = vals[4];
                            ovr.shearX = vals[5];
                            ovr.shearY = vals[6];
                            g_state.spine.viewer->setBoneOverride(bi.name, ovr);

                            nk_layout_row_dynamic(ctx, 13, 1);
                            nk_label_colored(ctx, "Animated:", NK_TEXT_LEFT, nk_rgb(100, 180, 255));
                            nk_layout_row_dynamic(ctx, 13, 4);
                            for (int f = 0; f < 7; f++)
                            {
                                char abuf[24];
                                snprintf(abuf, sizeof(abuf), "%s:%.1f", labels[f], anim[f]);
                                nk_label_colored(ctx, abuf, NK_TEXT_LEFT, nk_rgb(80, 150, 220));
                                if (f == 3)
                                {
                                    nk_layout_row_dynamic(ctx, 13, 4);
                                }
                            }

                            nk_layout_row_dynamic(ctx, 13, 1);
                            nk_label_colored(ctx, "Setup:", NK_TEXT_LEFT, nk_rgb(100, 200, 100));
                            nk_layout_row_dynamic(ctx, 13, 4);
                            for (int f = 0; f < 7; f++)
                            {
                                char sbuf[24];
                                snprintf(sbuf, sizeof(sbuf), "%s:%.1f", labels[f], setup[f]);
                                nk_label_colored(ctx, sbuf, NK_TEXT_LEFT, nk_rgb(80, 170, 80));
                                if (f == 3)
                                {
                                    nk_layout_row_dynamic(ctx, 13, 4);
                                }
                            }

                            nk_layout_row_dynamic(ctx, 2, 1);
                            nk_spacing(ctx, 1);
                        }
                    }

                    if (g_state.spine.scroll_to_bone && scroll_target_y >= 0)
                    {
                        nk_uint scx, scy;
                        nk_group_get_scroll(ctx, "BoneList", &scx, &scy);
                        struct nk_rect content = nk_window_get_content_region(ctx);
                        float rel_y = scroll_target_y - content.y + static_cast<float>(scy);
                        float new_scroll = rel_y - content.h * 0.3f;
                        if (new_scroll < 0)
                            new_scroll = 0;
                        nk_group_set_scroll(ctx, "BoneList", scx, static_cast<nk_uint>(new_scroll));
                    }
                    g_state.spine.scroll_to_bone = false;

                    nk_group_end(ctx);
                }
                nk_style_pop_vec2(ctx);
                nk_style_pop_vec2(ctx);

                nk_group_end(ctx);
            }
        }

        nk_layout_row_end(ctx);
    }
}
