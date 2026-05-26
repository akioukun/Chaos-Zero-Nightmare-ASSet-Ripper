#define NOMINMAX
#include <iostream>
#include <string>
#include <future>
#include <atomic>
#include <memory>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>

#include <GL/glew.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_image.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL3_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_sdl_gl3.h"

#include "portable-file-dialogs.h"
#include "DataPack.h"
#include "Core.h"
#include "SCTParser.h"
#include "DBParser.h"
#include "SCSPParser.h"
#include "SpineDictionary.h"
#include "SpineRenderer.h"
#include "Logger.h"
#include "json.hpp"

#define INITIAL_WINDOW_WIDTH 1400
#define INITIAL_WINDOW_HEIGHT 900
#define DOUBLE_CLICK_TIME_MS 300

using json = nlohmann::ordered_json;

static std::unique_ptr<DataPack> data_pack = nullptr;
static Core::FileNode const *selected_node = nullptr;
static std::unordered_set<const Core::FileNode *> selected_file_nodes;
static std::future<void> task_future;
static std::atomic<float> task_progress = 0.0f;
static std::atomic<bool> is_task_running = false;
static std::atomic<bool> is_scan_complete = false;
static std::string status_text = "Select a data.pack file to begin.";
static std::unordered_set<const Core::FileNode *> expanded_folders;
static char search_buffer[256] = {0};
static std::string search_query = "";
static std::vector<const Core::FileNode*> visible_nodes;

static GLuint preview_texture = 0;
static int preview_width = 0;
static int preview_height = 0;
static bool has_preview = false;
static std::string preview_error = "";
static std::string preview_atlas_data = "";
static std::string full_atlas_data = "";
static std::string preview_json_data = "";
enum class PreviewMode
{
    None,
    Image,
    DB,
    JSON,
    Text
};
static PreviewMode current_preview_mode = PreviewMode::None;

static bool atlas_wrap_lines = true;
static bool show_atlas_window = false;
static char atlas_filter[256] = {0};
static std::vector<char> atlas_text_buf;

static json db_json_data;
static std::vector<std::string> db_column_names;
static std::vector<std::vector<std::string>> db_rows;

static std::string db_filename = "";

static SDL_Window *image_window = nullptr;
static SDL_Renderer *image_renderer = nullptr;
static SDL_Texture *image_window_texture = nullptr;
static int image_window_width = 0;
static int image_window_height = 0;
static std::string image_window_title = "";

static bool show_context_menu = false;
static const Core::FileNode *context_menu_node = nullptr;
static struct nk_vec2 context_menu_pos = {0, 0};
static bool show_export_options_window = false;
static nk_bool export_sct_as_png = nk_true;
static bool export_convert_all_sct = false;
static nk_bool export_db_as_json = nk_true;
static nk_bool enable_open_folder = nk_false;

static bool show_credits_window = false;
static bool show_export_success = false;
static std::string export_success_msg = "";

static Uint32 last_click_time = 0;
static const Core::FileNode *last_clicked_node = nullptr;
static int click_count = 0;

static bool show_sct_preview_window = false;
static GLuint sct_preview_texture = 0;
static int sct_preview_width = 0;
static int sct_preview_height = 0;
static std::string sct_preview_filename = "";

// Spine viewer state
static SpineDictionary spine_dictionary;
static bool show_spine_viewer = false;
static char spine_search_buffer[256] = {0};
static std::string spine_search_query = "";
static int spine_selected_index = -1;
static std::future<void> spine_build_future;
static std::atomic<bool> spine_building = false;
static std::unique_ptr<SpineViewer> active_spine_viewer;
static int spine_anim_selected = 0;
static int spine_skin_selected = 0;
static float spine_speed = 1.0f;
static bool spine_playing = true;
static bool spine_flip_x = false;
static bool spine_flip_y = false;
static float spine_zoom = 1.0f;
static Uint64 spine_last_tick = 0;
static std::unordered_set<std::string> spine_expanded_categories;
static std::vector<int> spine_visible_indices; // built during list render
static bool spine_edit_mode = false;
static float spine_scale_max = 1000.0f;
static std::string spine_selected_bone = "";
static char spine_scale_max_buf[16] = "1000";
static bool spine_scroll_to_bone = false;
static std::unordered_set<std::string> spine_collapsed_bones; // folded in list only, not hidden

int get_file_count(const Core::FileNode &node)
{
    try
    {
        if (std::holds_alternative<Core::FileInfo>(node.data))
            return 1;
        int count = 0;
        const auto &folder = std::get<Core::FolderInfo>(node.data);
        for (const auto &child : folder.children)
        {
            count += get_file_count(child);
        }
        return count;
    }
    catch (...)
    {
        return 0;
    }
}

uint64_t get_folder_size(const Core::FileNode &node)
{
    try
    {
        if (std::holds_alternative<Core::FileInfo>(node.data))
        {
            return std::get<Core::FileInfo>(node.data).size;
        }
        uint64_t size = 0;
        const auto &folder = std::get<Core::FolderInfo>(node.data);
        for (const auto &child : folder.children)
        {
            size += get_folder_size(child);
        }
        return size;
    }
    catch (...)
    {
        return 0;
    }
}

std::string format_size(uint64_t bytes)
{
    const char *units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = (double)bytes;
    while (size >= 1024.0 && unit < 3)
    {
        size /= 1024.0;
        unit++;
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.2f %s", size, units[unit]);
    return buffer;
}

bool matches_search(const Core::FileNode &node, const std::string &query)
{
    if (query.empty())
        return true;
    std::string name_lower = node.name;
    std::string query_lower = query;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
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

bool is_sct_format(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".sct" || ext_lower == ".sct2";
}

bool is_db_file(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".db";
}

bool is_scsp_file(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".scsp";
}

bool is_previewable_format(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" ||
           ext_lower == ".bmp" || ext_lower == ".tga" || is_sct_format(ext_lower);
}

bool is_animated_webp(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".webp";
}

bool is_atlas_file(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".atlas";
}

bool is_json_file(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".json";
}

bool is_text_file(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".txt" || ext_lower == ".atlas";
}

void load_json_preview(const Core::FileNode &node, const std::string &content = "")
{
    try
    {
        preview_json_data = "";
        std::string json_content = content;

        if (json_content.empty())
        {
            std::vector<uint8_t> file_data = data_pack->GetFileData(node);
            if (!file_data.empty())
            {
                json_content = std::string(file_data.begin(), file_data.end());
            }
            else
            {
                preview_error = "Failed to read JSON file";
                current_preview_mode = PreviewMode::None;
                return;
            }
        }

        try
        {
            json parsed = json::parse(json_content);
            preview_json_data = parsed.dump(2);
        }
        catch (...)
        {
            preview_json_data = json_content;
        }
        current_preview_mode = PreviewMode::JSON;
    }
    catch (const std::exception &e)
    {
        preview_error = "Error loading JSON: " + std::string(e.what());
        current_preview_mode = PreviewMode::None;
    }
}

void load_db_preview(const Core::FileNode &node)
{
    try
    {
        db_column_names.clear();
        db_rows.clear();
        db_json_data.clear();
        preview_json_data = "";
        current_preview_mode = PreviewMode::None;

        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        if (file_data.empty())
        {
            preview_error = "Failed to read DB file";
            return;
        }

        std::string json_str = DBParser::ConvertToJson(file_data);
        if (json_str.empty() || json_str == "{}")
        {
            preview_json_data = json_str;
            current_preview_mode = PreviewMode::JSON;
            return;
        }

        try
        {
            db_json_data = json::parse(json_str);
        }
        catch (const json::parse_error &e)
        {
            preview_json_data = json_str;
            current_preview_mode = PreviewMode::JSON;
            return;
        }

        db_filename = node.name;

        if (!db_json_data.is_array() || db_json_data.empty())
        {
            preview_json_data = db_json_data.dump(2);
            current_preview_mode = PreviewMode::JSON;
            return;
        }

        // Check if it looks like a table (elements are objects)
        if (db_json_data[0].is_object())
        {
            for (auto &el : db_json_data[0].items())
            {
                db_column_names.push_back(el.key());
            }

            for (auto &row : db_json_data)
            {
                if (row.is_object())
                {
                    std::vector<std::string> row_data;
                    for (const auto &col : db_column_names)
                    {
                        if (row.contains(col))
                        {
                            if (row[col].is_null())
                            {
                                row_data.push_back("");
                            }
                            else if (row[col].is_string())
                            {
                                row_data.push_back(row[col].get<std::string>());
                            }
                            else
                            {
                                row_data.push_back(row[col].dump());
                            }
                        }
                        else
                        {
                            row_data.push_back("");
                        }
                    }
                    db_rows.push_back(row_data);
                }
            }
            current_preview_mode = PreviewMode::DB;
        }
        else
        {
            // Array of non-objects? Show as JSON
            preview_json_data = db_json_data.dump(2);
            current_preview_mode = PreviewMode::JSON;
        }

        preview_error = "";
    }
    catch (const std::exception &e)
    {
        preview_error = "DB parsing error: " + std::string(e.what());
        current_preview_mode = PreviewMode::JSON;
    }
}

void load_scsp_preview(const Core::FileNode &node)
{
    try
    {
        preview_json_data = "";
        current_preview_mode = PreviewMode::None;

        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        if (file_data.empty())
        {
            preview_error = "Failed to read SCSP file";
            return;
        }

        std::string json_str = SCSPParser::ConvertSCSPToJson(file_data);
        if (!json_str.empty())
        {
            try
            {
                json parsed = json::parse(json_str);
                preview_json_data = parsed.dump(2);
            }
            catch (...)
            {
                preview_json_data = json_str;
            }
            current_preview_mode = PreviewMode::JSON;
        }
        else
        {
            preview_error = "Failed to parse SCSP file";
        }

        preview_error = "";
    }
    catch (const std::exception &e)
    {
        preview_error = "SCSP parsing error: " + std::string(e.what());
        current_preview_mode = PreviewMode::None;
    }
}

void load_text_preview(const Core::FileNode &node)
{
    try
    {
        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        if (file_data.empty())
        {
            preview_atlas_data = "Failed to read file";
            full_atlas_data = "";
            current_preview_mode = PreviewMode::Text;
            return;
        }
        full_atlas_data = std::string(file_data.begin(), file_data.end());
        preview_atlas_data = full_atlas_data;
        atlas_text_buf.assign(preview_atlas_data.begin(), preview_atlas_data.end());
        atlas_text_buf.push_back('\0');
        if (preview_atlas_data.length() > 20000)
        {
            preview_atlas_data = preview_atlas_data.substr(0, 20000) + "\n\n... (truncated)";
        }
        current_preview_mode = PreviewMode::Text;
    }
    catch (const std::exception &e)
    {
        preview_atlas_data = "Error loading text: " + std::string(e.what());
        full_atlas_data = "";
        current_preview_mode = PreviewMode::Text;
    }
}

void load_image_preview(const Core::FileNode &node)
{
    if (preview_texture)
    {
        glDeleteTextures(1, &preview_texture);
        preview_texture = 0;
    }
    has_preview = false;
    preview_width = 0;
    preview_height = 0;
    preview_error = "";
    preview_atlas_data = "";
    full_atlas_data = "";
    preview_json_data = "";
    db_column_names.clear();
    db_rows.clear();
    current_preview_mode = PreviewMode::None;

    try
    {
        if (!std::holds_alternative<Core::FileInfo>(node.data))
        {
            preview_error = "Not a file";
            return;
        }
        const auto &info = std::get<Core::FileInfo>(node.data);

        if (is_db_file(info.format))
        {
            load_db_preview(node);
            return;
        }

        if (is_scsp_file(info.format))
        {
            load_scsp_preview(node);
            return;
        }

        if (is_json_file(info.format))
        {
            load_json_preview(node);
            return;
        }

        if (is_text_file(info.format))
        {
            load_text_preview(node);
            return;
        }

        if (is_animated_webp(info.format))
        {
            preview_error = "Animated WebP preview not supported. Use 'Export' to save the file.";
            return;
        }

        if (!is_previewable_format(info.format))
        {
            preview_error = "Preview not available for " + info.format + " files";
            return;
        }

        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        if (file_data.empty())
        {
            preview_error = "Failed to read file data";
            return;
        }

        std::string ext_lower = info.format;
        std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

        SDL_Surface *rgba_surface = nullptr;

        if (is_sct_format(ext_lower))
        {
            try
            {
                std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(file_data, false);

                if (png_data.empty())
                {
                    preview_error = "Failed to convert SCT/SCT2 file";
                    current_preview_mode = PreviewMode::None;
                    return;
                }

                SDL_RWops *rw = SDL_RWFromMem(png_data.data(), (int)png_data.size());
                if (!rw)
                {
                    preview_error = "Failed to create memory stream for SCT";
                    current_preview_mode = PreviewMode::None;
                    return;
                }

                SDL_Surface *surface = IMG_Load_RW(rw, 1);
                if (!surface)
                {
                    preview_error = "Failed to decode converted SCT image: " + std::string(IMG_GetError());
                    current_preview_mode = PreviewMode::None;
                    return;
                }

                rgba_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
                SDL_FreeSurface(surface);
            }
            catch (const std::exception &e)
            {
                preview_error = "SCT parsing error: " + std::string(e.what());
                current_preview_mode = PreviewMode::None;
                return;
            }
        }
        else
        {
            SDL_RWops *rw = SDL_RWFromMem(file_data.data(), (int)file_data.size());
            if (!rw)
            {
                preview_error = "Failed to create memory stream";
                current_preview_mode = PreviewMode::None;
                return;
            }

            SDL_Surface *surface = IMG_Load_RW(rw, 1);
            if (!surface)
            {
                preview_error = "Failed to decode image: " + std::string(IMG_GetError());
                current_preview_mode = PreviewMode::None;
                return;
            }

            rgba_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(surface);
        }

        if (!rgba_surface)
        {
            preview_error = "Failed to convert image format";
            current_preview_mode = PreviewMode::None;
            return;
        }

        preview_width = rgba_surface->w;
        preview_height = rgba_surface->h;

        glGenTextures(1, &preview_texture);
        glBindTexture(GL_TEXTURE_2D, preview_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_width, preview_height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba_surface->pixels);
        SDL_FreeSurface(rgba_surface);
        has_preview = true;
        current_preview_mode = PreviewMode::Image;
    }
    catch (const std::exception &e)
    {
        preview_error = "Error: " + std::string(e.what());
        has_preview = false;
        current_preview_mode = PreviewMode::None;
    }
    catch (...)
    {
        preview_error = "Unknown error occurred";
        has_preview = false;
        current_preview_mode = PreviewMode::None;
    }
}

void export_db_as_json_file(const Core::FileNode &node)
{
    try
    {
        std::string default_name = node.name;
        size_t dot_pos = default_name.find_last_of('.');
        if (dot_pos != std::string::npos)
        {
            default_name = default_name.substr(0, dot_pos);
        }
        default_name += ".json";

        auto f = pfd::save_file("Export DB as JSON", default_name,
                                {"JSON Files", "*.json", "All Files", "*.*"});

        if (!f.result().empty())
        {
            std::vector<uint8_t> file_data = data_pack->GetFileData(node);
            std::string json_str = DBParser::ConvertToJson(file_data);

            if (!json_str.empty() && json_str != "{}")
            {
                try
                {
                    // Formatta il JSON con indentazione
                    json parsed = json::parse(json_str);
                    std::string formatted_json = parsed.dump(2);

                    std::ofstream out(f.result());
                    out << formatted_json;
                    out.close();
                    status_text = "Exported DB to JSON: " + f.result();
                }
                catch (const json::parse_error &e)
                {
                    // Se il parsing fallisce, scrivi il JSON raw
                    std::ofstream out(f.result());
                    out << json_str;
                    out.close();
                    status_text = "Exported DB to JSON (unformatted): " + f.result();
                }
            }
            else
            {
                status_text = "Failed to convert DB to JSON";
            }
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Export error: " + std::string(e.what());
    }
}

void export_scsp_as_json_file(const Core::FileNode &node)
{
    try
    {
        std::string default_name = node.name;
        size_t dot_pos = default_name.find_last_of('.');
        if (dot_pos != std::string::npos)
        {
            default_name = default_name.substr(0, dot_pos);
        }
        default_name += ".json";

        auto f = pfd::save_file("Export SCSP as JSON", default_name,
                                {"JSON Files", "*.json", "All Files", "*.*"});

        if (!f.result().empty())
        {
            std::vector<uint8_t> file_data = data_pack->GetFileData(node);
            std::string json_str = SCSPParser::ConvertSCSPToJson(file_data);

            if (!json_str.empty())
            {
                try
                {
                    json parsed = json::parse(json_str);
                    std::string formatted_json = parsed.dump(2);

                    std::ofstream out(f.result());
                    out << formatted_json;
                    out.close();
                    status_text = "Exported SCSP to JSON: " + f.result();
                }
                catch (const json::parse_error &e)
                {
                    std::ofstream out(f.result());
                    out << json_str;
                    out.close();
                    status_text = "Exported SCSP to JSON (unformatted): " + f.result();
                }
            }
            else
            {
                status_text = "Failed to convert SCSP to JSON";
            }
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Export error: " + std::string(e.what());
    }
}

void export_json_file(const Core::FileNode &node)
{
    try
    {
        std::string default_name = node.name;
        size_t dot_pos = default_name.find_last_of('.');
        if (dot_pos != std::string::npos)
        {
            default_name = default_name.substr(0, dot_pos);
        }
        default_name += ".json";

        auto f = pfd::save_file("Export JSON", default_name,
                                {"JSON Files", "*.json", "All Files", "*.*"});

        if (!f.result().empty())
        {
            std::vector<uint8_t> file_data = data_pack->GetFileData(node);
            if (file_data.empty())
            {
                status_text = "Failed to read JSON file";
                return;
            }

            std::ofstream out(f.result(), std::ios::binary);
            out.write(reinterpret_cast<const char *>(file_data.data()), file_data.size());
            out.close();
            status_text = "Exported JSON: " + f.result();
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Export error: " + std::string(e.what());
    }
}

void open_image_preview_window(const Core::FileNode &node)
{
    try
    {
        if (image_window)
        {
            if (image_window_texture)
            {
                SDL_DestroyTexture(image_window_texture);
                image_window_texture = nullptr;
            }
            if (image_renderer)
            {
                SDL_DestroyRenderer(image_renderer);
                image_renderer = nullptr;
            }
            SDL_DestroyWindow(image_window);
            image_window = nullptr;
        }

        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        const auto &info = std::get<Core::FileInfo>(node.data);

        SDL_Surface *surface = nullptr;

        if (is_sct_format(info.format))
        {
            std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(file_data, false);
            if (png_data.empty())
            {
                status_text = "Failed to convert SCT for preview window";
                return;
            }
            SDL_RWops *rw = SDL_RWFromMem(png_data.data(), (int)png_data.size());
            surface = IMG_Load_RW(rw, 1);
        }
        else
        {
            SDL_RWops *rw = SDL_RWFromMem(file_data.data(), (int)file_data.size());
            surface = IMG_Load_RW(rw, 1);
        }

        if (!surface)
        {
            status_text = "Failed to load image for preview window";
            return;
        }

        int original_width = surface->w;
        int original_height = surface->h;

        SDL_DisplayMode display_mode;
        SDL_GetCurrentDisplayMode(0, &display_mode);
        int screen_width = display_mode.w;
        int screen_height = display_mode.h;

        int max_width = (int)(screen_width * 0.9f);
        int max_height = (int)(screen_height * 0.9f);

        image_window_width = original_width;
        image_window_height = original_height;

        if (image_window_width > max_width || image_window_height > max_height)
        {
            float scale_w = (float)max_width / original_width;
            float scale_h = (float)max_height / original_height;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;

            image_window_width = (int)(original_width * scale);
            image_window_height = (int)(original_height * scale);
        }

        image_window_title = node.name + " (" + std::to_string(original_width) + "x" + std::to_string(original_height) + ")";

        image_window = SDL_CreateWindow(
            image_window_title.c_str(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            image_window_width,
            image_window_height,
            SDL_WINDOW_SHOWN);

        if (!image_window)
        {
            SDL_FreeSurface(surface);
            status_text = "Failed to create preview window";
            return;
        }

        image_renderer = SDL_CreateRenderer(image_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!image_renderer)
        {
            SDL_FreeSurface(surface);
            SDL_DestroyWindow(image_window);
            image_window = nullptr;
            status_text = "Failed to create renderer";
            return;
        }

        image_window_texture = SDL_CreateTextureFromSurface(image_renderer, surface);
        SDL_FreeSurface(surface);

        if (!image_window_texture)
        {
            SDL_DestroyRenderer(image_renderer);
            image_renderer = nullptr;
            SDL_DestroyWindow(image_window);
            image_window = nullptr;
            status_text = "Failed to create texture";
            return;
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Error opening image window: " + std::string(e.what());
    }
}

void render_image_window()
{
    if (!image_window || !image_renderer || !image_window_texture)
        return;

    SDL_RenderClear(image_renderer);
    SDL_RenderCopy(image_renderer, image_window_texture, nullptr, nullptr);
    SDL_RenderPresent(image_renderer);
}

void export_file_as_png(const Core::FileNode &node)
{
    try
    {
        const auto &info = std::get<Core::FileInfo>(node.data);

        std::string default_name = node.name;
        size_t dot_pos = default_name.find_last_of('.');
        if (dot_pos != std::string::npos)
        {
            default_name = default_name.substr(0, dot_pos);
        }
        default_name += ".png";

        auto f = pfd::save_file("Export as PNG", default_name,
                                {"PNG Files", "*.png", "All Files", "*.*"});

        if (!f.result().empty())
        {
            std::vector<uint8_t> file_data = data_pack->GetFileData(node);
            std::vector<uint8_t> png_data;

            if (is_sct_format(info.format))
            {
                png_data = SCTParser::ConvertToPNG(file_data, false);
            }
            else
            {
                png_data = file_data;
            }

            if (!png_data.empty())
            {
                std::ofstream out(f.result(), std::ios::binary);
                out.write((const char *)png_data.data(), png_data.size());
                out.close();
                status_text = "Exported to: " + f.result();
            }
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Export error: " + std::string(e.what());
    }
}

void export_file_as_sct(const Core::FileNode &node)
{
    try
    {
        auto f = pfd::save_file("Export as SCT", node.name,
                                {"SCT Files", "*.sct;*.sct2", "All Files", "*.*"});

        if (!f.result().empty())
        {
            std::vector<uint8_t> file_data = data_pack->GetFileData(node);
            std::ofstream out(f.result(), std::ios::binary);
            out.write((const char *)file_data.data(), file_data.size());
            out.close();
            status_text = "Exported to: " + f.result();
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Export error: " + std::string(e.what());
    }
}

void handle_node_click(const Core::FileNode *node, bool is_folder)
{
    bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    Uint32 current_time = SDL_GetTicks();
    Uint32 time_diff = current_time - last_click_time;

    if (time_diff < 250 && node == last_clicked_node)
    {
        last_click_time = current_time;
        return;
    }

    click_count = 0;

    if (ctrl_pressed)
    {
        if (selected_file_nodes.find(node) != selected_file_nodes.end())
        {
            selected_file_nodes.erase(node);
        }
        else
        {
            selected_file_nodes.insert(node);
        }

        selected_node = node;
        if (!is_folder)
        {
            load_image_preview(*node);
        }
        else
        {
            has_preview = false;
            preview_error = "";
            preview_atlas_data = "";
            preview_json_data = "";
            full_atlas_data = "";
            db_column_names.clear();
            db_rows.clear();
            current_preview_mode = PreviewMode::None;
        }
    }
    else
    {
        selected_file_nodes.clear();
        selected_file_nodes.insert(node);
        selected_node = node;

        if (!is_folder)
        {
            load_image_preview(*node);
        }
        else
        {
            has_preview = false;
            preview_error = "";
            preview_atlas_data = "";
            preview_json_data = "";
            full_atlas_data = "";
            db_column_names.clear();
            db_rows.clear();
            current_preview_mode = PreviewMode::None;
        }
    }

    last_click_time = current_time;
    last_clicked_node = node;
}

void handle_node_right_click(const Core::FileNode *node, struct nk_vec2 pos)
{
    context_menu_node = node;
    context_menu_pos = pos;
    show_context_menu = true;
}

nlohmann::ordered_json build_file_tree_json(const Core::FileNode &node)
{
    nlohmann::ordered_json j;
    j["name"] = node.name;
    j["path"] = node.full_path;

    if (std::holds_alternative<Core::FileInfo>(node.data))
    {
        const auto &info = std::get<Core::FileInfo>(node.data);
        j["type"] = "file";
        j["size"] = info.size;
        j["offset"] = info.offset;
        j["format"] = info.format;
    }
    else
    {
        const auto &folder = std::get<Core::FolderInfo>(node.data);
        j["type"] = "folder";
        j["children"] = nlohmann::ordered_json::array();
        for (const auto &child : folder.children)
        {
            j["children"].push_back(build_file_tree_json(child));
        }
    }

    return j;
}

void export_to_json()
{
    try
    {
        auto f = pfd::save_file("Export File Map", "filemap.json",
                                {"JSON Files", "*.json", "All Files", "*.*"});

        if (!f.result().empty())
        {
            std::ofstream out(f.result());
            if (out.is_open())
            {
                nlohmann::ordered_json j = build_file_tree_json(data_pack->GetFileTree());
                out << j.dump(2);
                out.close();
                export_success_msg = "File map exported successfully!";
                show_export_success = true;
                status_text = "Exported to: " + f.result();
            }
        }
    }
    catch (const std::exception &e)
    {
        status_text = "Export error: " + std::string(e.what());
    }
}

void draw_file_node(nk_context *ctx, const Core::FileNode &node, int depth = 0)
{
    try
    {
        if (!has_matching_child(node, search_query))
            return;

        visible_nodes.push_back(&node);

        if (std::holds_alternative<Core::FolderInfo>(node.data))
        {
            const auto &folder = std::get<Core::FolderInfo>(node.data);
            bool is_expanded = expanded_folders.find(&node) != expanded_folders.end();
            bool is_selected = (selected_file_nodes.find(&node) != selected_file_nodes.end()) || (selected_node == &node);

            struct nk_color bg_color = (depth % 2 == 0) ? nk_rgb(35, 35, 38) : nk_rgb(40, 40, 43);
            if (is_selected)
                bg_color = nk_rgb(65, 65, 70);

            nk_layout_row_begin(ctx, NK_STATIC, 26, 4);
            nk_layout_row_push(ctx, depth * 16.0f + 10.0f);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 24.0f);
            struct nk_style_button expand_style = ctx->style.button;
            expand_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
            expand_style.hover = nk_style_item_color(nk_rgb(80, 80, 85));
            expand_style.text_normal = nk_rgb(200, 200, 200);
            expand_style.text_hover = nk_rgb(255, 255, 255);
            expand_style.rounding = 3.0f;

            if (nk_button_label_styled(ctx, &expand_style, is_expanded ? "-" : "+"))
            {
                if (is_expanded)
                {
                    expanded_folders.erase(&node);
                }
                else
                {
                    expanded_folders.insert(&node);
                }
            }

            nk_layout_row_push(ctx, 370.0f);
            struct nk_style_button button_style = ctx->style.button;
            button_style.normal = nk_style_item_color(bg_color);
            button_style.hover = nk_style_item_color(is_selected ? nk_rgb(85, 85, 95) : nk_rgb(50, 50, 55));
            button_style.active = nk_style_item_color(nk_rgb(70, 70, 80));
            button_style.text_normal = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(220, 220, 220);
            button_style.text_hover = nk_rgb(255, 255, 255);
            button_style.text_active = nk_rgb(255, 255, 255);
            button_style.text_alignment = NK_TEXT_LEFT;
            button_style.padding = nk_vec2(8, 4);
            button_style.rounding = 3.0f;

            std::string folder_label = node.name;
            bool highlight_match = !search_query.empty() && matches_search(node, search_query);
            if (highlight_match)
                button_style.text_normal = nk_rgb(100, 255, 100);

            if (nk_button_label_styled(ctx, &button_style, folder_label.c_str()))
            {
                handle_node_click(&node, true);
            }

            nk_layout_row_push(ctx, 200.0f);
            int file_count = get_file_count(node);
            std::string info = std::to_string(file_count) + " items | " + format_size(get_folder_size(node));
            nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

            nk_layout_row_end(ctx);

            if (is_expanded)
            {
                for (const auto &child : folder.children)
                    draw_file_node(ctx, child, depth + 1);
            }
        }
        else
        {
            if (!matches_search(node, search_query))
                return;

            const auto &file_info = std::get<Core::FileInfo>(node.data);
            bool is_selected = (selected_file_nodes.find(&node) != selected_file_nodes.end()) || (selected_node == &node);

            struct nk_color bg_color = (depth % 2 == 0) ? nk_rgb(35, 35, 38) : nk_rgb(40, 40, 43);
            if (is_selected)
                bg_color = nk_rgb(65, 65, 70);

            nk_layout_row_begin(ctx, NK_STATIC, 26, 4);
            nk_layout_row_push(ctx, depth * 16.0f + 10.0f);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 24.0f);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 370.0f);
            struct nk_style_button button_style = ctx->style.button;
            button_style.normal = nk_style_item_color(bg_color);
            button_style.hover = nk_style_item_color(is_selected ? nk_rgb(85, 85, 95) : nk_rgb(50, 50, 55));
            button_style.active = nk_style_item_color(nk_rgb(70, 70, 80));
            button_style.text_normal = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(200, 200, 200);
            button_style.text_hover = nk_rgb(255, 255, 255);
            button_style.text_active = nk_rgb(255, 255, 255);
            button_style.text_alignment = NK_TEXT_LEFT;
            button_style.padding = nk_vec2(8, 4);
            button_style.rounding = 3.0f;

            std::string file_label = node.name;

            if (nk_button_label_styled(ctx, &button_style, file_label.c_str()))
            {
                handle_node_click(&node, false);
            }

            if (nk_input_is_mouse_hovering_rect(&ctx->input, nk_widget_bounds(ctx)))
            {
                if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_RIGHT))
                {
                    handle_node_right_click(&node, ctx->input.mouse.pos);
                }
            }

            nk_layout_row_push(ctx, 200.0f);
            std::string size_str = format_size(file_info.size) + " | " + file_info.format;
            nk_label_colored(ctx, size_str.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

            nk_layout_row_end(ctx);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error drawing node: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Unknown error drawing node" << std::endl;
    }
}

// Recursive spine tree rendering: walks SpineCategory tree, drawing folders + leaf entries
static bool spine_category_has_search_match(const SpineCategory& cat,
                                             const std::vector<SpineEntry>& entries,
                                             const std::string& query_lower) {
    if (query_lower.empty()) return true;
    // Check direct entries
    for (size_t idx : cat.entry_indices) {
        std::string dn = entries[idx].display_name;
        std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
        if (dn.find(query_lower) != std::string::npos) return true;
        // Also match against the full category path
        std::string cp = entries[idx].category;
        std::transform(cp.begin(), cp.end(), cp.begin(), ::tolower);
        if (cp.find(query_lower) != std::string::npos) return true;
    }
    // Check subcategories recursively
    for (const auto& [name, sub] : cat.subcategories) {
        if (spine_category_has_search_match(sub, entries, query_lower)) return true;
    }
    return false;
}

static void draw_spine_category(nk_context* ctx,
                                 const SpineCategory& cat,
                                 const std::vector<SpineEntry>& entries,
                                 int depth) {
    std::string query_lower = spine_search_query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    // For each subcategory, draw as a collapsible folder
    for (const auto& [subname, sub] : cat.subcategories) {
        // Check if this subtree has any matching entries
        if (!spine_category_has_search_match(sub, entries, query_lower)) continue;

        // Use full_path as the unique key for expand/collapse
        bool expanded = spine_expanded_categories.count(sub.full_path) > 0;
        // Auto-expand when searching
        if (!query_lower.empty()) expanded = true;

        // Count total entries under this subtree recursively
        std::function<int(const SpineCategory&)> count_entries = [&](const SpineCategory& c) -> int {
            int n = (int)c.entry_indices.size();
            for (const auto& [k, sc] : c.subcategories) n += count_entries(sc);
            return n;
        };
        int total = count_entries(sub);

        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        // Indent
        float indent = depth * 16.0f;
        if (indent > 0) {
            nk_layout_row_push(ctx, indent);
            nk_spacing(ctx, 1);
        }

        nk_layout_row_push(ctx, 300.0f - indent);
        struct nk_style_button cbtn = ctx->style.button;
        cbtn.text_alignment = NK_TEXT_LEFT;
        cbtn.padding = nk_vec2(6, 3);
        cbtn.rounding = 2.0f;
        // Alternate folder colors by depth for visual hierarchy
        int shade = 50 + (depth % 3) * 5;
        cbtn.normal = nk_style_item_color(nk_rgb(shade, shade + 5, shade + 15));
        cbtn.hover = nk_style_item_color(nk_rgb(shade + 10, shade + 15, shade + 25));
        cbtn.text_normal = nk_rgb(180, 200, 230);
        cbtn.text_hover = nk_rgb(220, 230, 255);

        std::string folder_label = (expanded ? "- " : "+ ") + sub.name + " (" + std::to_string(total) + ")";
        if (nk_button_label_styled(ctx, &cbtn, folder_label.c_str())) {
            if (expanded) spine_expanded_categories.erase(sub.full_path);
            else spine_expanded_categories.insert(sub.full_path);
        }
        nk_layout_row_end(ctx);

        if (!expanded) continue;

        // Draw entries directly in this folder
        for (size_t idx : sub.entry_indices) {
            const auto& ent = entries[idx];

            if (!query_lower.empty()) {
                std::string dn = ent.display_name;
                std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
                std::string cp = ent.category;
                std::transform(cp.begin(), cp.end(), cp.begin(), ::tolower);
                if (dn.find(query_lower) == std::string::npos &&
                    cp.find(query_lower) == std::string::npos) continue;
            }

            spine_visible_indices.push_back((int)idx);

            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            float entry_indent = (depth + 1) * 16.0f;
            nk_layout_row_push(ctx, entry_indent);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 300.0f - entry_indent);
            bool isSel = ((int)idx == spine_selected_index);
            struct nk_style_button ebtn = ctx->style.button;
            ebtn.text_alignment = NK_TEXT_LEFT;
            ebtn.padding = nk_vec2(6, 3);
            ebtn.rounding = 2.0f;
            if (isSel) {
                ebtn.normal = nk_style_item_color(nk_rgb(55, 80, 120));
                ebtn.hover = nk_style_item_color(nk_rgb(65, 90, 130));
                ebtn.text_normal = nk_rgb(255, 255, 255);
            } else {
                ebtn.normal = nk_style_item_color(nk_rgb(38, 38, 42));
                ebtn.hover = nk_style_item_color(nk_rgb(50, 50, 55));
                ebtn.text_normal = nk_rgb(190, 190, 190);
            }
            ebtn.text_hover = nk_rgb(255, 255, 255);

            if (nk_button_label_styled(ctx, &ebtn, ent.display_name.c_str())) {
                if (spine_selected_index != (int)idx) {
                    spine_selected_index = (int)idx;
                    spine_anim_selected = 0;
                    spine_skin_selected = 0;
                    spine_last_tick = 0;
                    spine_edit_mode = false;
                    if (!active_spine_viewer) active_spine_viewer = std::make_unique<SpineViewer>();
                    active_spine_viewer->loadSkeleton(spine_dictionary, *data_pack, ent);
                    active_spine_viewer->setFlipX(spine_flip_x);
                    active_spine_viewer->setFlipY(spine_flip_y);
                }
            }
            nk_layout_row_end(ctx);
        }

        // Recurse into subcategories
        draw_spine_category(ctx, sub, entries, depth + 1);
    }

    // Also draw any entries directly at this level (root-level entries)
    if (depth == 0) {
        for (size_t idx : cat.entry_indices) {
            const auto& ent = entries[idx];

            if (!query_lower.empty()) {
                std::string dn = ent.display_name;
                std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
                if (dn.find(query_lower) == std::string::npos) continue;
            }

            spine_visible_indices.push_back((int)idx);

            nk_layout_row_dynamic(ctx, 24, 1);
            bool isSel = ((int)idx == spine_selected_index);
            struct nk_style_button ebtn = ctx->style.button;
            ebtn.text_alignment = NK_TEXT_LEFT;
            ebtn.padding = nk_vec2(16, 3);
            ebtn.rounding = 2.0f;
            if (isSel) {
                ebtn.normal = nk_style_item_color(nk_rgb(55, 80, 120));
                ebtn.hover = nk_style_item_color(nk_rgb(65, 90, 130));
                ebtn.text_normal = nk_rgb(255, 255, 255);
            } else {
                ebtn.normal = nk_style_item_color(nk_rgb(38, 38, 42));
                ebtn.hover = nk_style_item_color(nk_rgb(50, 50, 55));
                ebtn.text_normal = nk_rgb(190, 190, 190);
            }
            ebtn.text_hover = nk_rgb(255, 255, 255);

            if (nk_button_label_styled(ctx, &ebtn, ent.display_name.c_str())) {
                if (spine_selected_index != (int)idx) {
                    spine_selected_index = (int)idx;
                    spine_anim_selected = 0;
                    spine_skin_selected = 0;
                    spine_last_tick = 0;
                    spine_edit_mode = false;
                    if (!active_spine_viewer) active_spine_viewer = std::make_unique<SpineViewer>();
                    active_spine_viewer->loadSkeleton(spine_dictionary, *data_pack, ent);
                    active_spine_viewer->setFlipX(spine_flip_x);
                    active_spine_viewer->setFlipY(spine_flip_y);
                }
            }
        }
    }
}

// Headless CLI test: --test-spine <pack_path>
// Scans the pack, builds spine dictionary, tries loading every skeleton, reports pass/fail.
int run_spine_test(const std::string& pack_path_str) {
    // Convert to wide string for DataPack
    int sz = MultiByteToWideChar(CP_UTF8, 0, pack_path_str.c_str(), (int)pack_path_str.size(), NULL, 0);
    std::wstring wpath(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, pack_path_str.c_str(), (int)pack_path_str.size(), &wpath[0], sz);

    LogInfo("=== SPINE TEST START ===");
    std::cout << "Opening pack: " << pack_path_str << std::endl;

    auto pack = std::make_unique<DataPack>(wpath);
    if (pack->GetType() == DataPack::PackType::Unknown) {
        std::cerr << "ERROR: Invalid pack file" << std::endl;
        return 1;
    }

    std::cout << "Scanning file tree..." << std::endl;
    std::atomic<float> progress = 0;
    pack->Scan(progress);
    int total_files = get_file_count(pack->GetFileTree());
    std::cout << "Scan complete: " << total_files << " files" << std::endl;

    std::cout << "Building spine dictionary..." << std::endl;
    SpineDictionary dict;
    dict.Build(*pack, pack->GetFileTree());
    const auto& entries = dict.GetEntries();
    std::cout << "Dictionary: " << entries.size() << " spine entries" << std::endl;

    // Minimal SDL/GL init for texture loading (needed by SpineViewer)
    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("test", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    glewInit();

    int pass = 0, fail = 0;
    std::vector<std::pair<std::string, std::string>> failures;

    SpineViewer viewer;
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        bool ok = viewer.loadSkeleton(dict, *pack, e);
        if (ok) {
            // Try one frame of update+render to catch runtime crashes
            try {
                viewer.update(0.016f);
                viewer.render(64, 64);
            } catch (...) {
                ok = false;
            }
            pass++;
        } else {
            fail++;
            failures.push_back({e.display_name, viewer.getError()});
        }
        viewer.unload();

        if ((i + 1) % 500 == 0 || i == entries.size() - 1) {
            std::cout << "  " << (i+1) << "/" << entries.size()
                      << " (pass=" << pass << " fail=" << fail << ")" << std::endl;
        }
    }

    std::cout << "\n========== RESULTS ==========" << std::endl;
    std::cout << "Total:  " << entries.size() << std::endl;
    std::cout << "Pass:   " << pass << std::endl;
    std::cout << "Fail:   " << fail << std::endl;

    if (!failures.empty()) {
        std::cout << "\nFailures:" << std::endl;
        for (auto& [name, err] : failures) {
            std::cout << "  " << name << ": " << err << std::endl;
        }
    }

    LogInfo("=== SPINE TEST END: pass=" + std::to_string(pass) + " fail=" + std::to_string(fail) + " ===");

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return fail > 0 ? 1 : 0;
}

int main(int argc, char *argv[])
{
    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    SDL_Cursor *resize_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    SDL_Cursor *arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *win = SDL_CreateWindow("Chaos Zero Nightmare ASSet Ripper v1.4.0",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       INITIAL_WINDOW_WIDTH, INITIAL_WINDOW_HEIGHT,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    SDL_GLContext glContext = SDL_GL_CreateContext(win);
    glewInit();

    nk_context *ctx = nk_sdl_init(win);
    {
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
        const char *font_kr = "C:\\Windows\\Fonts\\malgun.ttf";
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
        const char *font_cn = "C:\\Windows\\Fonts\\msyh.ttc";
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
            const char *font_base = "C:\\Windows\\Fonts\\segoeui.ttf";
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
            nk_style_set_font(ctx, &font->handle);
    }

    bool running = true;
    bool scroll_to_selected = false;
    while (running)
    {
        SDL_Event evt;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt))
        {
            if (evt.type == SDL_QUIT)
            {
                running = false;
            }
            else if (evt.type == SDL_WINDOWEVENT)
            {
                if (evt.window.event == SDL_WINDOWEVENT_CLOSE)
                {
                    Uint32 windowID = evt.window.windowID;
                    if (windowID == SDL_GetWindowID(win))
                    {
                        running = false;
                    }
                    else if (image_window && windowID == SDL_GetWindowID(image_window))
                    {
                        if (image_window_texture)
                        {
                            SDL_DestroyTexture(image_window_texture);
                            image_window_texture = nullptr;
                        }
                        if (image_renderer)
                        {
                            SDL_DestroyRenderer(image_renderer);
                            image_renderer = nullptr;
                        }
                        SDL_DestroyWindow(image_window);
                        image_window = nullptr;
                    }
                }
            }
            else if (evt.type == SDL_KEYDOWN)
            {
                // Spine viewer gets priority for arrow keys when open
                if (show_spine_viewer && spine_dictionary.IsBuilt() && !spine_visible_indices.empty())
                {
                    if (evt.key.keysym.sym == SDLK_UP || evt.key.keysym.sym == SDLK_DOWN)
                    {
                        auto it = std::find(spine_visible_indices.begin(), spine_visible_indices.end(), spine_selected_index);
                        int new_idx = spine_selected_index;
                        if (evt.key.keysym.sym == SDLK_UP) {
                            if (it != spine_visible_indices.end() && it != spine_visible_indices.begin())
                                new_idx = *(it - 1);
                            else if (it == spine_visible_indices.end() && !spine_visible_indices.empty())
                                new_idx = spine_visible_indices.back();
                        } else {
                            if (it != spine_visible_indices.end() && (it + 1) != spine_visible_indices.end())
                                new_idx = *(it + 1);
                            else if (it == spine_visible_indices.end() && !spine_visible_indices.empty())
                                new_idx = spine_visible_indices.front();
                        }
                        if (new_idx != spine_selected_index) {
                            spine_selected_index = new_idx;
                            // Load skeleton on arrow key selection
                            const auto& entries = spine_dictionary.GetEntries();
                            if (spine_selected_index >= 0 && spine_selected_index < (int)entries.size()) {
                                if (!active_spine_viewer) active_spine_viewer = std::make_unique<SpineViewer>();
                                active_spine_viewer->loadSkeleton(spine_dictionary, *data_pack, entries[spine_selected_index]);
                                active_spine_viewer->setFlipX(spine_flip_x);
                                active_spine_viewer->setFlipY(spine_flip_y);
                                spine_anim_selected = 0;
                                spine_skin_selected = 0;
                                spine_last_tick = 0;
                                spine_edit_mode = false;
                            }
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_RETURN && spine_selected_index >= 0)
                    {
                        const auto& entries = spine_dictionary.GetEntries();
                        if (spine_selected_index < (int)entries.size()) {
                            if (!active_spine_viewer) active_spine_viewer = std::make_unique<SpineViewer>();
                            active_spine_viewer->loadSkeleton(spine_dictionary, *data_pack, entries[spine_selected_index]);
                            spine_anim_selected = 0;
                            spine_skin_selected = 0;
                            spine_last_tick = 0;
                            spine_edit_mode = false;
                        }
                    }
                }
                else if (selected_node)
                {
                    // File tree arrow keys (only when spine viewer is NOT open)
                    if ((evt.key.keysym.sym == SDLK_UP || evt.key.keysym.sym == SDLK_DOWN) && !visible_nodes.empty())
                    {
                        auto it = std::find(visible_nodes.begin(), visible_nodes.end(), selected_node);
                        if (it != visible_nodes.end())
                        {
                            if (evt.key.keysym.sym == SDLK_UP && it > visible_nodes.begin())
                            {
                                selected_node = *(it - 1);
                                handle_node_click(selected_node, std::holds_alternative<Core::FolderInfo>(selected_node->data));
                                scroll_to_selected = true;
                            }
                            else if (evt.key.keysym.sym == SDLK_DOWN && it < visible_nodes.end() - 1)
                            {
                                selected_node = *(it + 1);
                                handle_node_click(selected_node, std::holds_alternative<Core::FolderInfo>(selected_node->data));
                                scroll_to_selected = true;
                            }
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_RETURN)
                    {
                        if (std::holds_alternative<Core::FolderInfo>(selected_node->data))
                        {
                            if (expanded_folders.find(selected_node) != expanded_folders.end())
                                expanded_folders.erase(selected_node);
                            else
                                expanded_folders.insert(selected_node);
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_RIGHT)
                    {
                        if (std::holds_alternative<Core::FolderInfo>(selected_node->data))
                        {
                            expanded_folders.insert(selected_node);
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_LEFT)
                    {
                        if (std::holds_alternative<Core::FolderInfo>(selected_node->data))
                        {
                            expanded_folders.erase(selected_node);
                        }
                    }
                }
            }
            nk_sdl_handle_event(&evt);
        }
        nk_input_end(ctx);

        if (is_task_running && task_future.valid() &&
            task_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            is_task_running = false;
            try
            {
                task_future.get();
            }
            catch (const std::exception &e)
            {
                status_text = "Error: " + std::string(e.what());
            }
            catch (...)
            {
                status_text = "Unknown error occurred";
            }
            task_progress = 1.0f;
            if (status_text.find("Scanning") != std::string::npos)
            {
                is_scan_complete = true;
                status_text = "Scan complete. " + std::to_string(get_file_count(data_pack->GetFileTree())) + " files found.";
            }
            else if (status_text.find("Extracting") != std::string::npos)
            {
                status_text = "Extraction complete.";
            }
        }

        int window_width, window_height;
        SDL_GetWindowSize(win, &window_width, &window_height);

        if (show_context_menu && context_menu_node)
        {
            if (nk_begin(ctx, "Context Menu",
                         nk_rect(context_menu_pos.x, context_menu_pos.y, 180.0f, 200.0f),
                         NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
            {

                if (std::holds_alternative<Core::FileInfo>(context_menu_node->data))
                {
                    const auto &info = std::get<Core::FileInfo>(context_menu_node->data);

                    nk_layout_row_dynamic(ctx, 25, 1);

                    if (is_db_file(info.format))
                    {
                        if (nk_button_label(ctx, "Export as JSON"))
                        {
                            export_db_as_json_file(*context_menu_node);
                            show_context_menu = false;
                        }
                    }
                    else if (is_scsp_file(info.format))
                    {
                        if (nk_button_label(ctx, "Export as JSON"))
                        {
                            export_scsp_as_json_file(*context_menu_node);
                            show_context_menu = false;
                        }
                    }
                    else if (is_sct_format(info.format))
                    {
                        if (nk_button_label(ctx, "Export as PNG"))
                        {
                            export_file_as_png(*context_menu_node);
                            show_context_menu = false;
                        }
                        if (nk_button_label(ctx, "Export as SCT"))
                        {
                            export_file_as_sct(*context_menu_node);
                            show_context_menu = false;
                        }
                        if (nk_button_label(ctx, "Open Preview Window"))
                        {
                            open_image_preview_window(*context_menu_node);
                            show_context_menu = false;
                        }
                    }
                    else if (is_previewable_format(info.format))
                    {
                        if (nk_button_label(ctx, "Export as PNG"))
                        {
                            export_file_as_png(*context_menu_node);
                            show_context_menu = false;
                        }
                        if (nk_button_label(ctx, "Open Preview Window"))
                        {
                            open_image_preview_window(*context_menu_node);
                            show_context_menu = false;
                        }
                    }

                    if (nk_button_label(ctx, "Extract Raw"))
                    {
                        try
                        {
                            auto f = pfd::save_file("Extract File", context_menu_node->name, {"All Files", "*.*"});
                            if (!f.result().empty())
                            {
                                std::vector<uint8_t> file_data = data_pack->GetFileData(*context_menu_node);
                                std::ofstream out(f.result(), std::ios::binary);
                                out.write((const char *)file_data.data(), file_data.size());
                                out.close();
                                status_text = "Extracted to: " + f.result();
                            }
                        }
                        catch (...)
                        {
                        }
                        show_context_menu = false;
                    }
                }

                if (nk_button_label(ctx, "Close"))
                {
                    show_context_menu = false;
                }
            }
            else
            {
                show_context_menu = false;
            }
            nk_end(ctx);
        }

        if (show_export_options_window)
        {
            const float export_options_width = 530.0f;
            const float export_options_height = 480.0f;
            const float export_options_x = (window_width - export_options_width) * 0.5f;
            const float export_options_y = (window_height - export_options_height) * 0.5f;
            if (nk_begin(ctx, "Export Options", nk_rect(export_options_x, export_options_y, export_options_width, export_options_height),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE))
            {

                nk_layout_row_dynamic(ctx, 25, 1);
                nk_label(ctx, "Configure extraction options:", NK_TEXT_LEFT);

                nk_layout_row_begin(ctx, NK_STATIC, 32, 2);
                nk_layout_row_push(ctx, 380);
                nk_label(ctx, "Convert SCT files to PNG", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 120);
                {
                    struct nk_style_button toggle_style = ctx->style.button;
                    if (export_sct_as_png)
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
                    if (nk_button_label_styled(ctx, &toggle_style, export_sct_as_png ? "ON" : "OFF"))
                    {
                        export_sct_as_png = export_sct_as_png ? nk_false : nk_true;
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
                    struct nk_style_button toggle_style = ctx->style.button;
                    if (export_db_as_json)
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
                    if (nk_button_label_styled(ctx, &toggle_style, export_db_as_json ? "ON" : "OFF"))
                    {
                        export_db_as_json = export_db_as_json ? nk_false : nk_true;
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
                    struct nk_style_button toggle_style = ctx->style.button;
                    if (enable_open_folder)
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
                    if (nk_button_label_styled(ctx, &toggle_style, enable_open_folder ? "ON" : "OFF"))
                    {
                        enable_open_folder = enable_open_folder ? nk_false : nk_true;
                    }
                }
                nk_layout_row_end(ctx);

                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "When enabled, open folder button show up", NK_TEXT_LEFT);
                nk_label(ctx, "letting user choose a folder to scan instead of only data.pack", NK_TEXT_LEFT);


                nk_layout_row_dynamic(ctx, 25, 1);
                std::string status1 = export_sct_as_png ? "SCT to PNG: ENABLED" : "SCT to PNG: DISABLED";
                nk_label_colored(ctx, status1.c_str(), NK_TEXT_LEFT,
                                 export_sct_as_png ? nk_rgb(100, 255, 100) : nk_rgb(255, 150, 150));

                std::string status2 = export_db_as_json ? "DB to JSON: ENABLED" : "DB to JSON: DISABLED";
                nk_label_colored(ctx, status2.c_str(), NK_TEXT_LEFT,
                                 export_db_as_json ? nk_rgb(100, 255, 100) : nk_rgb(255, 150, 150));

                nk_layout_row_dynamic(ctx, 30, 2);
                if (nk_button_label(ctx, "OK"))
                {
                    show_export_options_window = false;
                    status_text = "Options saved";
                }
                if (nk_button_label(ctx, "Cancel"))
                {
                    show_export_options_window = false;
                }
            }
            else
            {
                show_export_options_window = false;
            }
            nk_end(ctx);
        }

        if (show_credits_window)
        {
            const float credits_options_width = 700.0f;
            const float credits_options_height = 300.0f;
            const float credits_options_x = (window_width - credits_options_width) * 0.5f;
            const float credits_options_y = (window_height - credits_options_height) * 0.5f;
            if (nk_begin(ctx, "Credits", nk_rect(credits_options_x, credits_options_y, credits_options_width, credits_options_height),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE |
                             NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
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
                    show_credits_window = false;
                }
            }
            else
            {
                show_credits_window = false;
            }
            nk_end(ctx);
        }

        if (show_export_success)
        {
            const float export_success_width = 215.0f;
            const float export_success_height = 120.0f;
            const float export_success_x = (window_width - export_success_width) * 0.5f;
            const float export_success_y = (window_height - export_success_height) * 0.5f;
            if (nk_begin(ctx, "Success", nk_rect(export_success_x, export_success_y, export_success_width, export_success_height),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE))
            {

                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, export_success_msg.c_str(), NK_TEXT_CENTERED);
                nk_layout_row_dynamic(ctx, 30, 1);
                if (nk_button_label(ctx, "OK"))
                {
                    show_export_success = false;
                }
            }
            else
            {
                show_export_success = false;
            }
            nk_end(ctx);
        }

        if (show_atlas_window && !preview_atlas_data.empty())
        {
            if (nk_begin(ctx, "Atlas Viewer", nk_rect(40, 40, (float)window_width - 80, (float)window_height - 80),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
            {

                nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
                nk_layout_row_push(ctx, 120);
                if (nk_button_label(ctx, "Copy All"))
                {
                    SDL_SetClipboardText(full_atlas_data.c_str());
                }
                nk_layout_row_push(ctx, 120);
                if (nk_button_label(ctx, "Save As..."))
                {
                    try
                    {
                        auto f = pfd::save_file("Save Atlas Text", "atlas.txt", {"Text", "*.txt", "All Files", "*.*"});
                        if (!f.result().empty())
                        {
                            std::ofstream out(f.result());
                            if (out.is_open())
                            {
                                out << full_atlas_data;
                                out.close();
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                }
                nk_layout_row_end(ctx);

                if (atlas_text_buf.empty())
                {
                    atlas_text_buf.push_back('\0');
                }
                nk_layout_row_dynamic(ctx, (float)window_height - 180, 1);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY, atlas_text_buf.data(), (int)atlas_text_buf.size(), nk_filter_default);
            }
            else
            {
                show_atlas_window = false;
            }
            nk_end(ctx);
        }

        if (nk_begin(ctx, "Main", nk_rect(0, 0, (float)window_width, (float)window_height), NK_WINDOW_NO_SCROLLBAR))
        {
            bool pack_loaded = (data_pack != nullptr);
            bool tree_scanned = pack_loaded && is_scan_complete.load();
            bool selection_exists = (selected_node != nullptr);
            bool has_file_selection = !selected_file_nodes.empty();
            bool has_extract_selection = has_file_selection || (selected_node != nullptr);

            nk_layout_row_dynamic(ctx, 38, enable_open_folder ? 9 : 8);

            struct nk_style_button btn_style = ctx->style.button;
            btn_style.rounding = 4.0f;
            btn_style.padding = nk_vec2(10, 8);
            btn_style.normal = nk_style_item_color(nk_rgb(70, 70, 75));
            btn_style.hover = nk_style_item_color(nk_rgb(90, 90, 95));

            bool pack_already_loaded = (data_pack != nullptr);
            bool can_open_pack = !is_task_running && !pack_already_loaded;

            if (can_open_pack && nk_button_label_styled(ctx, &btn_style, "Open Pack"))
            {
                try
                {
                    auto f = pfd::open_file("Select a data.pack file", ".",
                                            {"Pack Files", "*.pack", "All Files", "*.*"});
                    if (!f.result().empty())
                    {
                        std::string selected_path = f.result()[0];
                        int size_needed = MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                                                              (int)selected_path.size(), NULL, 0);
                        std::wstring wpath(size_needed, 0);
                        MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                                            (int)selected_path.size(), &wpath[0], size_needed);

                        data_pack.reset();
                        is_scan_complete = false;
                        selected_node = nullptr;
                        selected_file_nodes.clear();
                        expanded_folders.clear();
                        has_preview = false;
                        preview_error = "";
                        preview_atlas_data = "";
                        preview_json_data = "";
                        full_atlas_data = "";
                        db_column_names.clear();
                        db_rows.clear();
                        search_query = "";
                        memset(search_buffer, 0, sizeof(search_buffer));
                        current_preview_mode = PreviewMode::None;
                        if (spine_build_future.valid()) spine_build_future.wait();
                        spine_dictionary.Clear();
                        show_spine_viewer = false;
                        spine_selected_index = -1;
                        memset(spine_search_buffer, 0, sizeof(spine_search_buffer));
                        spine_search_query = "";
                        if (active_spine_viewer) active_spine_viewer->unload();
                        active_spine_viewer.reset();
                        spine_expanded_categories.clear();

                        data_pack = std::make_unique<DataPack>(wpath);
                        if (data_pack->GetType() == DataPack::PackType::Unknown)
                        {
                            status_text = "Error: Invalid or unknown file.";
                            data_pack = nullptr;
                        }
                        else
                        {
                            status_text = "Loaded. Click 'Scan Tree' to analyze contents.";
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    status_text = "Error opening file: " + std::string(e.what());
                }
            }
            else if (is_task_running || pack_already_loaded)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Open Pack");
                nk_widget_disable_end(ctx);
            }

            if (enable_open_folder)
            {
                if (can_open_pack && nk_button_label_styled(ctx, &btn_style, "Open Folder"))
                {
                    try
                    {
                        auto f = pfd::select_folder("Select a folder to view", ".");
                        if (!f.result().empty())
                        {
                            std::string selected_path = f.result();
                            int size_needed = MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                                                                  (int)selected_path.size(), NULL, 0);
                            std::wstring wpath(size_needed, 0);
                            MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                                                (int)selected_path.size(), &wpath[0], size_needed);

                            data_pack.reset();
                            is_scan_complete = false;
                            selected_node = nullptr;
                            selected_file_nodes.clear();
                            expanded_folders.clear();
                            has_preview = false;
                            preview_error = "";
                            preview_atlas_data = "";
                            preview_json_data = "";
                            full_atlas_data = "";
                            db_column_names.clear();
                            db_rows.clear();
                            search_query = "";
                            memset(search_buffer, 0, sizeof(search_buffer));
                            current_preview_mode = PreviewMode::None;
                            if (spine_build_future.valid()) spine_build_future.wait();
                            spine_dictionary.Clear();
                            show_spine_viewer = false;
                            spine_selected_index = -1;
                            memset(spine_search_buffer, 0, sizeof(spine_search_buffer));
                            spine_search_query = "";
                            if (active_spine_viewer) active_spine_viewer->unload();
                            active_spine_viewer.reset();
                            spine_expanded_categories.clear();

                            data_pack = std::make_unique<DataPack>(wpath);
                            if (data_pack->GetType() == DataPack::PackType::Unknown)
                            {
                                status_text = "Error: Invalid or unknown folder.";
                                data_pack = nullptr;
                            }
                            else
                            {
                                status_text = "Folder Loaded. Click 'Scan Tree' to build the view.";
                            }
                        }
                    }
                    catch (const std::exception &e)
                    {
                        status_text = "Error opening folder: " + std::string(e.what());
                    }
                }
                else if (is_task_running || pack_already_loaded)
                {
                    nk_widget_disable_begin(ctx);
                    nk_button_label_styled(ctx, &btn_style, "Open Folder");
                    nk_widget_disable_end(ctx);
                }
            }

            if (pack_loaded && !tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Scan Tree"))
            {
                try
                {
                    is_task_running = true;
                    is_scan_complete = false;
                    status_text = "Scanning...";
                    task_progress = 0.0f;

                    expanded_folders.clear();
                    selected_node = nullptr;
                    selected_file_nodes.clear();
                    last_clicked_node = nullptr;
                    has_preview = false;
                    preview_error = "";
                    preview_atlas_data = "";
                    preview_json_data = "";
                    full_atlas_data = "";
                    db_column_names.clear();
                    db_rows.clear();
                    current_preview_mode = PreviewMode::None;

                    if (preview_texture)
                    {
                        glDeleteTextures(1, &preview_texture);
                        preview_texture = 0;
                    }
                    task_future = std::async(std::launch::async, []
                                             {
                        try {
                            data_pack->Scan(task_progress);
                        }
                        catch (...) {} });
                }
                catch (const std::exception &e)
                {
                    status_text = "Error starting scan: " + std::string(e.what());
                    is_task_running = false;
                }
            }
            else if (!pack_loaded || tree_scanned || is_task_running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Scan Tree");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, show_spine_viewer ? "File Tree" : "Spine Viewer"))
            {
                if (!show_spine_viewer) {
                    if (!spine_dictionary.IsBuilt() && !spine_building) {
                        spine_building = true;
                        spine_build_future = std::async(std::launch::async, []() {
                            try {
                                spine_dictionary.Build(*data_pack, data_pack->GetFileTree());
                            } catch (...) {}
                            spine_building = false;
                        });
                    }
                    show_spine_viewer = true;
                } else {
                    show_spine_viewer = false;
                }
            }
            else if (!tree_scanned || is_task_running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Spine Viewer");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !show_spine_viewer && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Extract All"))
            {
                try
                {
                    auto d = pfd::select_folder("Select destination folder", ".");
                    if (!d.result().empty())
                    {
                        std::string dest_str = d.result();
                        std::wstring dest_path(dest_str.begin(), dest_str.end());
                        is_task_running = true;
                        status_text = "Extracting all files...";
                        task_progress = 0.0f;
                        bool convert_sct = (export_sct_as_png != 0);
                        bool convert_db = (export_db_as_json != 0);
                        task_future = std::async(std::launch::async, [dest_path, convert_sct, convert_db]()
                                                 {
                            try {
                                data_pack->Extract(data_pack->GetFileTree(), dest_path, task_progress, convert_sct, convert_db);
                            }
                            catch (...) {} });
                    }
                }
                catch (const std::exception &e)
                {
                    status_text = "Error starting extraction: " + std::string(e.what());
                }
            }
            else if (!tree_scanned || is_task_running || show_spine_viewer)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract All");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !show_spine_viewer && has_extract_selection && !is_task_running &&
                nk_button_label_styled(ctx, &btn_style, "Extract Selected"))
            {

                try
                {
                    auto d = pfd::select_folder("Select destination folder", ".");
                    if (!d.result().empty())
                    {
                        std::string dest_str = d.result();
                        std::wstring dest_path(dest_str.begin(), dest_str.end());
                        is_task_running = true;
                        std::vector<const Core::FileNode *> nodes_to_extract;
                        nodes_to_extract.reserve(selected_file_nodes.size() + 1);
                        for (const auto *n : selected_file_nodes)
                        {
                            if (n)
                                nodes_to_extract.push_back(n);
                        }
                        if (nodes_to_extract.empty() && selected_node)
                        {
                            nodes_to_extract.push_back(selected_node);
                        }

                        status_text = "Extracting " + std::to_string(nodes_to_extract.size()) + " item(s)...";
                        task_progress = 0.0f;
                        bool convert_sct = (export_sct_as_png != 0);
                        bool convert_db = (export_db_as_json != 0);
                        task_future = std::async(std::launch::async, [dest_path, nodes_to_extract, convert_sct, convert_db]()
                                                 {
                            try {
                                const float total = nodes_to_extract.empty() ? 1.0f : (float)nodes_to_extract.size();
                                for (size_t i = 0; i < nodes_to_extract.size(); i++)
                                {
                                    std::atomic<float> local_progress = 0.0f;
                                    data_pack->Extract(*nodes_to_extract[i], dest_path, local_progress, convert_sct, convert_db);
                                    task_progress = (float)(i + 1) / total;
                                }
                            }
                            catch (...) {} });
                    }
                }
                catch (const std::exception &e)
                {
                    status_text = "Error starting extraction: " + std::string(e.what());
                }
            }
            else if (!tree_scanned || !has_extract_selection || is_task_running || show_spine_viewer)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract Selected");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Export filemap JSON"))
            {
                export_to_json();
            }
            else if (!tree_scanned || is_task_running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Export filemap JSON");
                nk_widget_disable_end(ctx);
            }

            if (!is_task_running && nk_button_label_styled(ctx, &btn_style, "Options"))
            {
                show_export_options_window = true;
            }
            else if (is_task_running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Options");
                nk_widget_disable_end(ctx);
            }



            if (nk_button_label_styled(ctx, &btn_style, "Credits"))
            {
                show_credits_window = true;
            }

            float content_height = (float)window_height - 85;

          if (!show_spine_viewer) {
            nk_layout_row_begin(ctx, NK_STATIC, 30, 2);
            nk_layout_row_push(ctx, 80);
            nk_label(ctx, "Search:", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 300);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, search_buffer, sizeof(search_buffer), nk_filter_default);
            search_query = search_buffer;
            nk_layout_row_end(ctx);
            bool showing_preview_panel = (current_preview_mode != PreviewMode::None || !preview_error.empty());

            static float sidebar_width = 600.0f;
            static bool dragging_splitter = false;
            float min_sidebar = 300.0f;
            float max_sidebar = (float)window_width * 0.7f;
            if (sidebar_width < min_sidebar)
                sidebar_width = min_sidebar;
            if (sidebar_width > max_sidebar)
                sidebar_width = max_sidebar;

            float left_width = showing_preview_panel ? sidebar_width : (float)window_width - 20.0f;
            float right_width = (float)window_width - left_width - 30.0f;

            nk_layout_row_begin(ctx, NK_STATIC, content_height, (showing_preview_panel) ? 3 : 1);
            nk_layout_row_push(ctx, left_width);

            if (nk_group_begin(ctx, "FileTree", NK_WINDOW_BORDER | NK_WINDOW_TITLE))
            {
                visible_nodes.clear();

                if (data_pack && tree_scanned)
                {
                    draw_file_node(ctx, data_pack->GetFileTree());

                    if (scroll_to_selected && selected_node)
                    {
                        auto it = std::find(visible_nodes.begin(), visible_nodes.end(), selected_node);
                        if (it != visible_nodes.end())
                        {
                            int index = std::distance(visible_nodes.begin(), it);
                            nk_uint current_x, current_y;
                            nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

                            float row_height = 26.0f + ctx->style.window.spacing.y;
                            float node_y = index * row_height;
                            float view_h = nk_window_get_content_region(ctx).h;

                            // keep the selected row inside a safe visible band so keyboard navigation
                            // doesn't outrun scrolling when moving downward quickly.
                            float top_margin = row_height * 2.0f;
                            float bottom_margin = row_height * 2.0f;
                            float visible_top = (float)current_y + top_margin;
                            float visible_bottom = (float)current_y + view_h - bottom_margin;

                            if (node_y < visible_top)
                            {
                                float target = node_y - top_margin;
                                if (target < 0.0f)
                                    target = 0.0f;
                                nk_group_set_scroll(ctx, "FileTree", current_x, (nk_uint)target);
                            }
                            else if (node_y + row_height > visible_bottom)
                            {
                                float target = node_y + row_height - view_h + bottom_margin;
                                if (target < 0.0f)
                                    target = 0.0f;
                                nk_group_set_scroll(ctx, "FileTree", current_x, (nk_uint)target);
                            }
                        }
                        scroll_to_selected = false;
                    }
                }
                else if (data_pack && is_task_running)
                {
                    nk_layout_row_begin(ctx, NK_STATIC, 26, 4);
                    nk_layout_row_push(ctx, 10.0f);
                    nk_spacing(ctx, 1);

                    nk_layout_row_push(ctx, 24.0f);
                    struct nk_style_button expand_style = ctx->style.button;
                    expand_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                    expand_style.hover = nk_style_item_color(nk_rgb(60, 60, 65));
                    expand_style.text_normal = nk_rgb(150, 150, 150);
                    expand_style.rounding = 3.0f;
                    
                    nk_widget_disable_begin(ctx);
                    nk_button_label_styled(ctx, &expand_style, "+");
                    nk_widget_disable_end(ctx);

                    nk_layout_row_push(ctx, 370.0f);
                    struct nk_style_button button_style = ctx->style.button;
                    button_style.normal = nk_style_item_color(nk_rgb(35, 35, 38));
                    button_style.hover = nk_style_item_color(nk_rgb(35, 35, 38));
                    button_style.active = nk_style_item_color(nk_rgb(35, 35, 38));
                    button_style.text_normal = nk_rgb(220, 220, 220);
                    button_style.text_alignment = NK_TEXT_LEFT;
                    button_style.padding = nk_vec2(8, 4);
                    button_style.rounding = 3.0f;
                    nk_button_label_styled(ctx, &button_style, data_pack->GetFileTree().name.c_str());

                    nk_layout_row_push(ctx, 200.0f);
                    std::string info = std::to_string(data_pack->GetParsedFileCount()) + " items | " + format_size(data_pack->GetParsedTotalSize());
                    nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));
                    nk_layout_row_end(ctx);
                }
                else if (data_pack)
                {
                    nk_layout_row_dynamic(ctx, 25, 1);
                    nk_label(ctx, "Click 'Scan Tree' to load files...", NK_TEXT_CENTERED);
                }
                else
                {
                    nk_layout_row_dynamic(ctx, 25, 1);
                    nk_label(ctx, "No pack file loaded.", NK_TEXT_CENTERED);
                }
                nk_group_end(ctx);
            }

            if (showing_preview_panel)
            {
                struct nk_rect bounds;
                nk_layout_row_push(ctx, 8.0f);
                bounds = nk_widget_bounds(ctx);
                nk_input *in = &ctx->input;

                bool hovering_splitter = nk_input_is_mouse_hovering_rect(in, bounds);
                bool mouse_down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT);

                if (hovering_splitter && mouse_down && !dragging_splitter)
                {
                    dragging_splitter = true;
                }

                if (!mouse_down)
                {
                    dragging_splitter = false;
                }

                if (dragging_splitter || hovering_splitter)
                {
                    SDL_SetCursor(resize_cursor);
                }
                else
                {
                    SDL_SetCursor(arrow_cursor);
                }

                if (dragging_splitter)
                {
                    sidebar_width += ctx->input.mouse.delta.x;
                }

                // Draw splitter handle
                nk_fill_rect(&ctx->current->buffer, bounds, 0, nk_rgb(40, 40, 45));
                nk_stroke_line(&ctx->current->buffer, bounds.x + 4, bounds.y + 10, bounds.x + 4, bounds.y + bounds.h - 10, 1.0f, nk_rgb(100, 100, 100));

                nk_layout_row_push(ctx, right_width - 8.0f);
                if (nk_group_begin(ctx, "Preview", NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR))
                {

                    if (current_preview_mode == PreviewMode::Image)
                    {
                        nk_layout_row_dynamic(ctx, 30, 1);
                        if (selected_node)
                        {
                            std::string title = "Preview: " + selected_node->name;
                            nk_label(ctx, title.c_str(), NK_TEXT_CENTERED);
                        }

                        nk_layout_row_dynamic(ctx, 25, 1);
                        std::string dims = std::to_string(preview_width) + " x " + std::to_string(preview_height);
                        nk_label_colored(ctx, dims.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

                        if (selected_node && std::holds_alternative<Core::FileInfo>(selected_node->data))
                        {
                            const auto &info = std::get<Core::FileInfo>(selected_node->data);
                            nk_layout_row_dynamic(ctx, 25, 1);
                            std::string size_str = "Size: " + format_size(info.size);
                            nk_label_colored(ctx, size_str.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));
                        }

                        if (selected_node && std::holds_alternative<Core::FileInfo>(selected_node->data))
                        {
                            const auto &info = std::get<Core::FileInfo>(selected_node->data);
                            if (is_previewable_format(info.format))
                            {
                                nk_layout_row_dynamic(ctx, 30, 1);
                                if (nk_button_label(ctx, "Open in Window"))
                                {
                                    open_image_preview_window(*selected_node);
                                }
                            }
                        }

                        float max_preview_width = right_width - 40.0f;
                        float max_preview_height = content_height - 180.0f;

                        float scale_w = max_preview_width / preview_width;
                        float scale_h = max_preview_height / preview_height;
                        float scale = (scale_w < scale_h) ? scale_w : scale_h;
                        if (scale > 1.0f)
                            scale = 1.0f;

                        float display_width = preview_width * scale;
                        float display_height = preview_height * scale;

                        nk_layout_row_begin(ctx, NK_STATIC, display_height, 1);
                        nk_layout_row_push(ctx, display_width);
                        struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                        struct nk_rect bounds = nk_widget_bounds(ctx);
                        struct nk_image img = nk_image_id((int)preview_texture);
                        nk_draw_image(canvas, bounds, &img, nk_rgb(255, 255, 255));
                        nk_layout_row_end(ctx);
                    }
                    else if (current_preview_mode == PreviewMode::DB)
                    {
                        if (selected_node)
                        {
                            nk_layout_row_dynamic(ctx, 30, 1);
                            std::string title = "Database Preview: " + db_filename;
                            nk_label_colored(ctx, title.c_str(), NK_TEXT_CENTERED, nk_rgb(150, 200, 255));
                        }

                        nk_layout_row_dynamic(ctx, 25, 1);
                        std::string stats = std::to_string(db_rows.size()) + " rows x " + std::to_string(db_column_names.size()) + " columns";
                        nk_label_colored(ctx, stats.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

                        nk_layout_row_dynamic(ctx, 30, 1);
                        if (nk_button_label(ctx, "Export as JSON"))
                        {
                            export_db_as_json_file(*selected_node);
                        }

                        nk_layout_row_dynamic(ctx, 25, 1);
                        nk_label_colored(ctx, "Preview:", NK_TEXT_LEFT, nk_rgb(200, 200, 200));

                        static char db_search_buffer[128] = "";
                        nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
                        nk_layout_row_push(ctx, 80);
                        nk_label(ctx, "Search:", NK_TEXT_LEFT);
                        nk_layout_row_push(ctx, right_width - 100);
                        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, db_search_buffer, sizeof(db_search_buffer), nk_filter_default);
                        nk_layout_row_end(ctx);

                        float preview_table_height = content_height - 230;
                        nk_layout_row_dynamic(ctx, preview_table_height, 1);

                        if (nk_group_begin(ctx, "DBPreviewTable", NK_WINDOW_BORDER))
                        {
                            float base_width = 100.0f;
                            std::vector<float> col_widths(db_column_names.size(), base_width);
                            for (size_t j = 0; j < db_column_names.size(); j++)
                            {
                                size_t max_len = db_column_names[j].length();
                                for (const auto &row : db_rows)
                                {
                                    if (j < row.size() && row[j].length() > max_len)
                                        max_len = row[j].length();
                                }
                                col_widths[j] = std::min(std::max(7.5f * max_len, 120.0f), 400.0f);
                            }

                            float index_col_width = 60.0f;

                            nk_layout_row_begin(ctx, NK_STATIC, 40, (int)db_column_names.size() + 1);

                            nk_layout_row_push(ctx, index_col_width);
                            struct nk_rect bounds = nk_widget_bounds(ctx);
                            nk_fill_rect(&ctx->current->buffer, bounds, 0, nk_rgb(60, 70, 90));
                            nk_label_colored(ctx, "#", NK_TEXT_CENTERED, nk_rgb(220, 230, 255));

                            for (size_t j = 0; j < db_column_names.size(); j++)
                            {
                                nk_layout_row_push(ctx, col_widths[j]);
                                struct nk_rect col_bounds = nk_widget_bounds(ctx);
                                nk_fill_rect(&ctx->current->buffer, col_bounds, 0, nk_rgb(60, 70, 90));
                                nk_label_colored(ctx, db_column_names[j].c_str(), NK_TEXT_CENTERED, nk_rgb(220, 230, 255));
                            }
                            nk_layout_row_end(ctx);

                            size_t preview_rows = db_rows.size();
                            int visible_index = 1;
                            int rows_shown = 0;
                            for (size_t i = 0; i < preview_rows; i++)
                            {
                                if (rows_shown > 200)
                                    break; // rows limit for performance

                                bool match = false;
                                if (strlen(db_search_buffer) == 0)
                                {
                                    match = true;
                                }
                                else
                                {
                                    std::string q = db_search_buffer;
                                    for (const auto &cell : db_rows[i])
                                    {
                                        if (cell.find(q) != std::string::npos)
                                        {
                                            match = true;
                                            break;
                                        }
                                    }
                                }

                                if (!match)
                                    continue;
                                rows_shown++;

                                struct nk_color row_color = (visible_index % 2 == 0) ? nk_rgb(45, 45, 50) : nk_rgb(40, 40, 45);
                                nk_layout_row_begin(ctx, NK_STATIC, 38, (int)db_column_names.size() + 1);

                                nk_layout_row_push(ctx, index_col_width);
                                struct nk_rect index_bounds = nk_widget_bounds(ctx);
                                nk_fill_rect(&ctx->current->buffer, index_bounds, 0, row_color);

                                std::string row_index = std::to_string(visible_index++);
                                nk_label_colored(ctx, row_index.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 200, 255));

                                for (size_t j = 0; j < db_rows[i].size(); j++)
                                {
                                    nk_layout_row_push(ctx, col_widths[j]);
                                    struct nk_rect cell_bounds = nk_widget_bounds(ctx);
                                    nk_fill_rect(&ctx->current->buffer, cell_bounds, 0, row_color);

                                    std::string cell_text = db_rows[i][j];
                                    int max_chars = (int)(col_widths[j] / 7);
                                    if (cell_text.length() > max_chars)
                                        cell_text = cell_text.substr(0, max_chars - 3) + "...";

                                    nk_label_colored(ctx, cell_text.c_str(), NK_TEXT_LEFT, nk_rgb(200, 200, 200));
                                }
                                nk_layout_row_end(ctx);
                            }

                            if (db_rows.size() > 200)
                            {
                                nk_layout_row_dynamic(ctx, 20, 1);
                                nk_label_colored(ctx, "... (preview limit reached)", NK_TEXT_LEFT, nk_rgb(255, 100, 100));
                            }

                            nk_group_end(ctx);
                        }
                    }
                    else if (current_preview_mode == PreviewMode::JSON)
                    {
                        nk_layout_row_dynamic(ctx, 25, 1);
                        nk_label(ctx, "JSON Viewer", NK_TEXT_CENTERED);

                        bool is_db_source = selected_node && std::holds_alternative<Core::FileInfo>(selected_node->data) && is_db_file(std::get<Core::FileInfo>(selected_node->data).format);
                        bool is_scsp_source = selected_node && std::holds_alternative<Core::FileInfo>(selected_node->data) && is_scsp_file(std::get<Core::FileInfo>(selected_node->data).format);
                        bool is_json_source = selected_node && std::holds_alternative<Core::FileInfo>(selected_node->data) && is_json_file(std::get<Core::FileInfo>(selected_node->data).format);

                        if (is_scsp_source)
                        {
                            nk_layout_row_dynamic(ctx, 30, 1);
                            if (nk_button_label(ctx, "Export as JSON"))
                            {
                                export_scsp_as_json_file(*selected_node);
                            }
                        }
                        else
                        {
                            nk_layout_row_begin(ctx, NK_STATIC, 30, 3);

                            if (is_db_source || is_json_source)
                            {
                                nk_layout_row_push(ctx, 120);
                                if (nk_button_label(ctx, "Export as JSON"))
                                {
                                    if (is_db_source)
                                    {
                                        export_db_as_json_file(*selected_node);
                                    }
                                    else
                                    {
                                        export_json_file(*selected_node);
                                    }
                                }
                            }
                            else
                            {
                                nk_layout_row_push(ctx, 120);
                                nk_label(ctx, "", NK_TEXT_LEFT);
                            }

                            nk_layout_row_push(ctx, 120);
                            if (nk_button_label(ctx, "Copy All"))
                            {
                                SDL_SetClipboardText(preview_json_data.c_str());
                            }
                            nk_layout_row_push(ctx, 120);
                            if (nk_button_label(ctx, "Save As..."))
                            {
                                try
                                {
                                    std::string default_name = selected_node ? selected_node->name : "output";
                                    size_t dot_pos = default_name.find_last_of('.');
                                    if (dot_pos != std::string::npos)
                                    {
                                        default_name = default_name.substr(0, dot_pos);
                                    }
                                    default_name += ".json";

                                    auto f = pfd::save_file("Save JSON", default_name,
                                                            {"JSON Files", "*.json", "All Files", "*.*"});

                                    if (!f.result().empty())
                                    {
                                        std::ofstream out(f.result());
                                        if (out.is_open())
                                        {
                                            out << preview_json_data;
                                            out.close();
                                            status_text = "Saved to: " + f.result();
                                        }
                                    }
                                }
                                catch (...)
                                {
                                }
                            }
                            nk_layout_row_end(ctx);
                        }

                        nk_layout_row_dynamic(ctx, content_height - 130, 1);
                        if (nk_group_begin(ctx, "JsonPreview", NK_WINDOW_BORDER))
                        {
                            std::stringstream ss(preview_json_data);
                            std::string line;
                            int line_count = 0;
                            while (std::getline(ss, line))
                            {
                                if (line_count > 500)
                                {
                                    nk_layout_row_dynamic(ctx, 20, 1);
                                    nk_label_colored(ctx, "... (preview limit reached)", NK_TEXT_LEFT, nk_rgb(255, 100, 100));
                                    break;
                                }

                                nk_layout_row_dynamic(ctx, 20, 1);
                                nk_label_colored(ctx, line.c_str(), NK_TEXT_LEFT, nk_rgb(220, 220, 220));
                                line_count++;
                            }
                            nk_group_end(ctx);
                        }
                    }
                    else if (current_preview_mode == PreviewMode::Text)
                    {
                        nk_layout_row_dynamic(ctx, 25, 1);
                        nk_label(ctx, "Text Viewer", NK_TEXT_CENTERED);

                        nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
                        nk_layout_row_push(ctx, 120);
                        if (nk_button_label(ctx, "Copy All"))
                        {
                            const std::string &data_to_copy = full_atlas_data.empty() ? preview_atlas_data : full_atlas_data;
                            SDL_SetClipboardText(data_to_copy.c_str());
                        }
                        nk_layout_row_push(ctx, 120);
                        if (nk_button_label(ctx, "Save As..."))
                        {
                            try
                            {
                                std::string default_name = selected_node ? selected_node->name : "output";
                                size_t dot_pos = default_name.find_last_of('.');
                                if (dot_pos != std::string::npos)
                                {
                                    default_name = default_name.substr(0, dot_pos);
                                }

                                std::string data_to_save = full_atlas_data.empty() ? preview_atlas_data : full_atlas_data;
                                bool is_atlas = data_to_save.find("format: ") != std::string::npos &&
                                                data_to_save.find("filter: ") != std::string::npos;

                                if (is_atlas)
                                {
                                    default_name += ".atlas";

                                    // replace .sct with .png in the atlas content
                                    // this so user dont have to do it manually
                                    size_t pos = 0;
                                    while ((pos = data_to_save.find(".sct", pos)) != std::string::npos)
                                    {
                                        data_to_save.replace(pos, 4, ".png");
                                        pos += 4;
                                    }
                                }
                                else
                                {
                                    default_name += ".txt";
                                }

                                auto f = pfd::save_file(is_atlas ? "Save Atlas" : "Save Text", default_name,
                                                        is_atlas ? std::vector<std::string>{"Atlas Files", "*.atlas", "Text Files", "*.txt", "All Files", "*.*"}
                                                                 : std::vector<std::string>{"Text Files", "*.txt", "All Files", "*.*"});

                                if (!f.result().empty())
                                {
                                    std::ofstream out(f.result(), std::ios::binary);
                                    if (out.is_open())
                                    {
                                        out << data_to_save;
                                        out.close();
                                        status_text = "Saved to: " + f.result();
                                    }
                                }
                            }
                            catch (...)
                            {
                            }
                        }
                        nk_layout_row_end(ctx);

                        nk_layout_row_dynamic(ctx, content_height - 130, 1);
                        if (nk_group_begin(ctx, "TextPreview", NK_WINDOW_BORDER))
                        {
                            std::stringstream ss(preview_atlas_data);
                            std::string line;
                            int line_count = 0;
                            while (std::getline(ss, line))
                            {
                                if (line_count > 500)
                                {
                                    nk_layout_row_dynamic(ctx, 20, 1);
                                    nk_label_colored(ctx, "... (preview limit reached)", NK_TEXT_LEFT, nk_rgb(255, 100, 100));
                                    break;
                                }

                                if (!line.empty() && line.back() == '\r')
                                    line.pop_back();

                                nk_layout_row_dynamic(ctx, 20, 1);
                                nk_label_colored(ctx, line.c_str(), NK_TEXT_LEFT, nk_rgb(220, 220, 220));
                                line_count++;
                            }
                            nk_group_end(ctx);
                        }
                    }
                    else if (!preview_error.empty())
                    {
                        nk_layout_row_dynamic(ctx, 30, 1);
                        nk_label_colored(ctx, "Error:", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
                        nk_layout_row_dynamic(ctx, 20, 1);
                        nk_label_colored(ctx, preview_error.c_str(), NK_TEXT_CENTERED, nk_rgb(255, 255, 255));
                    }

                    nk_group_end(ctx);
                }
            }

            nk_layout_row_end(ctx);
          } else {
            // ====== SPINE VIEWER INLINE ======
            // Update animation
            if (active_spine_viewer && active_spine_viewer->isLoaded()) {
                float dt = 0;
                if (spine_playing) {
                    Uint64 now = SDL_GetPerformanceCounter();
                    if (spine_last_tick > 0) {
                        dt = (float)(now - spine_last_tick) / (float)SDL_GetPerformanceFrequency();
                        dt *= spine_speed;
                    }
                    spine_last_tick = now;
                }
                active_spine_viewer->update(dt);
            }

            if (spine_building) {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, "Building Spine dictionary...", NK_TEXT_CENTERED);
            } else if (spine_dictionary.IsBuilt()) {
                const auto& spine_entries_inline = spine_dictionary.GetEntries();
                const auto& root_cat = spine_dictionary.GetRootCategory();

                // Search bar
                nk_layout_row_begin(ctx, NK_STATIC, 28, 3);
                nk_layout_row_push(ctx, 60);
                nk_label(ctx, "Search:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 250);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, spine_search_buffer, sizeof(spine_search_buffer), nk_filter_default);
                spine_search_query = spine_search_buffer;
                nk_layout_row_push(ctx, 200);
                std::string sstats = std::to_string(spine_entries_inline.size()) + " skeletons";
                nk_label_colored(ctx, sstats.c_str(), NK_TEXT_LEFT, nk_rgb(150, 200, 255));
                nk_layout_row_end(ctx);

                // Layout: list | viewport | editor
                float sw = (float)window_width;
                float sh = content_height - 30.0f;
                float iListW = sw * 0.22f;
                float iEditorW = spine_edit_mode ? sw * 0.30f : 0;
                float iViewerW = sw - iListW - iEditorW - 50.0f;

                nk_layout_row_begin(ctx, NK_STATIC, sh, spine_edit_mode ? 3 : 2);

                // Skeleton list
                spine_visible_indices.clear();
                nk_layout_row_push(ctx, iListW);
                if (nk_group_begin(ctx, "SpineListInline", NK_WINDOW_BORDER)) {
                    draw_spine_category(ctx, root_cat, spine_entries_inline, 0);
                    nk_group_end(ctx);
                }

                // Viewer panel
                nk_layout_row_push(ctx, iViewerW);
                if (nk_group_begin(ctx, "SpineViewInline", NK_WINDOW_BORDER)) {
                    if (active_spine_viewer && active_spine_viewer->isLoaded()) {
                        // === Controls row 1: Anim, Skin, Play/Pause ===
                        nk_layout_row_begin(ctx, NK_STATIC, 28, 8);

                        nk_layout_row_push(ctx, 50);
                        nk_label(ctx, "Anim:", NK_TEXT_LEFT);
                        nk_layout_row_push(ctx, 160);
                        auto anim_names = active_spine_viewer->getAnimationNames();
                        if (!anim_names.empty()) {
                            spine_anim_selected = active_spine_viewer->getCurrentAnimIndex();
                            if (spine_anim_selected >= (int)anim_names.size()) spine_anim_selected = 0;
                            if (nk_combo_begin_label(ctx, anim_names[spine_anim_selected].c_str(), nk_vec2(200, 300))) {
                                nk_layout_row_dynamic(ctx, 22, 1);
                                for (int a = 0; a < (int)anim_names.size(); a++) {
                                    if (nk_combo_item_label(ctx, anim_names[a].c_str(), NK_TEXT_LEFT)) {
                                        if (a != spine_anim_selected) {
                                            spine_anim_selected = a;
                                            active_spine_viewer->setAnimation(anim_names[a], true);
                                        }
                                    }
                                }
                                nk_combo_end(ctx);
                            }
                        }

                        nk_layout_row_push(ctx, 45);
                        nk_label(ctx, "Skin:", NK_TEXT_LEFT);
                        nk_layout_row_push(ctx, 120);
                        auto skin_names = active_spine_viewer->getSkinNames();
                        if (!skin_names.empty()) {
                            if (spine_skin_selected >= (int)skin_names.size()) spine_skin_selected = 0;
                            if (nk_combo_begin_label(ctx, skin_names[spine_skin_selected].c_str(), nk_vec2(160, 300))) {
                                nk_layout_row_dynamic(ctx, 22, 1);
                                for (int s = 0; s < (int)skin_names.size(); s++) {
                                    if (nk_combo_item_label(ctx, skin_names[s].c_str(), NK_TEXT_LEFT)) {
                                        if (s != spine_skin_selected) {
                                            spine_skin_selected = s;
                                            active_spine_viewer->setSkin(skin_names[s]);
                                        }
                                    }
                                }
                                nk_combo_end(ctx);
                            }
                        }

                        nk_layout_row_push(ctx, 60);
                        if (nk_button_label(ctx, spine_playing ? "Pause" : "Play")) {
                            spine_playing = !spine_playing;
                            active_spine_viewer->setPlaying(spine_playing);
                            if (spine_playing) spine_last_tick = SDL_GetPerformanceCounter();
                        }

                        nk_layout_row_end(ctx);

                        // === Controls row 2: Speed, Zoom, Flip, Edit, Reset, Export ===
                        nk_layout_row_begin(ctx, NK_STATIC, 28, 13);

                        nk_layout_row_push(ctx, 50);
                        nk_label(ctx, "Speed:", NK_TEXT_LEFT);
                        nk_layout_row_push(ctx, 120);
                        nk_slider_float(ctx, 0.1f, &spine_speed, 3.0f, 0.1f);
                        nk_layout_row_push(ctx, 40);
                        char speed_label[16];
                        snprintf(speed_label, sizeof(speed_label), "%.1fx", spine_speed);
                        nk_label(ctx, speed_label, NK_TEXT_LEFT);

                        nk_layout_row_push(ctx, 45);
                        nk_label(ctx, "Zoom:", NK_TEXT_LEFT);
                        nk_layout_row_push(ctx, 100);
                        spine_zoom = active_spine_viewer->getZoom();
                        nk_slider_float(ctx, 0.1f, &spine_zoom, 5.0f, 0.1f);
                        nk_layout_row_push(ctx, 40);
                        char zoom_label[16];
                        snprintf(zoom_label, sizeof(zoom_label), "%.1fx", spine_zoom);
                        nk_label(ctx, zoom_label, NK_TEXT_LEFT);
                        active_spine_viewer->setZoom(spine_zoom);

                        nk_layout_row_push(ctx, 60);
                        {
                            struct nk_style_button flip_style = ctx->style.button;
                            flip_style.rounding = 3.0f;
                            if (spine_flip_x) {
                                flip_style.normal = nk_style_item_color(nk_rgb(56, 120, 74));
                                flip_style.hover = nk_style_item_color(nk_rgb(66, 138, 86));
                            } else {
                                flip_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                                flip_style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                            }
                            flip_style.text_normal = nk_rgb(220, 220, 220);
                            flip_style.text_hover = nk_rgb(255, 255, 255);
                            if (nk_button_label_styled(ctx, &flip_style, "Flip X")) {
                                spine_flip_x = !spine_flip_x;
                                active_spine_viewer->setFlipX(spine_flip_x);
                            }
                        }

                        nk_layout_row_push(ctx, 60);
                        {
                            struct nk_style_button flip_style = ctx->style.button;
                            flip_style.rounding = 3.0f;
                            if (spine_flip_y) {
                                flip_style.normal = nk_style_item_color(nk_rgb(56, 120, 74));
                                flip_style.hover = nk_style_item_color(nk_rgb(66, 138, 86));
                            } else {
                                flip_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                                flip_style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                            }
                            flip_style.text_normal = nk_rgb(220, 220, 220);
                            flip_style.text_hover = nk_rgb(255, 255, 255);
                            if (nk_button_label_styled(ctx, &flip_style, "Flip Y")) {
                                spine_flip_y = !spine_flip_y;
                                active_spine_viewer->setFlipY(spine_flip_y);
                            }
                        }

                        nk_layout_row_push(ctx, 45);
                        {
                            struct nk_style_button edit_style = ctx->style.button;
                            edit_style.rounding = 3.0f;
                            if (spine_edit_mode) {
                                edit_style.normal = nk_style_item_color(nk_rgb(120, 80, 40));
                                edit_style.hover = nk_style_item_color(nk_rgb(140, 95, 50));
                            } else {
                                edit_style.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                                edit_style.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                            }
                            edit_style.text_normal = nk_rgb(220, 220, 220);
                            edit_style.text_hover = nk_rgb(255, 255, 255);
                            if (nk_button_label_styled(ctx, &edit_style, "Edit")) {
                                spine_edit_mode = !spine_edit_mode;
                            }
                        }

                        nk_layout_row_push(ctx, 55);
                        if (nk_button_label(ctx, "Reset")) {
                            spine_zoom = 1.0f;
                            active_spine_viewer->resetView();
                        }

                        nk_layout_row_push(ctx, 100);
                        if (spine_selected_index >= 0 && spine_selected_index < (int)spine_entries_inline.size()) {
                            if (nk_button_label(ctx, "Export All")) {
                                const auto& entry = spine_entries_inline[spine_selected_index];
                                try {
                                    auto d = pfd::select_folder("Select destination folder", ".");
                                    if (!d.result().empty()) {
                                        std::string dest = d.result();
                                        int exported = 0;
                                        {
                                            std::vector<uint8_t> data = data_pack->GetFileData(*entry.scsp_node);
                                            std::string json_str = SCSPParser::ConvertSCSPToJson(data);
                                            if (!json_str.empty()) {
                                                try { json parsed = json::parse(json_str); json_str = parsed.dump(2); } catch (...) {}
                                                std::ofstream out(dest + "/" + entry.display_name + ".json");
                                                out << json_str; exported++;
                                            }
                                        }
                                        if (entry.atlas_node) {
                                            std::vector<uint8_t> data = data_pack->GetFileData(*entry.atlas_node);
                                            std::string atlas_str(data.begin(), data.end());
                                            size_t p = 0;
                                            while ((p = atlas_str.find(".sct", p)) != std::string::npos) { atlas_str.replace(p, 4, ".png"); p += 4; }
                                            std::ofstream out(dest + "/" + entry.atlas_node->name, std::ios::binary);
                                            out << atlas_str; exported++;
                                        }
                                        spine_dictionary.EnsureDetailsLoaded(*data_pack, entry);
                                        for (const auto* img : entry.image_nodes) {
                                            const auto& fi = std::get<Core::FileInfo>(img->data);
                                            std::vector<uint8_t> data = data_pack->GetFileData(*img);
                                            std::string out_name = img->name;
                                            std::string el = fi.format;
                                            std::transform(el.begin(), el.end(), el.begin(), ::tolower);
                                            if (el == ".sct" || el == ".sct2") {
                                                std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(data, false);
                                                if (!png_data.empty()) {
                                                    size_t dp = out_name.find_last_of('.');
                                                    if (dp != std::string::npos) out_name = out_name.substr(0, dp);
                                                    out_name += ".png";
                                                    std::ofstream out(dest + "/" + out_name, std::ios::binary);
                                                    out.write((const char*)png_data.data(), png_data.size()); exported++;
                                                }
                                            } else {
                                                std::ofstream out(dest + "/" + out_name, std::ios::binary);
                                                out.write((const char*)data.data(), data.size()); exported++;
                                            }
                                        }
                                        status_text = "Exported " + std::to_string(exported) + " files for '" + entry.display_name + "'";
                                    }
                                } catch (const std::exception& e) { status_text = "Export error: " + std::string(e.what()); }
                            }
                        }

                        nk_layout_row_end(ctx);

                        // === Controls row 3: Autoplay, Next, PMA ===
                        static bool spine_autoplay = false;
                        static bool spine_pma_blend = true;
                        static bool spine_pma_tex = true;
                        static int spine_bg_preset = 0; // 0=none, 1=dark, 2=mid, 3=white
                        nk_layout_row_begin(ctx, NK_STATIC, 24, 7);

                        nk_layout_row_push(ctx, 80);
                        {
                            struct nk_style_button ab = ctx->style.button;
                            ab.rounding = 3.0f;
                            ab.normal = nk_style_item_color(spine_autoplay ? nk_rgb(56, 120, 74) : nk_rgb(60, 60, 65));
                            ab.hover = nk_style_item_color(spine_autoplay ? nk_rgb(66, 138, 86) : nk_rgb(75, 75, 80));
                            ab.text_normal = nk_rgb(220, 220, 220);
                            if (nk_button_label_styled(ctx, &ab, spine_autoplay ? "Auto: ON" : "Auto: OFF")) {
                                spine_autoplay = !spine_autoplay;
                                active_spine_viewer->setAutoplayNext(spine_autoplay);
                            }
                        }

                        nk_layout_row_push(ctx, 50);
                        if (nk_button_label(ctx, "Next")) {
                            active_spine_viewer->nextAnimation();
                            spine_anim_selected = active_spine_viewer->getCurrentAnimIndex();
                        }

                        nk_layout_row_push(ctx, 20);
                        nk_spacing(ctx, 1);

                        nk_layout_row_push(ctx, 80);
                        {
                            struct nk_style_button pb = ctx->style.button;
                            pb.rounding = 3.0f;
                            pb.normal = nk_style_item_color(spine_pma_blend ? nk_rgb(70, 90, 120) : nk_rgb(60, 60, 65));
                            pb.hover = nk_style_item_color(nk_rgb(80, 100, 130));
                            pb.text_normal = nk_rgb(200, 200, 200);
                            if (nk_button_label_styled(ctx, &pb, spine_pma_blend ? "PMA: ON" : "PMA: OFF")) {
                                spine_pma_blend = !spine_pma_blend;
                                // Source textures from ASTC are already premultiplied —
                                // never re-premultiply, just toggle the blend mode
                                active_spine_viewer->setUsePMA(spine_pma_blend);
                                active_spine_viewer->setPremultiplyTextures(false);
                                if (spine_selected_index >= 0 && spine_selected_index < (int)spine_entries_inline.size()) {
                                    active_spine_viewer->loadSkeleton(spine_dictionary, *data_pack, spine_entries_inline[spine_selected_index]);
                                }
                            }
                        }

                        nk_layout_row_push(ctx, 10);
                        nk_spacing(ctx, 1);

                        // Viewport background preset
                        nk_layout_row_push(ctx, 80);
                        {
                            const char* bg_labels[] = {"BG: None", "BG: Dark", "BG: Gray", "BG: White"};
                            const float bg_colors[][3] = {
                                {0, 0, 0}, {0.12f, 0.12f, 0.14f},
                                {0.35f, 0.35f, 0.38f}, {1.0f, 1.0f, 1.0f}
                            };
                            struct nk_style_button bb = ctx->style.button;
                            bb.rounding = 3.0f;
                            bb.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                            bb.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                            bb.text_normal = nk_rgb(200, 200, 200);
                            if (nk_button_label_styled(ctx, &bb, bg_labels[spine_bg_preset])) {
                                spine_bg_preset = (spine_bg_preset + 1) % 4;
                                active_spine_viewer->setBgColor(
                                    bg_colors[spine_bg_preset][0],
                                    bg_colors[spine_bg_preset][1],
                                    bg_colors[spine_bg_preset][2]);
                            }
                        }

                        nk_layout_row_end(ctx);

                        // === Viewport ===
                        float vpH = sh - 120.0f;
                        if (vpH < 100) vpH = 100;
                        nk_layout_row_dynamic(ctx, vpH, 1);
                        struct nk_rect vb = nk_widget_bounds(ctx);
                        int vw = (int)vb.w, vh = (int)vb.h;

                        // Mouse interaction (skip if a combo/popup is active)
                        {
                            nk_input* inp = &ctx->input;
                            bool popup_active = (ctx->current && ctx->current->popup.win);
                            if (!popup_active && nk_input_is_mouse_hovering_rect(inp, vb)) {
                                float scr = inp->mouse.scroll_delta.y;
                                float mdx = inp->mouse.delta.x, mdy = inp->mouse.delta.y;
                                Uint32 km = SDL_GetModState();

                                if (scr != 0 && !(km & KMOD_CTRL)) {
                                    active_spine_viewer->zoomBy(scr > 0 ? 1.15f : 1.0f/1.15f);
                                    spine_zoom = active_spine_viewer->getZoom();
                                }
                                if (nk_input_is_mouse_down(inp, NK_BUTTON_MIDDLE) ||
                                    nk_input_is_mouse_down(inp, NK_BUTTON_RIGHT) ||
                                    (nk_input_is_mouse_down(inp, NK_BUTTON_LEFT) && (!spine_edit_mode || (km & KMOD_SHIFT)))) {
                                    if (mdx != 0 || mdy != 0) {
                                        float s = active_spine_viewer->getZoom() > 0 ? (float)vw / active_spine_viewer->getZoom() / vw : 1;
                                        active_spine_viewer->pan(mdx * s, -mdy * s);
                                    }
                                }
                                if (spine_edit_mode && !spine_selected_bone.empty() && scr != 0 && (km & KMOD_CTRL)) {
                                    auto bl = active_spine_viewer->getBoneList();
                                    for (auto& b : bl) { if (b.name == spine_selected_bone) {
                                        BoneOverride o; o.x=b.x; o.y=b.y; o.rotation=b.rotation;
                                        o.scaleX=b.scaleX+scr*0.05f; o.scaleY=b.scaleY+scr*0.05f;
                                        o.shearX=b.shearX; o.shearY=b.shearY;
                                        active_spine_viewer->setBoneOverride(b.name, o); break;
                                    }}
                                }
                                if (spine_edit_mode) {
                                    static SpineViewer::GizmoHandle ag = SpineViewer::GizmoHandle::None;
                                    float lx = inp->mouse.pos.x - vb.x, ly = inp->mouse.pos.y - vb.y;
                                    if (nk_input_is_mouse_pressed(inp, NK_BUTTON_LEFT) && !(km & KMOD_SHIFT)) {
                                        ag = active_spine_viewer->hitTestGizmo(lx, ly, vw, vh);
                                        if (ag == SpineViewer::GizmoHandle::None) {
                                            spine_selected_bone = active_spine_viewer->hitTestBone(lx, ly, vw, vh);
                                            if (!spine_selected_bone.empty()) {
                                                ag = SpineViewer::GizmoHandle::Move;
                                                spine_scroll_to_bone = true;
                                            }
                                        }
                                    }
                                    if (!nk_input_is_mouse_down(inp, NK_BUTTON_LEFT)) ag = SpineViewer::GizmoHandle::None;
                                    if (ag != SpineViewer::GizmoHandle::None && !spine_selected_bone.empty()
                                        && nk_input_is_mouse_down(inp, NK_BUTTON_LEFT) && (mdx!=0||mdy!=0)) {
                                        float s = active_spine_viewer->getZoom()>0?(float)vw/active_spine_viewer->getZoom()/vw:1;
                                        auto bl = active_spine_viewer->getBoneList();
                                        for (auto& b : bl) { if (b.name != spine_selected_bone) continue;
                                            BoneOverride o; o.x=b.x; o.y=b.y; o.rotation=b.rotation;
                                            o.scaleX=b.scaleX; o.scaleY=b.scaleY; o.shearX=b.shearX; o.shearY=b.shearY;
                                            if (ag==SpineViewer::GizmoHandle::Move) { o.x+=mdx*s; o.y-=mdy*s; }
                                            else if (ag==SpineViewer::GizmoHandle::Rotate) { o.rotation+=mdx*0.5f; }
                                            else { o.scaleX+=mdx*0.005f; o.scaleY-=mdy*0.005f; }
                                            active_spine_viewer->setBoneOverride(b.name, o); break;
                                        }
                                    }
                                }
                            }
                        }

                        if (vw > 0 && vh > 0) {
                            active_spine_viewer->render(vw, vh);
                            GLuint ft = active_spine_viewer->getFBOTexture();
                            if (ft) {
                                struct nk_image fimg = nk_image_id((int)ft);
                                nk_draw_image(nk_window_get_canvas(ctx), vb, &fimg, nk_rgb(255,255,255));
                            }
                        }
                    } else if (active_spine_viewer && !active_spine_viewer->getError().empty()) {
                        nk_layout_row_dynamic(ctx, 30, 1);
                        nk_label_colored(ctx, "Error:", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
                        nk_layout_row_dynamic(ctx, 20, 1);
                        nk_label(ctx, active_spine_viewer->getError().c_str(), NK_TEXT_CENTERED);
                    } else {
                        nk_layout_row_dynamic(ctx, 30, 1);
                        nk_label(ctx, "Select a skeleton from the list", NK_TEXT_CENTERED);
                    }
                    nk_group_end(ctx);
                }

                // Bone editor panel (third column, only when editing)
                if (spine_edit_mode && active_spine_viewer && active_spine_viewer->isLoaded()) {
                    nk_layout_row_push(ctx, iEditorW);
                    if (nk_group_begin(ctx, "BoneEditor", NK_WINDOW_BORDER)) {
                        nk_layout_row_dynamic(ctx, 24, 3);
                        if (nk_button_label(ctx, "Reset All")) {
                            active_spine_viewer->resetBoneEdits();
                            spine_selected_bone = "";
                        }

                        static bool spine_export_pending = false;
                        if (nk_button_label(ctx, "Export Modified")) {
                            spine_export_pending = true;
                        }

                        auto texList = active_spine_viewer->getTextureList();
                        if (texList.empty()) {
                            nk_spacing(ctx, 1);
                        }

                        if (spine_export_pending) {
                            spine_export_pending = false;
                            try {
                                auto d = pfd::select_folder("Select destination folder", ".");
                                if (!d.result().empty() && spine_selected_index >= 0) {
                                    std::string dest = d.result();
                                    const auto& entry = spine_entries_inline[spine_selected_index];
                                    int exported = 0;
                                    std::string modJson = active_spine_viewer->getModifiedSkeletonJson();
                                    if (!modJson.empty()) {
                                        std::ofstream out(dest + "/" + entry.display_name + "_modified.json");
                                        out << modJson; exported++;
                                    }
                                    if (entry.atlas_node) {
                                        std::vector<uint8_t> ad = data_pack->GetFileData(*entry.atlas_node);
                                        std::string as(ad.begin(), ad.end());
                                        size_t p = 0;
                                        while ((p = as.find(".sct", p)) != std::string::npos) { as.replace(p, 4, ".png"); p += 4; }
                                        std::ofstream out(dest + "/" + entry.atlas_node->name, std::ios::binary);
                                        out << as; exported++;
                                    }
                                    for (const auto* img : entry.image_nodes) {
                                        const auto& fi = std::get<Core::FileInfo>(img->data);
                                        std::vector<uint8_t> fd = data_pack->GetFileData(*img);
                                        std::string on = img->name;
                                        std::string el = fi.format;
                                        std::transform(el.begin(), el.end(), el.begin(), ::tolower);
                                        if (el == ".sct" || el == ".sct2") {
                                            auto png = SCTParser::ConvertToPNG(fd, false);
                                            if (!png.empty()) {
                                                size_t dp = on.find_last_of('.'); if (dp != std::string::npos) on = on.substr(0, dp);
                                                on += ".png";
                                                std::ofstream out(dest + "/" + on, std::ios::binary);
                                                out.write((const char*)png.data(), png.size()); exported++;
                                            }
                                        } else {
                                            std::ofstream out(dest + "/" + on, std::ios::binary);
                                            out.write((const char*)fd.data(), fd.size()); exported++;
                                        }
                                    }
                                    status_text = "Exported " + std::to_string(exported) + " modified files";
                                }
                            } catch (...) {}
                        }

                        // Bone search — ABOVE the scrollable list so it stays fixed
                        static char bone_search_buf[128] = {0};
                        nk_layout_row_begin(ctx, NK_STATIC, 20, 2);
                        nk_layout_row_push(ctx, 50);
                        nk_label(ctx, "Filter:", NK_TEXT_LEFT);
                        nk_layout_row_push(ctx, iEditorW - 70);
                        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, bone_search_buf, sizeof(bone_search_buf), nk_filter_default);
                        nk_layout_row_end(ctx);
                        std::string bone_query = bone_search_buf;
                        std::transform(bone_query.begin(), bone_query.end(), bone_query.begin(), ::tolower);

                        nk_layout_row_dynamic(ctx, sh - 105, 1);
                        nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(2, 0));
                        nk_style_push_vec2(ctx, &ctx->style.window.group_padding, nk_vec2(2, 2));
                        if (nk_group_begin(ctx, "BoneList", NK_WINDOW_BORDER)) {
                            auto bones = active_spine_viewer->getBoneList();
                            float scroll_target_y = -1;
                            static bool spine_bone_just_reset = false;

                            // Build parent->has_children lookup
                            std::unordered_set<std::string> has_children;
                            for (auto& b : bones) {
                                if (!b.parentName.empty()) has_children.insert(b.parentName);
                            }

                            for (size_t bi_idx = 0; bi_idx < bones.size(); bi_idx++) {
                                auto& bi = bones[bi_idx];

                                // Filter by search
                                if (!bone_query.empty()) {
                                    std::string lower_name = bi.name;
                                    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                                    if (lower_name.find(bone_query) == std::string::npos) continue;
                                }

                                // Skip if an ancestor is collapsed in the list
                                if (bone_query.empty()) {
                                    bool ancestor_collapsed = false;
                                    // Walk up parents using the bone list data
                                    std::string check = bi.parentName;
                                    int safety = 0;
                                    while (!check.empty() && safety++ < 20) {
                                        if (spine_collapsed_bones.count(check)) { ancestor_collapsed = true; break; }
                                        // Find parent's parent
                                        bool found = false;
                                        for (auto& pb : bones) {
                                            if (pb.name == check) { check = pb.parentName; found = true; break; }
                                        }
                                        if (!found) break;
                                    }
                                    if (ancestor_collapsed) continue;
                                }

                                bool is_sel = (bi.name == spine_selected_bone);
                                bool is_parent = has_children.count(bi.name) > 0;
                                bool is_collapsed = spine_collapsed_bones.count(bi.name) > 0;

                                // Bone row: indent spacer + name + H + R
                                float indent_px = bi.depth * 10.0f;
                                if (indent_px > 0) {
                                    int cols = 4;
                                    nk_layout_row_begin(ctx, NK_STATIC, 16, cols);
                                    nk_layout_row_push(ctx, indent_px);
                                    // Draw tree line
                                    struct nk_rect sp_bounds = nk_widget_bounds(ctx);
                                    struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
                                    float line_x = sp_bounds.x + indent_px - 6;
                                    nk_stroke_line(canvas, line_x, sp_bounds.y, line_x, sp_bounds.y + sp_bounds.h, 1.0f, nk_rgb(60, 65, 75));
                                    nk_stroke_line(canvas, line_x, sp_bounds.y + sp_bounds.h * 0.5f, sp_bounds.x + indent_px, sp_bounds.y + sp_bounds.h * 0.5f, 1.0f, nk_rgb(60, 65, 75));
                                    nk_spacing(ctx, 1);
                                    float remaining = iEditorW - indent_px - 60;
                                    nk_layout_row_push(ctx, remaining > 40 ? remaining : 40);
                                } else {
                                    nk_layout_row_begin(ctx, NK_STATIC, 16, 3);
                                    float remaining = iEditorW - 60;
                                    nk_layout_row_push(ctx, remaining > 40 ? remaining : 40);
                                }

                                // Grab bounds for auto-scroll
                                if (is_sel && spine_scroll_to_bone) {
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
                                bone_btn.text_normal = bi.hidden ? nk_rgb(100, 100, 100)
                                    : bi.hasOverride ? nk_rgb(255, 200, 80)
                                    : is_sel ? nk_rgb(100, 200, 255) : nk_rgb(180, 180, 180);
                                bone_btn.text_hover = nk_rgb(255, 255, 255);
                                std::string bone_label = bi.name;
                                if (is_parent) bone_label = (is_collapsed ? "+ " : "- ") + bone_label;
                                if (nk_button_label_styled(ctx, &bone_btn, bone_label.c_str())) {
                                    if (is_parent && bi.name == spine_selected_bone) {
                                        // Second click on same parent toggles collapse
                                        if (is_collapsed) spine_collapsed_bones.erase(bi.name);
                                        else spine_collapsed_bones.insert(bi.name);
                                    }
                                    spine_selected_bone = bi.name;
                                    active_spine_viewer->selectedBoneIndex = (int)bi_idx;
                                    spine_bone_just_reset = false;
                                }

                                nk_layout_row_push(ctx, 24);
                                {
                                    struct nk_style_button hb = ctx->style.button;
                                    hb.rounding = 1.0f; hb.border = 0; hb.padding = nk_vec2(0, 0);
                                    hb.normal = nk_style_item_color(bi.hidden ? nk_rgb(120, 50, 50) : nk_rgb(45, 45, 50));
                                    hb.hover = nk_style_item_color(nk_rgb(80, 60, 60));
                                    hb.text_normal = nk_rgb(200, 200, 200);
                                    if (nk_button_label_styled(ctx, &hb, bi.hidden ? "H" : "V")) {
                                        active_spine_viewer->toggleBoneHidden(bi.name);
                                    }
                                }

                                nk_layout_row_push(ctx, 24);
                                {
                                    struct nk_style_button rb = ctx->style.button;
                                    rb.padding = nk_vec2(0, 0); rb.border = 0; rb.rounding = 1.0f;
                                    if (nk_button_label_styled(ctx, &rb, "R")) {
                                        active_spine_viewer->resetBone(bi.name);
                                        spine_bone_just_reset = true;
                                    }
                                }
                                nk_layout_row_end(ctx);

                                if (is_sel) {
                                    // Auto-create override if not editing (skip if just reset)
                                    if (!bi.hasOverride && !spine_bone_just_reset) {
                                        BoneOverride ovr;
                                        ovr.x = bi.setupX; ovr.y = bi.setupY; ovr.rotation = bi.setupRot;
                                        ovr.scaleX = bi.setupSX; ovr.scaleY = bi.setupSY;
                                        ovr.shearX = bi.setupShX; ovr.shearY = bi.setupShY;
                                        active_spine_viewer->setBoneOverride(bi.name, ovr);
                                    }
                                    if (bi.hasOverride) spine_bone_just_reset = false;

                                    const char* labels[] = {"X", "Y", "Rot", "SclX", "SclY", "ShrX", "ShrY"};
                                    float anim[7] = { bi.animX, bi.animY, bi.animRot, bi.animSX, bi.animSY, bi.animShX, bi.animShY };
                                    float setup[7] = { bi.setupX, bi.setupY, bi.setupRot, bi.setupSX, bi.setupSY, bi.setupShX, bi.setupShY };

                                    static bool spine_link_scale = true;
                                    nk_layout_row_dynamic(ctx, 16, 2);
                                    nk_label_colored(ctx, "Editing:", NK_TEXT_LEFT, nk_rgb(255, 200, 80));
                                    {
                                        struct nk_style_button lb = ctx->style.button;
                                        lb.rounding = 1.0f; lb.padding = nk_vec2(2, 0); lb.border = 0;
                                        lb.normal = nk_style_item_color(spine_link_scale ? nk_rgb(56, 120, 74) : nk_rgb(60, 60, 65));
                                        lb.hover = nk_style_item_color(spine_link_scale ? nk_rgb(66, 138, 86) : nk_rgb(75, 75, 80));
                                        lb.text_normal = nk_rgb(200, 200, 200);
                                        if (nk_button_label_styled(ctx, &lb, spine_link_scale ? "Scale: Linked" : "Scale: Free")) {
                                            spine_link_scale = !spine_link_scale;
                                        }
                                    }

                                    float vals[7] = { bi.x, bi.y, bi.rotation, bi.scaleX, bi.scaleY, bi.shearX, bi.shearY };
                                    float oldSclX = vals[3], oldSclY = vals[4];
                                    float steps[7] = { 1.0f, 1.0f, 1.0f, 0.05f, 0.05f, 0.5f, 0.5f };
                                    float pxStep[7] = { 0.5f, 0.5f, 0.5f, 0.01f, 0.01f, 0.1f, 0.1f };
                                    for (int f = 0; f < 7; f++) {
                                        float range = fmaxf(fmaxf(fabsf(vals[f]), fabsf(setup[f])) * 3.0f, 10.0f);
                                        nk_layout_row_dynamic(ctx, 18, 1);
                                        vals[f] = nk_propertyf(ctx, labels[f], -range, vals[f], range, steps[f], pxStep[f]);
                                    }

                                    if (spine_link_scale) {
                                        float dsx = vals[3] - oldSclX, dsy = vals[4] - oldSclY;
                                        if (dsx != 0 && dsy == 0) vals[4] += dsx;
                                        if (dsy != 0 && dsx == 0) vals[3] += dsy;
                                    }

                                    BoneOverride ovr;
                                    ovr.x = vals[0]; ovr.y = vals[1]; ovr.rotation = vals[2];
                                    ovr.scaleX = vals[3]; ovr.scaleY = vals[4];
                                    ovr.shearX = vals[5]; ovr.shearY = vals[6];
                                    active_spine_viewer->setBoneOverride(bi.name, ovr);

                                    // Animated values
                                    nk_layout_row_dynamic(ctx, 13, 1);
                                    nk_label_colored(ctx, "Animated:", NK_TEXT_LEFT, nk_rgb(100, 180, 255));
                                    nk_layout_row_dynamic(ctx, 13, 4);
                                    for (int f = 0; f < 7; f++) {
                                        char abuf[24];
                                        snprintf(abuf, sizeof(abuf), "%s:%.1f", labels[f], anim[f]);
                                        nk_label_colored(ctx, abuf, NK_TEXT_LEFT, nk_rgb(80, 150, 220));
                                        if (f == 3) { nk_layout_row_dynamic(ctx, 13, 4); }
                                    }

                                    // Setup pose
                                    nk_layout_row_dynamic(ctx, 13, 1);
                                    nk_label_colored(ctx, "Setup:", NK_TEXT_LEFT, nk_rgb(100, 200, 100));
                                    nk_layout_row_dynamic(ctx, 13, 4);
                                    for (int f = 0; f < 7; f++) {
                                        char sbuf[24];
                                        snprintf(sbuf, sizeof(sbuf), "%s:%.1f", labels[f], setup[f]);
                                        nk_label_colored(ctx, sbuf, NK_TEXT_LEFT, nk_rgb(80, 170, 80));
                                        if (f == 3) { nk_layout_row_dynamic(ctx, 13, 4); }
                                    }

                                    nk_layout_row_dynamic(ctx, 2, 1);
                                    nk_spacing(ctx, 1);
                                }
                            }

                            // Auto-scroll
                            if (spine_scroll_to_bone && scroll_target_y >= 0) {
                                nk_uint scx, scy;
                                nk_group_get_scroll(ctx, "BoneList", &scx, &scy);
                                struct nk_rect content = nk_window_get_content_region(ctx);
                                float rel_y = scroll_target_y - content.y + (float)scy;
                                float new_scroll = rel_y - content.h * 0.3f;
                                if (new_scroll < 0) new_scroll = 0;
                                nk_group_set_scroll(ctx, "BoneList", scx, (nk_uint)new_scroll);
                            }
                            spine_scroll_to_bone = false;

                            nk_group_end(ctx);
                        }
                        nk_style_pop_vec2(ctx);
                        nk_style_pop_vec2(ctx);

                        nk_group_end(ctx);
                    }
                }

                nk_layout_row_end(ctx);
            }
          } // end else (show_spine_viewer)

            nk_layout_row_dynamic(ctx, 28, 1);
            if (selection_exists)
            {
                try
                {
                    if (std::holds_alternative<Core::FileInfo>(selected_node->data))
                    {
                        const auto &info = std::get<Core::FileInfo>(selected_node->data);
                    }
                }
                catch (...)
                {
                }
            }
        }
        nk_end(ctx);

        glViewport(0, 0, window_width, window_height);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
        render_image_window();
        SDL_GL_SwapWindow(win);
    }

    if (preview_texture)
        glDeleteTextures(1, &preview_texture);
    if (sct_preview_texture)
        glDeleteTextures(1, &sct_preview_texture);
    nk_sdl_shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(win);
    if (image_window)
    {
        if (image_window_texture)
        {
            SDL_DestroyTexture(image_window_texture);
        }
        if (image_renderer)
        {
            SDL_DestroyRenderer(image_renderer);
        }
        SDL_DestroyWindow(image_window);
    }
    IMG_Quit();
    SDL_Quit();
    return 0;
}


