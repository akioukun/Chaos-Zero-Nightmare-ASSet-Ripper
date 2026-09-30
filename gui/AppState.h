#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <GL/glew.h>
#include <SDL.h>

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

#include "json.hpp"
#include "core/Core.h"
#include "archive/IArchive.h"
#include "parsers/SpineDictionary.h"

class SpineViewer;

using json = nlohmann::ordered_json;

#define INITIAL_WINDOW_WIDTH 1400
#define INITIAL_WINDOW_HEIGHT 900
#define DOUBLE_CLICK_TIME_MS 300

struct FileBrowserState
{
    std::unique_ptr<IArchive> data_pack;
    const Core::FileNode *selected_node = nullptr;
    const Core::FileNode *last_clicked_node = nullptr;
    std::unordered_set<const Core::FileNode *> selected_nodes;
    std::unordered_set<const Core::FileNode *> expanded_folders;
    std::vector<const Core::FileNode *> visible_nodes;
    char search_buffer[256] = {};
    std::string search_query;
    Uint32 last_click_time = 0;
    int click_count = 0;
};

enum class TaskKind { None, Scan, Extract };

struct TaskState
{
    std::future<void> future;
    std::atomic<float> progress = 0.f;
    std::atomic<bool> running = false;
    std::atomic<bool> scan_complete = false;
    TaskKind kind = TaskKind::None;
    std::string status = "Select a data.pack file to begin.";
};

enum class PreviewMode
{
    None,
    Image,
    DB,
    JSON,
    Text
};

struct PreviewState
{
    GLuint texture = 0;
    int width = 0, height = 0;
    bool has_preview = false;
    std::string error, text_preview, text_full, json_preview;
    PreviewMode mode = PreviewMode::None;
    const Core::FileNode* preview_node = nullptr;
};

struct TextViewerState
{
    bool show_window = false;
    bool wrap_lines = true;
    char filter[256] = {};
    std::vector<char> text_buffer;
};

struct DatabaseViewerState
{
    json json_data;
    std::vector<std::string> column_names;
    std::vector<std::vector<std::string>> rows;
    std::string filename;
    char search_buffer[128] = {};
};

struct ImageWindowState
{
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    int width = 0, height = 0;
    std::string title;
};

struct ContextMenuState
{
    bool visible = false;
    const Core::FileNode *node = nullptr;
    struct nk_vec2 position = {0, 0};
};

struct CommonState
{
    bool show_options = false;
    nk_bool export_sct_as_png = nk_true;
    bool convert_all_sct = false;
    nk_bool export_db_as_json = nk_true;
    nk_bool enable_open_folder = nk_false;
    bool show_success_popup = false;
    std::string success_message;
    bool show_error_popup = false;
    std::string error_message;
    float sidebar_width = 600.0f;
    bool dragging_splitter = false;
};

struct CreditsState
{
    bool show_window = false;
};

struct SCTPreviewState
{
    bool show_window = false;
    GLuint texture = 0;
    int width = 0, height = 0;
    std::string filename;
};

struct SpineViewerState
{
    SpineDictionary dictionary;
    bool show_window = false;
    char search_buffer[256] = {};
    std::string search_query;
    int selected_index = -1;
    std::future<void> build_future;
    std::atomic<bool> building = false;
    std::unique_ptr<SpineViewer> viewer;
    int selected_animation = 0, selected_skin = 0;
    float speed = 1.f, zoom = 1.f;
    bool playing = true, flip_x = false, flip_y = false;
    Uint64 last_tick = 0;
    std::unordered_set<std::string> expanded_categories, collapsed_bones;
    std::vector<int> visible_indices;
    bool edit_mode = false;
    float scale_max = 1000.f;
    std::string selected_bone;
    char scale_max_buffer[16] = "1000";
    bool scroll_to_bone = false;

    // Viewport & gizmo state
    bool autoplay = false;
    bool pma_blend = true;
    bool pma_tex = true;
    int bg_preset = 0; // 0=none, 1=dark, 2=mid, 3=white
    int active_gizmo = 0;
    bool export_pending = false;
    char bone_search_buf[128] = {};
    bool bone_just_reset = false;
    bool link_scale = true;
};

enum class DiffStatus
{
    Unchanged,
    Added,
    Modified,
    Removed
};

struct DiffNode
{
    std::string name, full_path;
    bool is_folder = false;
    uint64_t size = 0;
    std::string format;
    DiffStatus status = DiffStatus::Unchanged;
    std::vector<std::unique_ptr<DiffNode>> children;
};

struct DiffViewerState
{
    bool show_tree = false;
    std::unique_ptr<DiffNode> root;
    std::unordered_set<const DiffNode *> expanded_folders, selected_nodes;
    const DiffNode *selected_node = nullptr;
    const DiffNode *last_clicked_node = nullptr;
    std::vector<const DiffNode *> visible_nodes;
};

struct AppState
{
    FileBrowserState browser;
    TaskState tasks;
    PreviewState preview;
    TextViewerState text_viewer;
    DatabaseViewerState database;
    ImageWindowState image;
    ContextMenuState context_menu;
    CommonState common;
    CreditsState credits;
    SCTPreviewState sct;
    SpineViewerState spine;
    DiffViewerState diff;
};

extern AppState g_state;

// Resets all application state (closes pack, clears selection, preview, spine viewer, diff tree).
void reset_app_state();
