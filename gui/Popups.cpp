#include <fstream>
#include <vector>

#include "gui/Popups.h"
#include "gui/PreviewPanel.h"
#include "gui/UIHelpers.h"
#include "core/RipperOptions.h"
#include "core/DialogPaths.h"

void save_options_to_ini()
{
    RipperOptions options = LoadRipperOptions();
    options.exportSctAsPng = (g_state.common.export_sct_as_png != nk_false);
    options.exportDbAsJson = (g_state.common.export_db_as_json != nk_false);
    options.enableOpenFolder = (g_state.common.enable_open_folder != nk_false);
    SaveRipperOptions(options);
}

void load_options_from_ini()
{
    const RipperOptions options = LoadRipperOptions();
    g_state.common.export_sct_as_png = options.exportSctAsPng ? nk_true : nk_false;
    g_state.common.export_db_as_json = options.exportDbAsJson ? nk_true : nk_false;
    g_state.common.enable_open_folder = options.enableOpenFolder ? nk_true : nk_false;
}

static nk_style_button make_toggle_style(const nk_context *ctx, const bool is_on)
{
    nk_style_button toggle_style = ctx->style.button;
    if (is_on)
    {
        toggle_style.normal = nk_style_item_color(nk_rgb(56, 120, 74));
        toggle_style.hover = nk_style_item_color(nk_rgb(66, 138, 86));
        toggle_style.active = nk_style_item_color(nk_rgb(50, 108, 66));
    }
    else
    {
        toggle_style.normal = nk_style_item_color(nk_rgb(100, 64, 64));
        toggle_style.hover = nk_style_item_color(nk_rgb(120, 74, 74));
        toggle_style.active = nk_style_item_color(nk_rgb(88, 56, 56));
    }
    toggle_style.text_normal = nk_rgb(240, 240, 240);
    toggle_style.text_hover = nk_rgb(255, 255, 255);
    toggle_style.text_active = nk_rgb(255, 255, 255);
    return toggle_style;
}

void draw_options_popup(nk_context *ctx, const int window_width, const int window_height)
{
    if (!g_state.common.show_options)
        return;

    constexpr float export_options_width = 530.0f;
    constexpr float export_options_height = 460.0f;
    const float export_options_x = (window_width - export_options_width) * 0.5f;
    if (const float export_options_y = (window_height - export_options_height) * 0.5f;
        nk_begin(ctx, "Export Options", nk_rect(export_options_x, export_options_y, export_options_width, export_options_height), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE))
    {
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, "Configure extraction options:", NK_TEXT_LEFT);

        nk_layout_row_begin(ctx, NK_STATIC, 32, 2);
        nk_layout_row_push(ctx, 380);
        nk_label(ctx, "Convert SCT files to PNG", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 120);
        {
            const nk_style_button toggle_style = make_toggle_style(ctx, g_state.common.export_sct_as_png != nk_false);
            if (nk_button_label_styled(ctx, &toggle_style, g_state.common.export_sct_as_png ? "ON" : "OFF"))
            {
                g_state.common.export_sct_as_png = g_state.common.export_sct_as_png ? nk_false : nk_true;
                save_options_to_ini();
            }
        }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "When enabled, .sct/.sct2 files will be", NK_TEXT_LEFT);
        nk_label(ctx, "automatically converted to PNG during extraction.", NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        nk_layout_row_begin(ctx, NK_STATIC, 32, 2);
        nk_layout_row_push(ctx, 380);
        nk_label(ctx, "Convert DB files to JSON", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 120);
        {
            const nk_style_button toggle_style = make_toggle_style(ctx, g_state.common.export_db_as_json != nk_false);
            if (nk_button_label_styled(ctx, &toggle_style, g_state.common.export_db_as_json ? "ON" : "OFF"))
            {
                g_state.common.export_db_as_json = g_state.common.export_db_as_json ? nk_false : nk_true;
                save_options_to_ini();
            }
        }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "When enabled, .db files will be", NK_TEXT_LEFT);
        nk_label(ctx, "automatically converted to JSON during extraction.", NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        nk_layout_row_begin(ctx, NK_STATIC, 32, 2);
        nk_layout_row_push(ctx, 380);
        nk_label(ctx, "Enable Open Folder", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 120);
        {
            const nk_style_button toggle_style = make_toggle_style(ctx, g_state.common.enable_open_folder != nk_false);
            if (nk_button_label_styled(ctx, &toggle_style, g_state.common.enable_open_folder ? "ON" : "OFF"))
            {
                g_state.common.enable_open_folder = g_state.common.enable_open_folder ? nk_false : nk_true;
                save_options_to_ini();
            }
        }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "When enabled, open folder button show up", NK_TEXT_LEFT);
        nk_label(ctx, "letting user choose a folder to scan instead of only data.pack", NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 25, 1);

        nk_layout_row_dynamic(ctx, 30, 2);
        if (nk_button_label(ctx, "OK"))
        {
            save_options_to_ini();
            g_state.common.show_options = false;
            g_state.tasks.status = "Options saved";
        }
        if (nk_button_label(ctx, "Cancel"))
        {
            g_state.common.show_options = false;
        }
    }
    else
    {
        g_state.common.show_options = false;
    }
    nk_end(ctx);
}

void draw_credits_popup(nk_context *ctx, int window_width, int window_height)
{
    if (!g_state.credits.show_window)
        return;

    constexpr float credits_options_width = 700.0f;
    constexpr float credits_options_height = 300.0f;
    const float credits_options_x = (window_width - credits_options_width) * 0.5f;
    if (const float credits_options_y = (window_height - credits_options_height) * 0.5f;
        nk_begin(ctx, "Credits", nk_rect(credits_options_x, credits_options_y, credits_options_width, credits_options_height),NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, "Chaos Zero Nightmare ASSet Ripper v1.4.0", NK_TEXT_CENTERED);
        nk_label(ctx, "by @akioukun (github.com/akioukun)", NK_TEXT_CENTERED);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "", NK_TEXT_LEFT);
        nk_label(ctx, "made with nuklear, sdl2/opengl, portable-file-dialogs", NK_TEXT_CENTERED);
        nk_label(ctx, "SCT/SCT2 support with astcenc & etcdec", NK_TEXT_CENTERED);
        nk_label(ctx, "big thanks to @formagGino (github.com/formagGinoo) for SCT Parser, DB Parser and SCSP Parser", NK_TEXT_CENTERED);
        nk_label(ctx, "thanks to @LukeFZ (github.com/LukeFZ) for DB decryption logic", NK_TEXT_CENTERED);
        nk_label(ctx, "thanks to @lIllIIlI (github.com/lIllIIlI) for SpineViewer logic", NK_TEXT_CENTERED);

        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Close"))
        {
            g_state.credits.show_window = false;
        }
    }
    else
    {
        g_state.credits.show_window = false;
    }
    nk_end(ctx);
}

void draw_feedback_popups(nk_context *ctx, int window_width, int window_height)
{
    if (g_state.common.show_success_popup)
    {
        constexpr float export_success_width = 215.0f;
        constexpr float export_success_height = 120.0f;
        const float export_success_x = (window_width - export_success_width) * 0.5f;
        if (const float export_success_y = (window_height - export_success_height) * 0.5f;
            nk_begin(ctx, "Success", nk_rect(export_success_x, export_success_y, export_success_width, export_success_height),NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE))
        {
            nk_layout_row_dynamic(ctx, 30, 1);
            nk_label(ctx, g_state.common.success_message.c_str(), NK_TEXT_CENTERED);
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "OK"))
            {
                g_state.common.show_success_popup = false;
            }
        }
        else
        {
            g_state.common.show_success_popup = false;
        }
        nk_end(ctx);
    }

    if (g_state.common.show_error_popup)
    {
        constexpr float popup_width = 400.0f;
        constexpr float popup_height = 140.0f;
        const float popup_x = (window_width - popup_width) * 0.5f;
        if (const float popup_y = (window_height - popup_height) * 0.5f;
            nk_begin(ctx, "Error", nk_rect(popup_x, popup_y, popup_width, popup_height),NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE))
        {
            nk_layout_row_dynamic(ctx, 45, 1);
            nk_label_wrap(ctx, g_state.common.error_message.c_str());
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "OK"))
            {
                g_state.common.show_error_popup = false;
            }
        }
        else
        {
            g_state.common.show_error_popup = false;
        }
        nk_end(ctx);
    }
}

void draw_context_menu(nk_context *ctx)
{
    if (!g_state.context_menu.visible || !g_state.context_menu.node)
        return;

    if (nk_begin(ctx, "Context Menu",
                 nk_rect(g_state.context_menu.position.x, g_state.context_menu.position.y, 180.0f, 200.0f),
                 NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
    {
        if (std::holds_alternative<Core::FileInfo>(g_state.context_menu.node->data))
        {
            const auto &info = std::get<Core::FileInfo>(g_state.context_menu.node->data);

            nk_layout_row_dynamic(ctx, 25, 1);

            if (is_db_file(info.format))
            {
                if (nk_button_label(ctx, "Export as JSON"))
                {
                    export_db_as_json_file(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
            }
            else if (is_scsp_file(info.format))
            {
                if (nk_button_label(ctx, "Export as JSON"))
                {
                    export_scsp_as_json_file(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
            }
            else if (is_sct_format(info.format))
            {
                if (nk_button_label(ctx, "Export as PNG"))
                {
                    export_file_as_png(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
                if (nk_button_label(ctx, "Export as SCT"))
                {
                    export_file_as_sct(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
                if (nk_button_label(ctx, "Open Preview Window"))
                {
                    open_image_preview_window(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
            }
            else if (is_previewable_format(info.format))
            {
                if (nk_button_label(ctx, "Export as PNG"))
                {
                    export_file_as_png(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
                if (nk_button_label(ctx, "Open Preview Window"))
                {
                    open_image_preview_window(*g_state.context_menu.node);
                    g_state.context_menu.visible = false;
                }
            }

            if (nk_button_label(ctx, "Extract Raw"))
            {
                try
                {
                    const std::string save_path = DialogPaths::SaveFile("Extract File", g_state.context_menu.node->name, {"All Files", "*.*"});
                    if (!save_path.empty())
                    {
                        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(*g_state.context_menu.node);
                        std::ofstream out(save_path, std::ios::binary);
                        out.write(reinterpret_cast<const char *>(file_data.data()), file_data.size());
                        out.close();
                        g_state.tasks.status = "Extracted to: " + save_path;
                    }
                }
                catch (...)
                {
                }
                g_state.context_menu.visible = false;
            }
        }

        if (nk_button_label(ctx, "Close"))
        {
            g_state.context_menu.visible = false;
        }
    }
    else
    {
        g_state.context_menu.visible = false;
    }
    nk_end(ctx);
}

