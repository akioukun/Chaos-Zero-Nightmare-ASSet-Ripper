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
#include "json.hpp"

#define INITIAL_WINDOW_WIDTH 1400
#define INITIAL_WINDOW_HEIGHT 900
#define DOUBLE_CLICK_TIME_MS 300

#define ICON_FOLDER ""
#define ICON_FILE ""
#define ICON_IMAGE ""
#define ICON_COMPRESSED ""
#define ICON_ATLAS ""
#define ICON_DATABASE ""

using json = nlohmann::ordered_json;

static std::unique_ptr<DataPack> data_pack = nullptr;
static Core::FileNode const *selected_node = nullptr;
static std::unordered_set<const Core::FileNode *> selected_file_nodes;
static std::future<void> task_future;
static std::atomic<float> task_progress = 0.0f;
static std::atomic<bool> is_task_running = false;
static std::string status_text = "Select a data.pack file to begin.";
static std::unordered_set<const Core::FileNode *> expanded_folders;
static char search_buffer[256] = {0};
static std::string search_query = "";

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
static bool export_sct_as_png = true;
static bool export_convert_all_sct = false;
static bool export_db_as_json = true;

static std::string wstring_to_utf8(const std::wstring &w)
{
    if (w.empty())
        return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), needed, nullptr, nullptr);
    return s;
}

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

std::string get_file_icon(const std::string &ext)
{
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

    if (ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" ||
        ext_lower == ".bmp" || ext_lower == ".tga" || ext_lower == ".webp" ||
        ext_lower == ".sct" || ext_lower == ".sct2")
    {
        return ICON_IMAGE;
    }
    else if (ext_lower == ".atlas" || ext_lower == ".txt")
    {
        return ICON_ATLAS;
    }
    else if (ext_lower == ".db" || ext_lower == ".json" || ext_lower == ".scsp")
    {
        return ICON_DATABASE;
    }
    else if (ext_lower == ".srt" || ext_lower == ".zip" || ext_lower == ".pack")
    {
        return ICON_COMPRESSED;
    }
    return ICON_FILE;
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

    if (!is_folder)
    {
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
            load_image_preview(*node);
        }
        else
        {
            selected_file_nodes.clear();
            selected_file_nodes.insert(node);
            if (selected_node != node)
            {
                selected_node = node;
                load_image_preview(*node);
            }
            else
            {
                selected_node = node;
            }
        }
    }
    else
    {
        if (!ctrl_pressed)
        {
            selected_file_nodes.clear();
        }
        if (selected_node != node)
            selected_node = node;
        has_preview = false;
        preview_error = "";
        preview_atlas_data = "";
        preview_json_data = "";
        full_atlas_data = "";
        db_column_names.clear();
        db_rows.clear();
        current_preview_mode = PreviewMode::None;
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

void export_file_tree_json(const Core::FileNode &node, std::ofstream &out, int depth = 0)
{
    std::string indent(depth * 2, ' ');

    out << indent << "{\n";
    out << indent << "  \"name\": \"" << node.name << "\",\n";
    out << indent << "  \"path\": \"" << node.full_path << "\",\n";

    if (std::holds_alternative<Core::FileInfo>(node.data))
    {
        const auto &info = std::get<Core::FileInfo>(node.data);
        out << indent << "  \"type\": \"file\",\n";
        out << indent << "  \"size\": " << info.size << ",\n";
        out << indent << "  \"offset\": " << info.offset << ",\n";
        out << indent << "  \"format\": \"" << info.format << "\"\n";
    }
    else
    {
        const auto &folder = std::get<Core::FolderInfo>(node.data);
        out << indent << "  \"type\": \"folder\",\n";
        out << indent << "  \"children\": [\n";

        for (size_t i = 0; i < folder.children.size(); ++i)
        {
            export_file_tree_json(folder.children[i], out, depth + 2);
            if (i < folder.children.size() - 1)
                out << ",";
            out << "\n";
        }
        out << indent << "  ]\n";
    }

    out << indent << "}";
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
                export_file_tree_json(data_pack->GetFileTree(), out);
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

        if (std::holds_alternative<Core::FolderInfo>(node.data))
        {
            const auto &folder = std::get<Core::FolderInfo>(node.data);
            bool is_expanded = expanded_folders.find(&node) != expanded_folders.end();
            bool is_selected = (selected_node == &node);

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
    SDL_Window *win = SDL_CreateWindow("Chaos Zero Nightmare ASSet Ripper v1.3",
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
            if (nk_begin(ctx, "Export Options", nk_rect(window_width / 2 - 200, window_height / 2 - 120, 400, 280),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE))
            {

                nk_layout_row_dynamic(ctx, 25, 1);
                nk_label(ctx, "Configure extraction options:", NK_TEXT_LEFT);

                nk_layout_row_dynamic(ctx, 30, 1);
                nk_checkbox_label(ctx, "Convert SCT files to PNG", (nk_bool *)&export_sct_as_png);

                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "When enabled, .sct/.sct2 files will be", NK_TEXT_LEFT);
                nk_label(ctx, "automatically converted to PNG during extraction.", NK_TEXT_LEFT);

                nk_layout_row_dynamic(ctx, 10, 1);
                nk_spacing(ctx, 1);

                nk_layout_row_dynamic(ctx, 30, 1);
                nk_checkbox_label(ctx, "Convert DB files to JSON", (nk_bool *)&export_db_as_json);

                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "When enabled, .db files will be", NK_TEXT_LEFT);
                nk_label(ctx, "automatically converted to JSON during extraction.", NK_TEXT_LEFT);

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
            if (nk_begin(ctx, "Credits", nk_rect(window_width / 2 - 250, window_height / 2 - 150, 700, 300),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                             NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
            {

                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, "Chaos Zero Nightmare ASSet Ripper v1.3", NK_TEXT_CENTERED);
                nk_label(ctx, "by @akioukun (github.com/akioukun)", NK_TEXT_CENTERED);
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "", NK_TEXT_LEFT);
                nk_label(ctx, "made with nuklear, sdl2/opengl, portable-file-dialogs", NK_TEXT_CENTERED);
                nk_label(ctx, "SCT/SCT2 support with astcenc & etcdec", NK_TEXT_CENTERED);
                nk_label(ctx, "big thanks to @formagGino (github.com/formagGinoo) for SCT Parser, DB Parser and SCSP Parser", NK_TEXT_CENTERED);
                nk_label(ctx, "thanks to @LukeFZ (github.com/LukeFZ) for DB decryption logic", NK_TEXT_CENTERED);

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
            if (nk_begin(ctx, "Success", nk_rect(window_width / 2 - 150, window_height / 2 - 50, 300, 100),
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
            bool tree_scanned = pack_loaded && !std::get<Core::FolderInfo>(data_pack->GetFileTree().data).children.empty();
            bool selection_exists = (selected_node != nullptr);
            bool has_file_selection = !selected_file_nodes.empty();

            nk_layout_row_dynamic(ctx, 38, 7);

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

            if (pack_loaded && !tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Scan Tree"))
            {
                try
                {
                    is_task_running = true;
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

            if (tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Extract All"))
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
                        bool convert_sct = export_sct_as_png;
                        bool convert_db = export_db_as_json;
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
            else if (!tree_scanned || is_task_running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract All");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && has_file_selection && !is_task_running &&
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
                        nodes_to_extract.reserve(selected_file_nodes.size());
                        for (const auto *n : selected_file_nodes)
                        {
                            if (n && std::holds_alternative<Core::FileInfo>(n->data))
                                nodes_to_extract.push_back(n);
                        }
                        if (nodes_to_extract.empty() && selected_node && std::holds_alternative<Core::FileInfo>(selected_node->data))
                        {
                            nodes_to_extract.push_back(selected_node);
                        }

                        status_text = "Extracting " + std::to_string(nodes_to_extract.size()) + " files...";
                        task_progress = 0.0f;
                        bool convert_sct = export_sct_as_png;
                        bool convert_db = export_db_as_json;
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
            else if (!tree_scanned || !has_file_selection || is_task_running)
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

            if (tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Options"))
            {
                show_export_options_window = true;
            }
            else if (!tree_scanned || is_task_running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Options");
                nk_widget_disable_end(ctx);
            }

            if (nk_button_label_styled(ctx, &btn_style, "Credits"))
            {
                show_credits_window = true;
            }

            nk_layout_row_begin(ctx, NK_STATIC, 30, 2);
            nk_layout_row_push(ctx, 80);
            nk_label(ctx, "Search:", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 300);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, search_buffer, sizeof(search_buffer), nk_filter_default);
            search_query = search_buffer;
            nk_layout_row_end(ctx);

            float content_height = (float)window_height - 40;
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
                if (data_pack && tree_scanned)
                {
                    draw_file_node(ctx, data_pack->GetFileTree());
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
                if (nk_group_begin(ctx, "Preview", NK_WINDOW_BORDER | NK_WINDOW_TITLE))
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
                                nk_label_colored(ctx, "... (Display limited to 200 rows)", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
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

                            if (is_db_source)
                            {
                                nk_layout_row_push(ctx, 120);
                                if (nk_button_label(ctx, "Export as JSON"))
                                {
                                    export_db_as_json_file(*selected_node);
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

                        nk_layout_row_dynamic(ctx, content_height - 100, 1);
                        if (nk_group_begin(ctx, "JsonPreview", NK_WINDOW_BORDER))
                        {
                            std::stringstream ss(preview_json_data);
                            std::string line;
                            int line_count = 0;
                            while (std::getline(ss, line))
                            {
                                if (is_scsp_source && line_count > 500)
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

                        nk_layout_row_dynamic(ctx, content_height - 100, 1);
                        if (nk_group_begin(ctx, "TextPreview", NK_WINDOW_BORDER))
                        {
                            std::stringstream ss(preview_atlas_data);
                            std::string line;
                            while (std::getline(ss, line))
                            {
                                if (!line.empty() && line.back() == '\r')
                                    line.pop_back();

                                nk_layout_row_dynamic(ctx, 20, 1);
                                nk_label_colored(ctx, line.c_str(), NK_TEXT_LEFT, nk_rgb(220, 220, 220));
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
