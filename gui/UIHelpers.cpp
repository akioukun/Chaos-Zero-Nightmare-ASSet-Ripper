#include "gui/UIHelpers.h"
#include <algorithm>

bool is_sct_format(const std::string &ext)
{
    const auto e = Core::ToLower(ext);
    return e == ".sct" || e == ".sct2";
}

bool is_db_file(const std::string &ext)
{
    return Core::ToLower(ext) == ".db";
}

bool is_scsp_file(const std::string &ext)
{
    return Core::ToLower(ext) == ".scsp";
}

bool is_previewable_format(const std::string &ext)
{
    const auto e = Core::ToLower(ext);
    return e == ".png" || e == ".jpg" || e == ".jpeg" ||
           e == ".bmp" || e == ".tga" || is_sct_format(e);
}

bool is_animated_webp(const std::string &ext)
{
    return Core::ToLower(ext) == ".webp";
}

bool is_atlas_file(const std::string &ext)
{
    return Core::ToLower(ext) == ".atlas";
}

bool is_json_file(const std::string &ext)
{
    return Core::ToLower(ext) == ".json";
}

bool is_text_file(const std::string &ext)
{
    const auto e = Core::ToLower(ext);
    return e == ".txt" || is_atlas_file(e);
}

bool matches_search(const Core::FileNode &node, const std::string &query)
{
    if (query.empty())
        return true;

    const std::string name_lower = Core::ToLower(node.name);
    const std::string query_lower = Core::ToLower(query);
    return name_lower.find(query_lower) != std::string::npos;
}

bool has_matching_child(const Core::FileNode &node, const std::string &query)
{
    if (query.empty())
        return true;

    if (matches_search(node, query))
        return true;

    if (std::holds_alternative<Core::FolderInfo>(node.data))
    {
        const auto &folder = std::get<Core::FolderInfo>(node.data);
        for (const auto &child : folder.children)
        {
            if (has_matching_child(child, query))
                return true;
        }
    }

    return false;
}

struct nk_style_button make_tree_button_style(nk_context *ctx, bool is_selected, int depth, struct nk_color text_color)
{
    struct nk_color bg_color = (depth % 2 == 0) ? nk_rgb(35, 35, 38) : nk_rgb(40, 40, 43);
    if (is_selected)
        bg_color = nk_rgb(65, 65, 70);

    struct nk_style_button style = ctx->style.button;
    style.normal = nk_style_item_color(bg_color);
    style.hover = nk_style_item_color(is_selected ? nk_rgb(85, 85, 95) : nk_rgb(50, 50, 55));
    style.active = nk_style_item_color(nk_rgb(70, 70, 80));
    style.text_normal = text_color;
    style.text_hover = nk_rgb(255, 255, 255);
    style.text_active = nk_rgb(255, 255, 255);
    style.text_alignment = NK_TEXT_LEFT;
    style.padding = nk_vec2(8, 4);
    style.rounding = 3.0f;
    return style;
}

struct nk_style_button make_tree_expand_style(nk_context *ctx)
{
    struct nk_style_button style = ctx->style.button;
    style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
    style.hover = nk_style_item_color(nk_rgb(80, 80, 85));
    style.text_normal = nk_rgb(200, 200, 200);
    style.text_hover = nk_rgb(255, 255, 255);
    style.rounding = 3.0f;
    return style;
}

struct nk_color get_diff_status_color(DiffStatus status, bool is_selected, bool is_folder)
{
    if (is_selected)
        return nk_rgb(255, 255, 255);
    switch (status)
    {
    case DiffStatus::Added:    return nk_rgb(100, 255, 100);
    case DiffStatus::Modified: return nk_rgb(255, 200, 50);
    case DiffStatus::Removed:  return nk_rgb(255, 100, 100);
    default:                   return is_folder ? nk_rgb(220, 220, 220) : nk_rgb(200, 200, 200);
    }
}

bool draw_tree_row(nk_context *ctx, int depth, bool is_folder, bool is_expanded,
                   bool is_selected, const std::string &label, const std::string &info,
                   struct nk_color text_color, bool &out_toggle_expand,
                   bool *out_right_clicked, struct nk_vec2 *out_mouse_pos)
{
    out_toggle_expand = false;
    if (out_right_clicked)
        *out_right_clicked = false;

    nk_layout_row_begin(ctx, NK_STATIC, 26, 4);

    // Col 1: Indentation spacer
    nk_layout_row_push(ctx, depth * 16.0f + 10.0f);
    nk_spacing(ctx, 1);

    // Col 2: Expand/collapse button or empty spacing
    nk_layout_row_push(ctx, 24.0f);
    if (is_folder)
    {
        struct nk_style_button expand_style = make_tree_expand_style(ctx);
        if (nk_button_label_styled(ctx, &expand_style, is_expanded ? "-" : "+"))
        {
            out_toggle_expand = true;
        }
    }
    else
    {
        nk_spacing(ctx, 1);
    }

    // Col 3: Label button
    nk_layout_row_push(ctx, 370.0f);
    struct nk_style_button button_style = make_tree_button_style(ctx, is_selected, depth, text_color);
    bool clicked = nk_button_label_styled(ctx, &button_style, label.c_str()) != 0;

    // Optional right-click detection on the label button
    if (out_right_clicked && out_mouse_pos)
    {
        if (nk_input_is_mouse_hovering_rect(&ctx->input, nk_widget_bounds(ctx)))
        {
            if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_RIGHT))
            {
                *out_right_clicked = true;
                *out_mouse_pos = ctx->input.mouse.pos;
            }
        }
    }

    // Col 4: Info label
    nk_layout_row_push(ctx, 200.0f);
    nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

    nk_layout_row_end(ctx);

    return clicked;
}

