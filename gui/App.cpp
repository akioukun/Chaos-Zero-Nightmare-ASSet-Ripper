#define NOMINMAX
#include "gui/App.h"
#include <iostream>
#include <string>
#include <future>
#include <atomic>
#include <memory>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <fstream>
#include <SDL_image.h>

#include "core/Core.h"
#include "core/FileTree.h"
#include "core/RipperOptions.h"
#include "gui/AppState.h"
#include "gui/Popups.h"
#include "gui/TextViewerWindow.h"
#include "gui/PreviewPanel.h"
#include "gui/FileBrowserPanel.h"
#include "gui/SpinePanel.h"
#include "gui/Toolbar.h"
#include "parsers/SpineDictionary.h"
#include "parsers/SpineRenderer.h"

// nuklear implementations
#define NK_IMPLEMENTATION
#define NK_SDL_GL3_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_sdl_gl3.h"

App::App() {
}

App::~App() {
    Cleanup();
}

void App::InitSDL() {
    load_options_from_ini();

    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    m_resize_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    m_arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    m_win = SDL_CreateWindow("Chaos Zero Nightmare ASSet Ripper v1.5.0",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       INITIAL_WINDOW_WIDTH, INITIAL_WINDOW_HEIGHT,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    m_glContext = SDL_GL_CreateContext(m_win);
    glewInit();

    m_ctx = nk_sdl_init(m_win);
    InitFonts();
}

void App::InitFonts() {
    struct nk_font_atlas *atlas;
    nk_sdl_font_stash_begin(&atlas);

    struct nk_font_config config = nk_font_config(0);
    config.oversample_h = 1;
    config.oversample_v = 1;
    config.pixel_snap = 1;

    float font_size = 18.0f;

    struct nk_font *font = nullptr;
    bool font_loaded = false;

    // Try to load Malgun Gothic (Korean)
    const char *font_kr = R"(C:\Windows\Fonts\malgun.ttf)";
    std::ifstream f_kr(font_kr);
    if (f_kr.good())
    {
        // Load Base + Korean
        config.range = nk_font_default_glyph_ranges();
        font = nk_font_atlas_add_from_file(atlas, font_kr, font_size, &config);

        config.merge_mode = nk_true;
        config.range = nk_font_korean_glyph_ranges();
        nk_font_atlas_add_from_file(atlas, font_kr, font_size, &config);
        font_loaded = true;
    }

    // Try to load Microsoft YaHei (Chinese)
    const char *font_cn = R"(C:\Windows\Fonts\msyh.ttc)";
    std::ifstream f_cn(font_cn);
    if (f_cn.good())
    {
        config.merge_mode = font_loaded ? nk_true : nk_false;

        if (!font_loaded)
        {
            config.range = nk_font_default_glyph_ranges();
            font = nk_font_atlas_add_from_file(atlas, font_cn, font_size, &config);
            config.merge_mode = nk_true;
            font_loaded = true;
        }

        config.range = nk_font_chinese_glyph_ranges();
        nk_font_atlas_add_from_file(atlas, font_cn, font_size, &config);
    }

    // Fallback to Segoe UI if no CJK font found
    if (!font_loaded)
    {
        const char *font_base = R"(C:\Windows\Fonts\segoeui.ttf)";
        std::ifstream f_base(font_base);
        if (f_base.good())
        {
            config.merge_mode = nk_false;
            config.range = nk_font_default_glyph_ranges();
            font = nk_font_atlas_add_from_file(atlas, font_base, font_size, &config);
        }
    }

    nk_sdl_font_stash_end();
    if (font)
        nk_style_set_font(m_ctx, &font->handle);
}

void App::ProcessEvents() {
    SDL_Event evt;
    nk_input_begin(m_ctx);
    while (SDL_PollEvent(&evt))
    {
        if (evt.type == SDL_QUIT)
        {
            m_running = false;
        }
        else if (evt.type == SDL_WINDOWEVENT)
        {
            if (evt.window.event == SDL_WINDOWEVENT_CLOSE)
            {
                Uint32 windowID = evt.window.windowID;
                if (windowID == SDL_GetWindowID(m_win))
                {
                    m_running = false;
                }
                else if (g_state.image.window && windowID == SDL_GetWindowID(g_state.image.window))
                {
                    if (g_state.image.texture)
                    {
                        SDL_DestroyTexture(g_state.image.texture);
                        g_state.image.texture = nullptr;
                    }
                    if (g_state.image.renderer)
                    {
                        SDL_DestroyRenderer(g_state.image.renderer);
                        g_state.image.renderer = nullptr;
                    }
                    SDL_DestroyWindow(g_state.image.window);
                    g_state.image.window = nullptr;
                }
            }
        }
        else if (evt.type == SDL_KEYDOWN)
        {
            // Spine viewer gets priority for arrow keys when open
            if (g_state.spine.show_window && g_state.spine.dictionary.IsBuilt() && !g_state.spine.visible_indices.empty())
            {
                if (evt.key.keysym.sym == SDLK_UP || evt.key.keysym.sym == SDLK_DOWN)
                {
                    auto it = std::find(g_state.spine.visible_indices.begin(), g_state.spine.visible_indices.end(), g_state.spine.selected_index);
                    int new_idx = g_state.spine.selected_index;
                    if (evt.key.keysym.sym == SDLK_UP)
                    {
                        if (it != g_state.spine.visible_indices.end() && it != g_state.spine.visible_indices.begin())
                            new_idx = *(it - 1);
                        else if (it == g_state.spine.visible_indices.end() && !g_state.spine.visible_indices.empty())
                            new_idx = g_state.spine.visible_indices.back();
                    }
                    else
                    {
                        if (it != g_state.spine.visible_indices.end() && (it + 1) != g_state.spine.visible_indices.end())
                            new_idx = *(it + 1);
                        else if (it == g_state.spine.visible_indices.end() && !g_state.spine.visible_indices.empty())
                            new_idx = g_state.spine.visible_indices.front();
                    }
                    if (new_idx != g_state.spine.selected_index)
                    {
                        g_state.spine.selected_index = new_idx;
                        // Load skeleton on arrow key selection
                        if (const auto &entries = g_state.spine.dictionary.GetEntries(); g_state.spine.selected_index >= 0 && g_state.spine.selected_index < (int)entries.size())
                        {
                            if (!g_state.spine.viewer)
                                g_state.spine.viewer = std::make_unique<SpineViewer>();
                            g_state.spine.viewer->loadSkeleton(g_state.spine.dictionary, *g_state.browser.data_pack, entries[g_state.spine.selected_index]);
                            g_state.spine.viewer->setFlipX(g_state.spine.flip_x);
                            g_state.spine.viewer->setFlipY(g_state.spine.flip_y);
                            g_state.spine.selected_animation = 0;
                            g_state.spine.selected_skin = 0;
                            g_state.spine.last_tick = 0;
                            g_state.spine.edit_mode = false;
                        }
                    }
                }
                else if (evt.key.keysym.sym == SDLK_RETURN && g_state.spine.selected_index >= 0)
                {
                    if (const auto &entries = g_state.spine.dictionary.GetEntries(); g_state.spine.selected_index < static_cast<int>(entries.size()))
                    {
                        if (!g_state.spine.viewer)
                            g_state.spine.viewer = std::make_unique<SpineViewer>();
                        g_state.spine.viewer->loadSkeleton(g_state.spine.dictionary, *g_state.browser.data_pack, entries[g_state.spine.selected_index]);
                        g_state.spine.selected_animation = 0;
                        g_state.spine.selected_skin = 0;
                        g_state.spine.last_tick = 0;
                        g_state.spine.edit_mode = false;
                    }
                }
            }
            else if (g_state.browser.selection.selected_node)
            {
                if ((evt.key.keysym.sym == SDLK_UP || evt.key.keysym.sym == SDLK_DOWN) && !g_state.browser.selection.visible_nodes.empty())
                {
                    auto it = std::find(g_state.browser.selection.visible_nodes.begin(), g_state.browser.selection.visible_nodes.end(), g_state.browser.selection.selected_node);
                    if (it != g_state.browser.selection.visible_nodes.end())
                    {
                        if (evt.key.keysym.sym == SDLK_UP && it > g_state.browser.selection.visible_nodes.begin())
                        {
                            g_state.browser.selection.selected_node = *(it - 1);
                            handle_node_click(g_state.browser.selection.selected_node, std::holds_alternative<Core::FolderInfo>(g_state.browser.selection.selected_node->data));
                            m_scroll_to_selected = true;
                        }
                        else if (evt.key.keysym.sym == SDLK_DOWN && it < g_state.browser.selection.visible_nodes.end() - 1)
                        {
                            g_state.browser.selection.selected_node = *(it + 1);
                            handle_node_click(g_state.browser.selection.selected_node, std::holds_alternative<Core::FolderInfo>(g_state.browser.selection.selected_node->data));
                            m_scroll_to_selected = true;
                        }
                    }
                }
                else if (evt.key.keysym.sym == SDLK_RETURN)
                {
                    if (std::holds_alternative<Core::FolderInfo>(g_state.browser.selection.selected_node->data))
                    {
                        if (g_state.browser.selection.expanded_folders.find(g_state.browser.selection.selected_node) != g_state.browser.selection.expanded_folders.end())
                            g_state.browser.selection.expanded_folders.erase(g_state.browser.selection.selected_node);
                        else
                            g_state.browser.selection.expanded_folders.insert(g_state.browser.selection.selected_node);
                    }
                }
                else if (evt.key.keysym.sym == SDLK_RIGHT)
                {
                    if (std::holds_alternative<Core::FolderInfo>(g_state.browser.selection.selected_node->data))
                    {
                        g_state.browser.selection.expanded_folders.insert(g_state.browser.selection.selected_node);
                    }
                }
                else if (evt.key.keysym.sym == SDLK_LEFT)
                {
                    if (std::holds_alternative<Core::FolderInfo>(g_state.browser.selection.selected_node->data))
                    {
                        g_state.browser.selection.expanded_folders.erase(g_state.browser.selection.selected_node);
                    }
                }
            }
        }
        nk_sdl_handle_event(&evt);
    }
    nk_input_end(m_ctx);
}

void App::CheckTasks() {
    if (g_state.tasks.running && g_state.tasks.future.valid() &&
        g_state.tasks.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        g_state.tasks.running = false;
        try
        {
            g_state.tasks.future.get();
        }
        catch (const std::exception &e)
        {
            g_state.tasks.status = "Error: " + std::string(e.what());
        }
        catch (...)
        {
            g_state.tasks.status = "Unknown error occurred";
        }
        g_state.tasks.progress = 1.0f;
        if (g_state.tasks.kind == TaskKind::Scan)
        {
            g_state.tasks.scan_complete = true;
            g_state.tasks.status = "Scan complete. " + std::to_string(Core::NodeFileCount(g_state.browser.data_pack->GetFileTree())) + " files found.";
        }
        else if (g_state.tasks.kind == TaskKind::Extract)
        {
            g_state.tasks.status = "Extraction complete.";
        }
        g_state.tasks.kind = TaskKind::None;
    }
}

void App::Render() {
    SDL_GetWindowSize(m_win, &m_window_width, &m_window_height);

    draw_context_menu(m_ctx);

    draw_options_popup(m_ctx, m_window_width, m_window_height);
    draw_credits_popup(m_ctx, m_window_width, m_window_height);
    draw_feedback_popups(m_ctx, m_window_width, m_window_height);
    draw_text_viewer_window(m_ctx, m_window_width, m_window_height);

    if (nk_begin(m_ctx, "Main", nk_rect(0, 0, static_cast<float>(m_window_width), static_cast<float>(m_window_height)), NK_WINDOW_NO_SCROLLBAR))
    {
        draw_toolbar(m_ctx);

        bool tree_scanned = (g_state.browser.data_pack != nullptr) && g_state.tasks.scan_complete.load();
        bool selection_exists = g_state.diff.show_tree ? (g_state.diff.selection.selected_node != nullptr) : (g_state.browser.selection.selected_node != nullptr);

        float content_height = static_cast<float>(m_window_height) - 130;

        if (!g_state.spine.show_window)
        {
            nk_layout_row_begin(m_ctx, NK_STATIC, 30, 2);
            nk_layout_row_push(m_ctx, 80);
            nk_label(m_ctx, "Search:", NK_TEXT_LEFT);
            nk_layout_row_push(m_ctx, 300);
            nk_edit_string_zero_terminated(m_ctx, NK_EDIT_FIELD, g_state.browser.search_buffer, sizeof(g_state.browser.search_buffer), nk_filter_default);
            g_state.browser.search_query = g_state.browser.search_buffer;
            nk_layout_row_end(m_ctx);
            bool showing_preview_panel = (g_state.preview.mode != PreviewMode::None || !g_state.preview.error.empty());

            float max_sidebar = static_cast<float>(m_window_width) * 0.7f;
            if (float min_sidebar = 300.0f; g_state.common.sidebar_width < min_sidebar)
                g_state.common.sidebar_width = min_sidebar;
            if (g_state.common.sidebar_width > max_sidebar)
                g_state.common.sidebar_width = max_sidebar;

            float left_width = showing_preview_panel ? g_state.common.sidebar_width : static_cast<float>(m_window_width) - 20.0f;
            float right_width = static_cast<float>(m_window_width) - left_width - 30.0f;

            nk_layout_row_begin(m_ctx, NK_STATIC, content_height, (showing_preview_panel) ? 3 : 1);
            nk_layout_row_push(m_ctx, left_width);

            draw_file_browser_panel(m_ctx, tree_scanned, m_scroll_to_selected);

            if (showing_preview_panel)
            {
                struct nk_rect bounds{};
                nk_layout_row_push(m_ctx, 8.0f);
                bounds = nk_widget_bounds(m_ctx);
                nk_input *in = &m_ctx->input;

                bool hovering_splitter = nk_input_is_mouse_hovering_rect(in, bounds);
                bool mouse_down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT);

                if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, bounds, nk_true))
                {
                    g_state.common.dragging_splitter = true;
                }

                if (!mouse_down)
                {
                    g_state.common.dragging_splitter = false;
                }

                if (g_state.common.dragging_splitter || hovering_splitter)
                {
                    SDL_SetCursor(m_resize_cursor);
                }
                else
                {
                    SDL_SetCursor(m_arrow_cursor);
                }

                if (g_state.common.dragging_splitter)
                {
                    g_state.common.sidebar_width += m_ctx->input.mouse.delta.x;
                }

                // Draw splitter handle
                nk_fill_rect(&m_ctx->current->buffer, bounds, 0, nk_rgb(40, 40, 45));
                nk_stroke_line(&m_ctx->current->buffer, bounds.x + 4, bounds.y + 10, bounds.x + 4, bounds.y + bounds.h - 10, 1.0f, nk_rgb(100, 100, 100));

                nk_layout_row_push(m_ctx, right_width - 8.0f);
                draw_preview_panel(m_ctx, right_width, content_height);
            }

            nk_layout_row_end(m_ctx);
        }
        else
        {
            draw_spine_panel(m_ctx, content_height, m_window_width);
        }

        nk_layout_row_dynamic(m_ctx, 4, 1);
        nk_spacing(m_ctx, 1);

        if (g_state.tasks.running)
        {
            nk_layout_row_begin(m_ctx, NK_STATIC, 22, 2);
            nk_layout_row_push(m_ctx, 220);
            float cur_progress = std::clamp(g_state.tasks.progress.load(), 0.0f, 1.0f);
            nk_size prog_val = cur_progress * 1000.0f;
            nk_progress(m_ctx, &prog_val, 1000, NK_FIXED);
            nk_layout_row_push(m_ctx, static_cast<float>(m_window_width) - 260);
            int percent = static_cast<int>(cur_progress * 100.0f);
            std::string status_text = g_state.tasks.status + " (" + std::to_string(percent) + "%)";
            nk_label_colored(m_ctx, status_text.c_str(), NK_TEXT_LEFT, nk_rgb(100, 200, 255));
            nk_layout_row_end(m_ctx);
        }
        else
        {
            nk_layout_row_dynamic(m_ctx, 22, 1);
            if (selection_exists && g_state.browser.selection.selected_node && std::holds_alternative<Core::FileInfo>(g_state.browser.selection.selected_node->data))
            {
                const auto &info = std::get<Core::FileInfo>(g_state.browser.selection.selected_node->data);
                char off_buf[32];
                snprintf(off_buf, sizeof(off_buf), "0x%llX", static_cast<unsigned long long>(info.offset));
                std::string details = "Selected: " + g_state.browser.selection.selected_node->name +
                                     " | Size: " + std::to_string(info.size) + " B" +
                                     " | Offset: " + off_buf +
                                     " | Format: " + info.format;
                nk_label_colored(m_ctx, details.c_str(), NK_TEXT_LEFT, nk_rgb(180, 180, 180));
            }
            else
            {
                nk_label_colored(m_ctx, g_state.tasks.status.c_str(), NK_TEXT_LEFT, nk_rgb(160, 160, 160));
            }
        }
    }
    nk_end(m_ctx);

    glViewport(0, 0, m_window_width, m_window_height);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
    render_image_window();
    SDL_GL_SwapWindow(m_win);
}

void App::Cleanup() {
    if (g_state.preview.texture)
        glDeleteTextures(1, &g_state.preview.texture);
    if (g_state.sct.texture)
        glDeleteTextures(1, &g_state.sct.texture);
        
    if (m_ctx) {
        nk_sdl_shutdown();
        m_ctx = nullptr;
    }
    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_win) {
        SDL_DestroyWindow(m_win);
        m_win = nullptr;
    }
    if (g_state.image.window)
    {
        if (g_state.image.texture)
        {
            SDL_DestroyTexture(g_state.image.texture);
        }
        if (g_state.image.renderer)
        {
            SDL_DestroyRenderer(g_state.image.renderer);
        }
        SDL_DestroyWindow(g_state.image.window);
        g_state.image.window = nullptr;
    }
    IMG_Quit();
    SDL_Quit();
}

int App::Run() {
    InitSDL();
    m_running = true;

    while (m_running) {
        m_scroll_to_selected = false;
        ProcessEvents();
        CheckTasks();
        Render();
    }
    
    return 0;
}
