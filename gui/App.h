#pragma once
#include <SDL.h>
#include <GL/glew.h>

#ifndef NK_NUKLEAR_H_
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#endif
#include "nuklear.h"

class App {
public:
    App();
    ~App();

    int Run();

private:
    void InitSDL();
    void InitFonts();
    void ProcessEvents();
    void Render();
    void CheckTasks();
    void Cleanup();

    SDL_Window* m_win = nullptr;
    SDL_GLContext m_glContext = nullptr;
    nk_context* m_ctx = nullptr;
    bool m_running = false;
    bool m_scroll_to_selected = false;
    int m_window_width = 0;
    int m_window_height = 0;
    SDL_Cursor* m_resize_cursor = nullptr;
    SDL_Cursor* m_arrow_cursor = nullptr;
};
