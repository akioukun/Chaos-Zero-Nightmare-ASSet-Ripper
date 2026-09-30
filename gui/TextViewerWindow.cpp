#include "gui/TextViewerWindow.h"
#include <SDL.h>
#include <fstream>
#include "core/DialogPaths.h"

void draw_text_viewer_window(nk_context *ctx, int window_width, int window_height)
{
    if (!g_state.text_viewer.show_window || g_state.preview.text_preview.empty())
        return;

    if (nk_begin(ctx, "Text Viewer", nk_rect(40, 40, static_cast<float>(window_width) - 80, static_cast<float>(window_height) - 80),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
    {
        nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
        nk_layout_row_push(ctx, 120);
        if (nk_button_label(ctx, "Copy All"))
        {
            SDL_SetClipboardText(g_state.preview.text_full.c_str());
        }
        nk_layout_row_push(ctx, 120);
        if (nk_button_label(ctx, "Save As..."))
        {
            try
            {
                const std::string save_path = DialogPaths::SaveFile("Save Text", "output.txt", {"Text", "*.txt", "All Files", "*.*"});
                if (!save_path.empty())
                {
                    std::ofstream out(save_path);
                    if (out.is_open())
                    {
                        out << g_state.preview.text_full;
                        out.close();
                    }
                }
            }
            catch (...)
            {
            }
        }
        nk_layout_row_end(ctx);

        if (g_state.text_viewer.text_buffer.empty())
        {
            g_state.text_viewer.text_buffer.push_back('\0');
        }
        nk_layout_row_dynamic(ctx, static_cast<float>(window_height) - 180, 1);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY, g_state.text_viewer.text_buffer.data(), static_cast<int>(g_state.text_viewer.text_buffer.size()), nk_filter_default);
    }
    else
    {
        g_state.text_viewer.show_window = false;
    }
    nk_end(ctx);
}
