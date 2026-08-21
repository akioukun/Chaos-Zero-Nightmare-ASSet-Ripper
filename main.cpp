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
#include "core/Core.h"
#include "archive/DataPack.h"
#include "archive/IArchive.h"
#include "archive/CompositeArchive.h"
#include "archive/SSRArchive.h"
#include "parsers/SCTParser.h"
#include "parsers/DBParser.h"
#include "parsers/SCSPParser.h"
#include "parsers/SpineDictionary.h"
#include "parsers/SpineRenderer.h"
#include "core/Logger.h"
#include "core/RipperOptions.h"
#include "json.hpp"

#define INITIAL_WINDOW_WIDTH 1400
#define INITIAL_WINDOW_HEIGHT 900
#define DOUBLE_CLICK_TIME_MS 300

using json = nlohmann::ordered_json;

std::unique_ptr<IArchive> CreateArchive(const std::wstring& wpath) {
    std::filesystem::path p(wpath);
    if (p.filename() == L"manifest.ssra" || p.extension() == L".ssra") {
        return std::make_unique<SSRArchive>(wpath);
    }
    std::filesystem::path dir = std::filesystem::is_directory(p) ? p : p.parent_path();
    std::filesystem::path gameres_path = dir / L"gameres";
    
    if (std::filesystem::exists(gameres_path) && std::filesystem::is_directory(gameres_path)) {
        auto composite = std::make_unique<CompositeArchive>(wpath);
        composite->AddArchive(std::make_unique<DataPack>(wpath));
        
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(gameres_path)) {
                if (entry.is_regular_file() && entry.path().filename() == L"manifest.ssra") {
                    composite->AddArchive(std::make_unique<SSRArchive>(entry.path().wstring()));
                }
            }
        } catch (const std::exception& e) {
            LogError("Error scanning gameres directory: " + std::string(e.what()));
        }
        return composite;
    }
    return std::make_unique<DataPack>(wpath);
}

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

struct TaskState
{
    std::future<void> future;
    std::atomic<float> progress = 0.f;
    std::atomic<bool> running = false;
    std::atomic<bool> scan_complete = false;
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
    std::string error, atlas_preview, atlas_full, json_preview;
    PreviewMode mode = PreviewMode::None;
    const Core::FileNode* preview_node = nullptr;
};

struct AtlasViewerState
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
    AtlasViewerState atlas;
    DatabaseViewerState database;
    ImageWindowState image;
    ContextMenuState context_menu;
    CommonState common;
    CreditsState credits;
    SCTPreviewState sct;
    SpineViewerState spine;
    DiffViewerState diff;
};

static AppState g_state;

static void save_options_to_ini()
{
    RipperOptions options;
    options.exportSctAsPng = (g_state.common.export_sct_as_png != nk_false);
    options.exportDbAsJson = (g_state.common.export_db_as_json != nk_false);
    options.enableOpenFolder = (g_state.common.enable_open_folder != nk_false);
    SaveRipperOptions(options);
}

static void load_options_from_ini()
{
    const RipperOptions options = LoadRipperOptions();
    g_state.common.export_sct_as_png = options.exportSctAsPng ? nk_true : nk_false;
    g_state.common.export_db_as_json = options.exportDbAsJson ? nk_true : nk_false;
    g_state.common.enable_open_folder = options.enableOpenFolder ? nk_true : nk_false;
}

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
        g_state.preview.json_preview = "";
        std::string json_content = content;

        if (json_content.empty())
        {
            std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            if (!file_data.empty())
            {
                json_content = std::string(file_data.begin(), file_data.end());
            }
            else
            {
                g_state.preview.error = "Failed to read JSON file";
                g_state.preview.mode = PreviewMode::None;
                return;
            }
        }

        try
        {
            json parsed = json::parse(json_content);
            g_state.preview.json_preview = parsed.dump(2);
        }
        catch (...)
        {
            g_state.preview.json_preview = json_content;
        }
        g_state.preview.mode = PreviewMode::JSON;
    }
    catch (const std::exception &e)
    {
        g_state.preview.error = "Error loading JSON: " + std::string(e.what());
        g_state.preview.mode = PreviewMode::None;
    }
}

void load_db_preview(const Core::FileNode &node)
{
    try
    {
        g_state.database.column_names.clear();
        g_state.database.rows.clear();
        g_state.database.json_data.clear();
        g_state.preview.json_preview = "";
        g_state.preview.mode = PreviewMode::None;

        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
        if (file_data.empty())
        {
            g_state.preview.error = "Failed to read DB file";
            return;
        }

        std::string json_str = DBParser::ConvertToJson(file_data);
        if (json_str.empty() || json_str == "{}")
        {
            g_state.preview.json_preview = json_str;
            g_state.preview.mode = PreviewMode::JSON;
            return;
        }

        try
        {
            g_state.database.json_data = json::parse(json_str);
        }
        catch (const json::parse_error &e)
        {
            g_state.preview.json_preview = json_str;
            g_state.preview.mode = PreviewMode::JSON;
            return;
        }

        g_state.database.filename = node.name;

        if (!g_state.database.json_data.is_array() || g_state.database.json_data.empty())
        {
            g_state.preview.json_preview = g_state.database.json_data.dump(2);
            g_state.preview.mode = PreviewMode::JSON;
            return;
        }

        // Check if it looks like a table (elements are objects)
        if (g_state.database.json_data[0].is_object())
        {
            for (auto &el : g_state.database.json_data[0].items())
            {
                g_state.database.column_names.push_back(el.key());
            }

            for (auto &row : g_state.database.json_data)
            {
                if (row.is_object())
                {
                    std::vector<std::string> row_data;
                    for (const auto &col : g_state.database.column_names)
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
                    g_state.database.rows.push_back(row_data);
                }
            }
            g_state.preview.mode = PreviewMode::DB;
        }
        else
        {
            // Array of non-objects? Show as JSON
            g_state.preview.json_preview = g_state.database.json_data.dump(2);
            g_state.preview.mode = PreviewMode::JSON;
        }

        g_state.preview.error = "";
    }
    catch (const std::exception &e)
    {
        g_state.preview.error = "DB parsing error: " + std::string(e.what());
        g_state.preview.mode = PreviewMode::JSON;
    }
}

void load_scsp_preview(const Core::FileNode &node)
{
    try
    {
        g_state.preview.json_preview = "";
        g_state.preview.mode = PreviewMode::None;

        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
        if (file_data.empty())
        {
            g_state.preview.error = "Failed to read SCSP file";
            return;
        }

        std::string json_str = SCSPParser::ConvertSCSPToJson(file_data);
        if (!json_str.empty())
        {
            try
            {
                json parsed = json::parse(json_str);
                g_state.preview.json_preview = parsed.dump(2);
            }
            catch (...)
            {
                g_state.preview.json_preview = json_str;
            }
            g_state.preview.mode = PreviewMode::JSON;
        }
        else
        {
            g_state.preview.error = "Failed to parse SCSP file";
        }

        g_state.preview.error = "";
    }
    catch (const std::exception &e)
    {
        g_state.preview.error = "SCSP parsing error: " + std::string(e.what());
        g_state.preview.mode = PreviewMode::None;
    }
}

void load_text_preview(const Core::FileNode &node)
{
    try
    {
        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
        if (file_data.empty())
        {
            g_state.preview.atlas_preview = "Failed to read file";
            g_state.preview.atlas_full = "";
            g_state.preview.mode = PreviewMode::Text;
            return;
        }
        g_state.preview.atlas_full = std::string(file_data.begin(), file_data.end());
        g_state.preview.atlas_preview = g_state.preview.atlas_full;
        g_state.atlas.text_buffer.assign(g_state.preview.atlas_preview.begin(), g_state.preview.atlas_preview.end());
        g_state.atlas.text_buffer.push_back('\0');
        if (g_state.preview.atlas_preview.length() > 20000)
        {
            g_state.preview.atlas_preview = g_state.preview.atlas_preview.substr(0, 20000) + "\n\n... (truncated)";
        }
        g_state.preview.mode = PreviewMode::Text;
    }
    catch (const std::exception &e)
    {
        g_state.preview.atlas_preview = "Error loading text: " + std::string(e.what());
        g_state.preview.atlas_full = "";
        g_state.preview.mode = PreviewMode::Text;
    }
}

void load_image_preview(const Core::FileNode &node)
{
    if (g_state.preview.texture != 0)
    {
        glDeleteTextures(1, &g_state.preview.texture);
        g_state.preview.texture = 0;
    }
    g_state.preview.has_preview = false;
    g_state.preview.width = 0;
    g_state.preview.height = 0;
    g_state.preview.error = "";
    g_state.preview.atlas_preview = "";
    g_state.preview.atlas_full = "";
    g_state.preview.json_preview = "";
    g_state.database.column_names.clear();
    g_state.database.rows.clear();
    g_state.preview.mode = PreviewMode::None;
    g_state.preview.preview_node = &node;

    try
    {
        if (!std::holds_alternative<Core::FileInfo>(node.data))
        {
            g_state.preview.error = "Not a file";
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
            g_state.preview.error = "Animated WebP preview not supported. Use 'Export' to save the file.";
            return;
        }

        if (!is_previewable_format(info.format))
        {
            g_state.preview.error = "Preview not available for " + info.format + " files";
            return;
        }

        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
        if (file_data.empty())
        {
            g_state.preview.error = "Failed to read file data";
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
                    g_state.preview.error = "Failed to convert SCT/SCT2 file";
                    g_state.preview.mode = PreviewMode::None;
                    return;
                }

                SDL_RWops *rw = SDL_RWFromMem(png_data.data(), (int)png_data.size());
                if (!rw)
                {
                    g_state.preview.error = "Failed to create memory stream for SCT";
                    g_state.preview.mode = PreviewMode::None;
                    return;
                }

                SDL_Surface *surface = IMG_Load_RW(rw, 1);
                if (!surface)
                {
                    g_state.preview.error = "Failed to decode converted SCT image: " + std::string(IMG_GetError());
                    g_state.preview.mode = PreviewMode::None;
                    return;
                }

                rgba_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
                SDL_FreeSurface(surface);
            }
            catch (const std::exception &e)
            {
                g_state.preview.error = "SCT parsing error: " + std::string(e.what());
                g_state.preview.mode = PreviewMode::None;
                return;
            }
        }
        else
        {
            SDL_RWops *rw = SDL_RWFromMem(file_data.data(), (int)file_data.size());
            if (!rw)
            {
                g_state.preview.error = "Failed to create memory stream";
                g_state.preview.mode = PreviewMode::None;
                return;
            }

            SDL_Surface *surface = IMG_Load_RW(rw, 1);
            if (!surface)
            {
                g_state.preview.error = "Failed to decode image: " + std::string(IMG_GetError());
                g_state.preview.mode = PreviewMode::None;
                return;
            }

            rgba_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(surface);
        }

        if (!rgba_surface)
        {
            g_state.preview.error = "Failed to convert image format";
            g_state.preview.mode = PreviewMode::None;
            return;
        }

        g_state.preview.width = rgba_surface->w;
        g_state.preview.height = rgba_surface->h;

        glGenTextures(1, &g_state.preview.texture);
        glBindTexture(GL_TEXTURE_2D, g_state.preview.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_state.preview.width, g_state.preview.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba_surface->pixels);
        SDL_FreeSurface(rgba_surface);
        g_state.preview.has_preview = true;
        g_state.preview.mode = PreviewMode::Image;
    }
    catch (const std::exception &e)
    {
        g_state.preview.error = "Error: " + std::string(e.what());
        g_state.preview.has_preview = false;
        g_state.preview.mode = PreviewMode::None;
    }
    catch (...)
    {
        g_state.preview.error = "Unknown error occurred";
        g_state.preview.has_preview = false;
        g_state.preview.mode = PreviewMode::None;
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
            std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
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
                    g_state.tasks.status = "Exported DB to JSON: " + f.result();
                }
                catch (const json::parse_error &e)
                {
                    // Se il parsing fallisce, scrivi il JSON raw
                    std::ofstream out(f.result());
                    out << json_str;
                    out.close();
                    g_state.tasks.status = "Exported DB to JSON (unformatted): " + f.result();
                }
            }
            else
            {
                g_state.tasks.status = "Failed to convert DB to JSON";
            }
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
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
            std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
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
                    g_state.tasks.status = "Exported SCSP to JSON: " + f.result();
                }
                catch (const json::parse_error &e)
                {
                    std::ofstream out(f.result());
                    out << json_str;
                    out.close();
                    g_state.tasks.status = "Exported SCSP to JSON (unformatted): " + f.result();
                }
            }
            else
            {
                g_state.tasks.status = "Failed to convert SCSP to JSON";
            }
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
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
            std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            if (file_data.empty())
            {
                g_state.tasks.status = "Failed to read JSON file";
                return;
            }

            std::ofstream out(f.result(), std::ios::binary);
            out.write(reinterpret_cast<const char *>(file_data.data()), file_data.size());
            out.close();
            g_state.tasks.status = "Exported JSON: " + f.result();
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
    }
}

void open_image_preview_window(const Core::FileNode &node)
{
    try
    {
        if (g_state.image.window)
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

        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
        const auto &info = std::get<Core::FileInfo>(node.data);

        SDL_Surface *surface = nullptr;

        if (is_sct_format(info.format))
        {
            std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(file_data, false);
            if (png_data.empty())
            {
                g_state.tasks.status = "Failed to convert SCT for preview window";
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
            g_state.tasks.status = "Failed to load image for preview window";
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

        g_state.image.width = original_width;
        g_state.image.height = original_height;

        if (g_state.image.width > max_width || g_state.image.height > max_height)
        {
            float scale_w = (float)max_width / original_width;
            float scale_h = (float)max_height / original_height;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;

            g_state.image.width = (int)(original_width * scale);
            g_state.image.height = (int)(original_height * scale);
        }

        g_state.image.title = node.name + " (" + std::to_string(original_width) + "x" + std::to_string(original_height) + ")";

        g_state.image.window = SDL_CreateWindow(
            g_state.image.title.c_str(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            g_state.image.width,
            g_state.image.height,
            SDL_WINDOW_SHOWN);

        if (!g_state.image.window)
        {
            SDL_FreeSurface(surface);
            g_state.tasks.status = "Failed to create preview window";
            return;
        }

        g_state.image.renderer = SDL_CreateRenderer(g_state.image.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!g_state.image.renderer)
        {
            SDL_FreeSurface(surface);
            SDL_DestroyWindow(g_state.image.window);
            g_state.image.window = nullptr;
            g_state.tasks.status = "Failed to create renderer";
            return;
        }

        g_state.image.texture = SDL_CreateTextureFromSurface(g_state.image.renderer, surface);
        SDL_FreeSurface(surface);

        if (!g_state.image.texture)
        {
            SDL_DestroyRenderer(g_state.image.renderer);
            g_state.image.renderer = nullptr;
            SDL_DestroyWindow(g_state.image.window);
            g_state.image.window = nullptr;
            g_state.tasks.status = "Failed to create texture";
            return;
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Error opening image window: " + std::string(e.what());
    }
}

void render_image_window()
{
    if (!g_state.image.window || !g_state.image.renderer || !g_state.image.texture)
        return;

    SDL_RenderClear(g_state.image.renderer);
    SDL_RenderCopy(g_state.image.renderer, g_state.image.texture, nullptr, nullptr);
    SDL_RenderPresent(g_state.image.renderer);
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
            std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
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
                g_state.tasks.status = "Exported to: " + f.result();
            }
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
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
            std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            std::ofstream out(f.result(), std::ios::binary);
            out.write((const char *)file_data.data(), file_data.size());
            out.close();
            g_state.tasks.status = "Exported to: " + f.result();
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
    }
}

void handle_node_click(const Core::FileNode *node, bool is_folder)
{
    bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    Uint32 current_time = SDL_GetTicks();
    Uint32 time_diff = current_time - g_state.browser.last_click_time;

    if (time_diff < 250 && node == g_state.browser.last_clicked_node)
    {
        g_state.browser.last_click_time = current_time;
        return;
    }

    g_state.browser.click_count = 0;

    if (ctrl_pressed)
    {
        if (g_state.browser.selected_nodes.find(node) != g_state.browser.selected_nodes.end())
        {
            g_state.browser.selected_nodes.erase(node);
        }
        else
        {
            g_state.browser.selected_nodes.insert(node);
        }

        g_state.browser.selected_node = node;
        if (!is_folder)
        {
            load_image_preview(*node);
        }
        else
        {
            g_state.preview.has_preview = false;
            g_state.preview.error = "";
            g_state.preview.atlas_preview = "";
            g_state.preview.json_preview = "";
            g_state.preview.atlas_full = "";
            g_state.database.column_names.clear();
            g_state.database.rows.clear();
            g_state.preview.mode = PreviewMode::None;
            g_state.preview.preview_node = nullptr;
        }
    }
    else
    {
        g_state.browser.selected_nodes.clear();
        g_state.browser.selected_nodes.insert(node);
        g_state.browser.selected_node = node;

        if (!is_folder)
        {
            load_image_preview(*node);
        }
        else
        {
            g_state.preview.has_preview = false;
            g_state.preview.error = "";
            g_state.preview.atlas_preview = "";
            g_state.preview.json_preview = "";
            g_state.preview.atlas_full = "";
            g_state.database.column_names.clear();
            g_state.database.rows.clear();
            g_state.preview.mode = PreviewMode::None;
            g_state.preview.preview_node = nullptr;
        }
    }

    g_state.browser.last_click_time = current_time;
    g_state.browser.last_clicked_node = node;
}

void handle_node_right_click(const Core::FileNode *node, struct nk_vec2 pos)
{
    g_state.context_menu.node = node;
    g_state.context_menu.position = pos;
    g_state.context_menu.visible = true;
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
                nlohmann::ordered_json j = build_file_tree_json(g_state.browser.data_pack->GetFileTree());
                out << j.dump(2);
                out.close();
                g_state.common.success_message = "File map exported successfully!";
                g_state.common.show_success_popup = true;
                g_state.tasks.status = "Exported to: " + f.result();
            }
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
    }
}

void flatten_old_json(const nlohmann::ordered_json &j, std::map<std::string, uint64_t> &out_map)
{
    if (j.contains("type") && j["type"] == "file")
    {
        if (j.contains("path") && j.contains("size"))
        {
            out_map[j["path"].get<std::string>()] = j["size"].get<uint64_t>();
        }
    }
    else if (j.contains("type") && j["type"] == "folder" && j.contains("children"))
    {
        for (const auto &child : j["children"])
        {
            flatten_old_json(child, out_map);
        }
    }
}

void flatten_new_tree(const Core::FileNode &node, std::map<std::string, uint64_t> &out_map)
{
    if (std::holds_alternative<Core::FileInfo>(node.data))
    {
        const auto &info = std::get<Core::FileInfo>(node.data);
        out_map[node.full_path] = info.size;
    }
    else
    {
        const auto &folder = std::get<Core::FolderInfo>(node.data);
        for (const auto &child : folder.children)
        {
            flatten_new_tree(child, out_map);
        }
    }
}

void insert_diff_node(DiffNode *root, const std::string &path, uint64_t size, DiffStatus status)
{
    std::string norm_path = path;
    std::replace(norm_path.begin(), norm_path.end(), '\\', '/');

    std::vector<std::string> parts;
    std::stringstream ss(norm_path);
    std::string part;
    while (std::getline(ss, part, '/'))
    {
        if (!part.empty())
            parts.push_back(part);
    }

    DiffNode *current = root;
    std::string current_full_path = "";
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (!current_full_path.empty())
            current_full_path += "/";
        current_full_path += parts[i];

        bool is_last = (i == parts.size() - 1);

        auto it = std::find_if(current->children.begin(), current->children.end(), [&](const std::unique_ptr<DiffNode> &n)
                               { return n->name == parts[i]; });

        if (it != current->children.end())
        {
            current = it->get();
            if (is_last)
            {
                if (status != DiffStatus::Unchanged)
                    current->status = status;
            }
            else
            {
                if (status != DiffStatus::Unchanged && current->status == DiffStatus::Unchanged)
                {
                    current->status = DiffStatus::Modified;
                }
            }
        }
        else
        {
            auto new_node = std::make_unique<DiffNode>();
            new_node->name = parts[i];
            new_node->full_path = current_full_path;
            new_node->is_folder = !is_last;

            if (is_last)
            {
                new_node->size = size;
                new_node->status = status;
                size_t dot_pos = parts[i].find_last_of('.');
                if (dot_pos != std::string::npos)
                {
                    new_node->format = parts[i].substr(dot_pos);
                }
                else
                {
                    new_node->format = "";
                }
            }
            else
            {
                new_node->size = 0;
                new_node->status = (status == DiffStatus::Unchanged) ? DiffStatus::Unchanged : DiffStatus::Modified;
            }

            current->children.push_back(std::move(new_node));
            current = current->children.back().get();
        }
    }
}

void build_diff_tree(const nlohmann::ordered_json &old_json)
{
    std::map<std::string, uint64_t> old_map;
    flatten_old_json(old_json, old_map);

    std::map<std::string, uint64_t> new_map;
    if (g_state.browser.data_pack)
    {
        flatten_new_tree(g_state.browser.data_pack->GetFileTree(), new_map);
    }

    g_state.diff.root = std::make_unique<DiffNode>();
    g_state.diff.root->name = "Diff Root";
    g_state.diff.root->full_path = "";
    g_state.diff.root->is_folder = true;
    g_state.diff.root->status = DiffStatus::Unchanged;

    for (const auto &kv : old_map)
    {
        const std::string &path = kv.first;
        uint64_t old_size = kv.second;

        auto it = new_map.find(path);
        if (it == new_map.end())
        {
            insert_diff_node(g_state.diff.root.get(), path, old_size, DiffStatus::Removed);
        }
        else
        {
            if (it->second != old_size)
            {
                insert_diff_node(g_state.diff.root.get(), path, it->second, DiffStatus::Modified);
            }
            else
            {
                insert_diff_node(g_state.diff.root.get(), path, it->second, DiffStatus::Unchanged);
            }
        }
    }

    for (const auto &kv : new_map)
    {
        const std::string &path = kv.first;
        uint64_t new_size = kv.second;
        if (old_map.find(path) == old_map.end())
        {
            insert_diff_node(g_state.diff.root.get(), path, new_size, DiffStatus::Added);
        }
    }

    std::function<void(DiffNode *)> sort_tree = [&](DiffNode *node)
    {
        std::sort(node->children.begin(), node->children.end(), [](const std::unique_ptr<DiffNode> &a, const std::unique_ptr<DiffNode> &b)
                  {
            if (a->is_folder != b->is_folder) return a->is_folder > b->is_folder;
            return a->name < b->name; });
        for (auto &child : node->children)
        {
            sort_tree(child.get());
        }
    };
    sort_tree(g_state.diff.root.get());

    g_state.diff.show_tree = true;
    g_state.diff.expanded_folders.clear();
    g_state.diff.expanded_folders.insert(g_state.diff.root.get());
    g_state.diff.selected_node = nullptr;
    g_state.diff.selected_nodes.clear();
}

void set_file_tree_mode()
{
    g_state.spine.show_window = false;
    g_state.diff.show_tree = false;
}

void set_spine_viewer_mode()
{
    g_state.diff.show_tree = false;
    g_state.spine.show_window = true;
}

bool load_diff_tree_from_filemap()
{
    try
    {
        auto f = pfd::open_file("Select an older filemap.json", ".", {"JSON Files", "*.json", "All Files", "*.*"});
        if (f.result().empty())
        {
            return false;
        }

        std::ifstream in(f.result()[0]);
        if (!in.is_open())
        {
            g_state.tasks.status = "Error opening filemap.json for diff.";
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        nlohmann::ordered_json old_json = nlohmann::ordered_json::parse(content);
        build_diff_tree(old_json);
        g_state.tasks.status = "Diff view generated.";
        return true;
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Error parsing JSON: " + std::string(e.what());
        return false;
    }
}

bool activate_diff_viewer()
{
    if (!g_state.diff.root)
    {
        if (!load_diff_tree_from_filemap())
        {
            return false;
        }
    }

    g_state.spine.show_window = false;
    g_state.diff.show_tree = true;
    return true;
}

std::string diff_status_label(DiffStatus status)
{
    switch (status)
    {
    case DiffStatus::Added:
        return "[ADD] ";
    case DiffStatus::Modified:
        return "[MOD] ";
    case DiffStatus::Removed:
        return "[DEL] ";
    default:
        return "";
    }
}

std::string diff_display_name(const DiffNode &node)
{
    return diff_status_label(node.status) + node.name;
}

const Core::FileNode* find_file_node_by_path(const Core::FileNode& current, const std::string& path)
{
    if (current.full_path == path)
        return &current;

    if (std::holds_alternative<Core::FolderInfo>(current.data))
    {
        for (const auto& child : std::get<Core::FolderInfo>(current.data).children)
        {
            if (const Core::FileNode* found = find_file_node_by_path(child, path))
                return found;
        }
    }
    return nullptr;
}

void handle_diff_node_click(const DiffNode *node, bool is_folder)
{
    bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
    Uint32 current_time = SDL_GetTicks();
    Uint32 time_diff = current_time - g_state.browser.last_click_time;

    if (time_diff < 250 && node == g_state.diff.last_clicked_node)
    {
        g_state.browser.last_click_time = current_time;
        return;
    }

    if (ctrl_pressed)
    {
        if (g_state.diff.selected_nodes.find(node) != g_state.diff.selected_nodes.end())
        {
            g_state.diff.selected_nodes.erase(node);
        }
        else
        {
            g_state.diff.selected_nodes.insert(node);
        }
        g_state.diff.selected_node = node;
        
        if (!is_folder && g_state.browser.data_pack)
        {
            const Core::FileNode* file_node = find_file_node_by_path(g_state.browser.data_pack->GetFileTree(), node->full_path);
            if (file_node)
            {
                load_image_preview(*file_node);
            }
            else
            {
                g_state.preview.has_preview = false;
                g_state.preview.error = "File not found in current datapack (perhaps it was removed).";
                g_state.preview.atlas_preview = "";
                g_state.preview.json_preview = "";
                g_state.preview.atlas_full = "";
                g_state.database.column_names.clear();
                g_state.database.rows.clear();
                g_state.preview.mode = PreviewMode::None;
                g_state.preview.preview_node = nullptr;
            }
        }
        else if (is_folder)
        {
            g_state.preview.has_preview = false;
            g_state.preview.error = "";
            g_state.preview.atlas_preview = "";
            g_state.preview.json_preview = "";
            g_state.preview.atlas_full = "";
            g_state.database.column_names.clear();
            g_state.database.rows.clear();
            g_state.preview.mode = PreviewMode::None;
            g_state.preview.preview_node = nullptr;
        }
    }
    else
    {
        g_state.diff.selected_nodes.clear();
        g_state.diff.selected_nodes.insert(node);
        g_state.diff.selected_node = node;

        if (!is_folder && g_state.browser.data_pack)
        {
            const Core::FileNode* file_node = find_file_node_by_path(g_state.browser.data_pack->GetFileTree(), node->full_path);
            if (file_node)
            {
                load_image_preview(*file_node);
            }
            else
            {
                g_state.preview.has_preview = false;
                g_state.preview.error = "File not found in current datapack (perhaps it was removed).";
                g_state.preview.atlas_preview = "";
                g_state.preview.json_preview = "";
                g_state.preview.atlas_full = "";
                g_state.database.column_names.clear();
                g_state.database.rows.clear();
                g_state.preview.mode = PreviewMode::None;
                g_state.preview.preview_node = nullptr;
            }
        }
        else if (is_folder)
        {
            g_state.preview.has_preview = false;
            g_state.preview.error = "";
            g_state.preview.atlas_preview = "";
            g_state.preview.json_preview = "";
            g_state.preview.atlas_full = "";
            g_state.database.column_names.clear();
            g_state.database.rows.clear();
            g_state.preview.mode = PreviewMode::None;
            g_state.preview.preview_node = nullptr;
        }
    }

    g_state.browser.last_click_time = current_time;
    g_state.diff.last_clicked_node = node;
}

int get_diff_file_count(const DiffNode &node)
{
    if (!node.is_folder)
        return 1;
    int count = 0;
    for (const auto &child : node.children)
    {
        count += get_diff_file_count(*child);
    }
    return count;
}

uint64_t get_diff_folder_size(const DiffNode &node)
{
    if (!node.is_folder)
        return node.size;
    uint64_t size = 0;
    for (const auto &child : node.children)
    {
        size += get_diff_folder_size(*child);
    }
    return size;
}

bool matches_diff_search(const DiffNode &node, const std::string &query)
{
    if (query.empty())
        return true;
    std::string name_lower = node.name;
    std::string query_lower = query;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
    return name_lower.find(query_lower) != std::string::npos;
}

bool has_matching_diff_child(const DiffNode &node, const std::string &query)
{
    if (query.empty())
        return true;
    if (matches_diff_search(node, query))
        return true;
    if (node.is_folder)
    {
        for (const auto &child : node.children)
        {
            if (has_matching_diff_child(*child, query))
                return true;
        }
    }
    return false;
}

void draw_diff_node(nk_context *ctx, const DiffNode &node, int depth = 0)
{
    try
    {
        if (!has_matching_diff_child(node, g_state.browser.search_query))
            return;

        g_state.diff.visible_nodes.push_back(&node);

        if (node.is_folder)
        {
            bool is_expanded = g_state.diff.expanded_folders.find(&node) != g_state.diff.expanded_folders.end();
            bool is_selected = (g_state.diff.selected_nodes.find(&node) != g_state.diff.selected_nodes.end()) || (g_state.diff.selected_node == &node);

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
                    g_state.diff.expanded_folders.erase(&node);
                else
                    g_state.diff.expanded_folders.insert(&node);
            }

            nk_layout_row_push(ctx, 370.0f);
            struct nk_style_button button_style = ctx->style.button;
            button_style.normal = nk_style_item_color(bg_color);
            button_style.hover = nk_style_item_color(is_selected ? nk_rgb(85, 85, 95) : nk_rgb(50, 50, 55));
            button_style.active = nk_style_item_color(nk_rgb(70, 70, 80));

            struct nk_color text_color = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(220, 220, 220);
            if (!is_selected)
            {
                if (node.status == DiffStatus::Added)
                    text_color = nk_rgb(100, 255, 100);
                else if (node.status == DiffStatus::Modified)
                    text_color = nk_rgb(255, 200, 50);
                else if (node.status == DiffStatus::Removed)
                    text_color = nk_rgb(255, 100, 100);
            }

            button_style.text_normal = text_color;
            button_style.text_hover = nk_rgb(255, 255, 255);
            button_style.text_active = nk_rgb(255, 255, 255);
            button_style.text_alignment = NK_TEXT_LEFT;
            button_style.padding = nk_vec2(8, 4);
            button_style.rounding = 3.0f;

            std::string folder_label = diff_display_name(node);
            if (nk_button_label_styled(ctx, &button_style, folder_label.c_str()))
            {
                handle_diff_node_click(&node, true);
            }

            nk_layout_row_push(ctx, 200.0f);
            int file_count = get_diff_file_count(node);
            std::string info = std::to_string(file_count) + " items | " + format_size(get_diff_folder_size(node));
            nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

            nk_layout_row_end(ctx);

            if (is_expanded)
            {
                for (const auto &child : node.children)
                    draw_diff_node(ctx, *child, depth + 1);
            }
        }
        else
        {
            if (!matches_diff_search(node, g_state.browser.search_query))
                return;

            bool is_selected = (g_state.diff.selected_nodes.find(&node) != g_state.diff.selected_nodes.end()) || (g_state.diff.selected_node == &node);

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

            struct nk_color text_color = is_selected ? nk_rgb(255, 255, 255) : nk_rgb(200, 200, 200);
            if (!is_selected)
            {
                if (node.status == DiffStatus::Added)
                    text_color = nk_rgb(100, 255, 100);
                else if (node.status == DiffStatus::Modified)
                    text_color = nk_rgb(255, 200, 50);
                else if (node.status == DiffStatus::Removed)
                    text_color = nk_rgb(255, 100, 100);
            }

            button_style.text_normal = text_color;
            button_style.text_hover = nk_rgb(255, 255, 255);
            button_style.text_active = nk_rgb(255, 255, 255);
            button_style.text_alignment = NK_TEXT_LEFT;
            button_style.padding = nk_vec2(8, 4);
            button_style.rounding = 3.0f;

            std::string file_label = diff_display_name(node);

            if (nk_button_label_styled(ctx, &button_style, file_label.c_str()))
            {
                handle_diff_node_click(&node, false);
            }

            nk_layout_row_push(ctx, 200.0f);
            std::string size_str = format_size(node.size) + " | " + node.format;
            nk_label_colored(ctx, size_str.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

            nk_layout_row_end(ctx);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error drawing diff node: " << e.what() << std::endl;
    }
}

void draw_file_node(nk_context *ctx, const Core::FileNode &node, int depth = 0)
{
    try
    {
        if (!has_matching_child(node, g_state.browser.search_query))
            return;

        g_state.browser.visible_nodes.push_back(&node);

        if (std::holds_alternative<Core::FolderInfo>(node.data))
        {
            const auto &folder = std::get<Core::FolderInfo>(node.data);
            bool is_expanded = g_state.browser.expanded_folders.find(&node) != g_state.browser.expanded_folders.end();
            bool is_selected = (g_state.browser.selected_nodes.find(&node) != g_state.browser.selected_nodes.end()) || (g_state.browser.selected_node == &node);

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
                    g_state.browser.expanded_folders.erase(&node);
                }
                else
                {
                    g_state.browser.expanded_folders.insert(&node);
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
            bool highlight_match = !g_state.browser.search_query.empty() && matches_search(node, g_state.browser.search_query);
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
            if (!matches_search(node, g_state.browser.search_query))
                return;

            const auto &file_info = std::get<Core::FileInfo>(node.data);
            bool is_selected = (g_state.browser.selected_nodes.find(&node) != g_state.browser.selected_nodes.end()) || (g_state.browser.selected_node == &node);

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

bool spine_category_has_search_match(const SpineCategory &cat, const std::vector<SpineEntry> &entries, const std::string &query_lower)
{
    if (query_lower.empty())
        return true;
    // Check direct entries
    for (size_t idx : cat.entry_indices)
    {
        std::string dn = entries[idx].display_name;
        std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
        if (dn.find(query_lower) != std::string::npos)
            return true;
        // Also match against the full category path
        std::string cp = entries[idx].category;
        std::transform(cp.begin(), cp.end(), cp.begin(), ::tolower);
        if (cp.find(query_lower) != std::string::npos)
            return true;
    }
    // Check subcategories recursively
    for (const auto &[name, sub] : cat.subcategories)
    {
        if (spine_category_has_search_match(sub, entries, query_lower))
            return true;
    }
    return false;
}

void draw_spine_category(nk_context *ctx, const SpineCategory &cat, const std::vector<SpineEntry> &entries, int depth)
{
    std::string query_lower = g_state.spine.search_query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    // For each subcategory, draw as a collapsible folder
    for (const auto &[subname, sub] : cat.subcategories)
    {
        // Check if this subtree has any matching entries
        if (!spine_category_has_search_match(sub, entries, query_lower))
            continue;

        // Use full_path as the unique key for expand/collapse
        bool expanded = g_state.spine.expanded_categories.count(sub.full_path) > 0;
        // Auto-expand when searching
        if (!query_lower.empty())
            expanded = true;

        // Count total entries under this subtree recursively
        std::function<int(const SpineCategory &)> count_entries = [&](const SpineCategory &c) -> int
        {
            int n = (int)c.entry_indices.size();
            for (const auto &[k, sc] : c.subcategories)
                n += count_entries(sc);
            return n;
        };
        int total = count_entries(sub);

        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        // Indent
        float indent = depth * 16.0f;
        if (indent > 0)
        {
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

        // Draw entries directly in this folder
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

            g_state.spine.visible_indices.push_back((int)idx);

            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            float entry_indent = (depth + 1) * 16.0f;
            nk_layout_row_push(ctx, entry_indent);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 300.0f - entry_indent);
            bool isSel = ((int)idx == g_state.spine.selected_index);
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
                if (g_state.spine.selected_index != (int)idx)
                {
                    g_state.spine.selected_index = (int)idx;
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

        // Recurse into subcategories
        draw_spine_category(ctx, sub, entries, depth + 1);
    }

    // Also draw any entries directly at this level (root-level entries)
    if (depth == 0)
    {
        for (size_t idx : cat.entry_indices)
        {
            const auto &ent = entries[idx];

            if (!query_lower.empty())
            {
                std::string dn = ent.display_name;
                std::transform(dn.begin(), dn.end(), dn.begin(), ::tolower);
                if (dn.find(query_lower) == std::string::npos)
                    continue;
            }

            g_state.spine.visible_indices.push_back((int)idx);

            nk_layout_row_dynamic(ctx, 24, 1);
            bool isSel = ((int)idx == g_state.spine.selected_index);
            struct nk_style_button ebtn = ctx->style.button;
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
                if (g_state.spine.selected_index != (int)idx)
                {
                    g_state.spine.selected_index = (int)idx;
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

int main(int argc, char *argv[])
{
    load_options_from_ini();

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
                            const auto &entries = g_state.spine.dictionary.GetEntries();
                            if (g_state.spine.selected_index >= 0 && g_state.spine.selected_index < (int)entries.size())
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
                        const auto &entries = g_state.spine.dictionary.GetEntries();
                        if (g_state.spine.selected_index < (int)entries.size())
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
                else if (g_state.browser.selected_node)
                {
                    // File tree arrow keys (only when spine viewer is NOT open)
                    if ((evt.key.keysym.sym == SDLK_UP || evt.key.keysym.sym == SDLK_DOWN) && !g_state.browser.visible_nodes.empty())
                    {
                        auto it = std::find(g_state.browser.visible_nodes.begin(), g_state.browser.visible_nodes.end(), g_state.browser.selected_node);
                        if (it != g_state.browser.visible_nodes.end())
                        {
                            if (evt.key.keysym.sym == SDLK_UP && it > g_state.browser.visible_nodes.begin())
                            {
                                g_state.browser.selected_node = *(it - 1);
                                handle_node_click(g_state.browser.selected_node, std::holds_alternative<Core::FolderInfo>(g_state.browser.selected_node->data));
                                scroll_to_selected = true;
                            }
                            else if (evt.key.keysym.sym == SDLK_DOWN && it < g_state.browser.visible_nodes.end() - 1)
                            {
                                g_state.browser.selected_node = *(it + 1);
                                handle_node_click(g_state.browser.selected_node, std::holds_alternative<Core::FolderInfo>(g_state.browser.selected_node->data));
                                scroll_to_selected = true;
                            }
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_RETURN)
                    {
                        if (std::holds_alternative<Core::FolderInfo>(g_state.browser.selected_node->data))
                        {
                            if (g_state.browser.expanded_folders.find(g_state.browser.selected_node) != g_state.browser.expanded_folders.end())
                                g_state.browser.expanded_folders.erase(g_state.browser.selected_node);
                            else
                                g_state.browser.expanded_folders.insert(g_state.browser.selected_node);
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_RIGHT)
                    {
                        if (std::holds_alternative<Core::FolderInfo>(g_state.browser.selected_node->data))
                        {
                            g_state.browser.expanded_folders.insert(g_state.browser.selected_node);
                        }
                    }
                    else if (evt.key.keysym.sym == SDLK_LEFT)
                    {
                        if (std::holds_alternative<Core::FolderInfo>(g_state.browser.selected_node->data))
                        {
                            g_state.browser.expanded_folders.erase(g_state.browser.selected_node);
                        }
                    }
                }
            }
            nk_sdl_handle_event(&evt);
        }
        nk_input_end(ctx);

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
            if (g_state.tasks.status.find("Scanning") != std::string::npos)
            {
                g_state.tasks.scan_complete = true;
                g_state.tasks.status = "Scan complete. " + std::to_string(get_file_count(g_state.browser.data_pack->GetFileTree())) + " files found.";
            }
            else if (g_state.tasks.status.find("Extracting") != std::string::npos)
            {
                g_state.tasks.status = "Extraction complete.";
            }
        }

        int window_width, window_height;
        SDL_GetWindowSize(win, &window_width, &window_height);

        if (g_state.context_menu.visible && g_state.context_menu.node)
        {
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
                            auto f = pfd::save_file("Extract File", g_state.context_menu.node->name, {"All Files", "*.*"});
                            if (!f.result().empty())
                            {
                                std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(*g_state.context_menu.node);
                                std::ofstream out(f.result(), std::ios::binary);
                                out.write((const char *)file_data.data(), file_data.size());
                                out.close();
                                g_state.tasks.status = "Extracted to: " + f.result();
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

        if (g_state.common.show_options)
        {
            const float export_options_width = 530.0f;
            const float export_options_height = 460.0f;
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
                    if (g_state.common.export_sct_as_png)
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
                    struct nk_style_button toggle_style = ctx->style.button;
                    if (g_state.common.export_db_as_json)
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
                    struct nk_style_button toggle_style = ctx->style.button;
                    if (g_state.common.enable_open_folder)
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

        if (g_state.credits.show_window)
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
                    g_state.credits.show_window = false;
                }
            }
            else
            {
                g_state.credits.show_window = false;
            }
            nk_end(ctx);
        }

        if (g_state.common.show_success_popup)
        {
            const float export_success_width = 215.0f;
            const float export_success_height = 120.0f;
            const float export_success_x = (window_width - export_success_width) * 0.5f;
            const float export_success_y = (window_height - export_success_height) * 0.5f;
            if (nk_begin(ctx, "Success", nk_rect(export_success_x, export_success_y, export_success_width, export_success_height),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE))
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

        if (g_state.atlas.show_window && !g_state.preview.atlas_preview.empty())
        {
            if (nk_begin(ctx, "Atlas Viewer", nk_rect(40, 40, (float)window_width - 80, (float)window_height - 80),
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE))
            {

                nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
                nk_layout_row_push(ctx, 120);
                if (nk_button_label(ctx, "Copy All"))
                {
                    SDL_SetClipboardText(g_state.preview.atlas_full.c_str());
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
                                out << g_state.preview.atlas_full;
                                out.close();
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                }
                nk_layout_row_end(ctx);

                if (g_state.atlas.text_buffer.empty())
                {
                    g_state.atlas.text_buffer.push_back('\0');
                }
                nk_layout_row_dynamic(ctx, (float)window_height - 180, 1);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY, g_state.atlas.text_buffer.data(), (int)g_state.atlas.text_buffer.size(), nk_filter_default);
            }
            else
            {
                g_state.atlas.show_window = false;
            }
            nk_end(ctx);
        }

        if (nk_begin(ctx, "Main", nk_rect(0, 0, (float)window_width, (float)window_height), NK_WINDOW_NO_SCROLLBAR))
        {
            bool pack_loaded = (g_state.browser.data_pack != nullptr);
            bool tree_scanned = pack_loaded && g_state.tasks.scan_complete.load();
            bool selection_exists = g_state.diff.show_tree ? (g_state.diff.selected_node != nullptr) : (g_state.browser.selected_node != nullptr);
            bool has_file_selection = g_state.diff.show_tree ? !g_state.diff.selected_nodes.empty() : !g_state.browser.selected_nodes.empty();
            bool has_extract_selection = has_file_selection || selection_exists;

            nk_layout_row_dynamic(ctx, 38, g_state.common.enable_open_folder ? 10 : 9);

            struct nk_style_button btn_style = ctx->style.button;
            btn_style.rounding = 4.0f;
            btn_style.padding = nk_vec2(10, 8);
            btn_style.normal = nk_style_item_color(nk_rgb(70, 70, 75));
            btn_style.hover = nk_style_item_color(nk_rgb(90, 90, 95));

            bool pack_already_loaded = (g_state.browser.data_pack != nullptr);
            bool can_open_pack = !g_state.tasks.running && !pack_already_loaded;

            if (can_open_pack && nk_button_label_styled(ctx, &btn_style, "Open Pack"))
            {
                try
                {
                    auto f = pfd::open_file("Select an archive or manifest file", ".",
                                            {"Pack / Manifest Files", "*.pack;*.ssra", "All Files", "*.*"});
                    if (!f.result().empty())
                    {
                        std::string selected_path = f.result()[0];
                        int size_needed = MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                                                              (int)selected_path.size(), NULL, 0);
                        std::wstring wpath(size_needed, 0);
                        MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                                            (int)selected_path.size(), &wpath[0], size_needed);

                        g_state.browser.data_pack.reset();
                        g_state.tasks.scan_complete = false;
                        g_state.browser.selected_node = nullptr;
                        g_state.browser.selected_nodes.clear();
                        g_state.browser.expanded_folders.clear();
                        g_state.preview.has_preview = false;
                        g_state.preview.error = "";
                        g_state.preview.atlas_preview = "";
                        g_state.preview.json_preview = "";
                        g_state.preview.atlas_full = "";
                        g_state.database.column_names.clear();
                        g_state.database.rows.clear();
                        g_state.browser.search_query = "";
                        memset(g_state.browser.search_buffer, 0, sizeof(g_state.browser.search_buffer));
                        g_state.preview.mode = PreviewMode::None;
                        if (g_state.spine.build_future.valid())
                            g_state.spine.build_future.wait();
                        g_state.spine.dictionary.Clear();
                        g_state.spine.show_window = false;
                        g_state.spine.selected_index = -1;
                        memset(g_state.spine.search_buffer, 0, sizeof(g_state.spine.search_buffer));
                        g_state.spine.search_query = "";
                        if (g_state.spine.viewer)
                            g_state.spine.viewer->unload();
                        g_state.spine.viewer.reset();
                        g_state.spine.expanded_categories.clear();
                        g_state.diff.show_tree = false;
                        g_state.diff.root.reset();
                        g_state.diff.visible_nodes.clear();
                        g_state.diff.expanded_folders.clear();
                        g_state.diff.selected_nodes.clear();
                        g_state.diff.selected_node = nullptr;

                        g_state.browser.data_pack = CreateArchive(wpath);
                        if (g_state.browser.data_pack->GetType() == IArchive::PackType::Unknown)
                        {
                            g_state.tasks.status = "Error: Invalid or unknown file.";
                            g_state.browser.data_pack = nullptr;
                        }
                        else
                        {
                            g_state.tasks.status = "Loaded. Click 'Scan Tree' to analyze contents.";
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    g_state.tasks.status = "Error opening file: " + std::string(e.what());
                }
            }
            else if (g_state.tasks.running || pack_already_loaded)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Open Pack");
                nk_widget_disable_end(ctx);
            }

            if (g_state.common.enable_open_folder)
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

                            g_state.browser.data_pack.reset();
                            g_state.tasks.scan_complete = false;
                            g_state.browser.selected_node = nullptr;
                            g_state.browser.selected_nodes.clear();
                            g_state.browser.expanded_folders.clear();
                            g_state.preview.has_preview = false;
                            g_state.preview.error = "";
                            g_state.preview.atlas_preview = "";
                            g_state.preview.json_preview = "";
                            g_state.preview.atlas_full = "";
                            g_state.database.column_names.clear();
                            g_state.database.rows.clear();
                            g_state.browser.search_query = "";
                            memset(g_state.browser.search_buffer, 0, sizeof(g_state.browser.search_buffer));
                            g_state.preview.mode = PreviewMode::None;
                            if (g_state.spine.build_future.valid())
                                g_state.spine.build_future.wait();
                            g_state.spine.dictionary.Clear();
                            g_state.spine.show_window = false;
                            g_state.spine.selected_index = -1;
                            memset(g_state.spine.search_buffer, 0, sizeof(g_state.spine.search_buffer));
                            g_state.spine.search_query = "";
                            if (g_state.spine.viewer)
                                g_state.spine.viewer->unload();
                            g_state.spine.viewer.reset();
                            g_state.spine.expanded_categories.clear();
                            g_state.diff.show_tree = false;
                            g_state.diff.root.reset();
                            g_state.diff.visible_nodes.clear();
                            g_state.diff.expanded_folders.clear();
                            g_state.diff.selected_nodes.clear();
                            g_state.diff.selected_node = nullptr;

                            g_state.browser.data_pack = CreateArchive(wpath);
                            if (g_state.browser.data_pack->GetType() == IArchive::PackType::Unknown)
                            {
                                g_state.tasks.status = "Error: Invalid or unknown folder.";
                                g_state.browser.data_pack = nullptr;
                            }
                            else
                            {
                                g_state.tasks.status = "Folder Loaded. Click 'Scan Tree' to build the view.";
                            }
                        }
                    }
                    catch (const std::exception &e)
                    {
                        g_state.tasks.status = "Error opening folder: " + std::string(e.what());
                    }
                }
                else if (g_state.tasks.running || pack_already_loaded)
                {
                    nk_widget_disable_begin(ctx);
                    nk_button_label_styled(ctx, &btn_style, "Open Folder");
                    nk_widget_disable_end(ctx);
                }
            }

            if (pack_loaded && !tree_scanned && !g_state.tasks.running && nk_button_label_styled(ctx, &btn_style, "Scan Tree"))
            {
                try
                {
                    g_state.tasks.running = true;
                    g_state.tasks.scan_complete = false;
                    g_state.tasks.status = "Scanning...";
                    g_state.tasks.progress = 0.0f;

                    g_state.browser.expanded_folders.clear();
                    g_state.browser.selected_node = nullptr;
                    g_state.browser.selected_nodes.clear();
                    g_state.browser.last_clicked_node = nullptr;
                    g_state.preview.has_preview = false;
                    g_state.preview.error = "";
                    g_state.preview.atlas_preview = "";
                    g_state.preview.json_preview = "";
                    g_state.preview.atlas_full = "";
                    g_state.database.column_names.clear();
                    g_state.database.rows.clear();
                    g_state.preview.mode = PreviewMode::None;

                    if (g_state.preview.texture)
                    {
                        glDeleteTextures(1, &g_state.preview.texture);
                        g_state.preview.texture = 0;
                    }
                    g_state.tasks.future = std::async(std::launch::async, []{
                        try {
                            g_state.browser.data_pack->Scan(g_state.tasks.progress);
                        }catch (...) {} });
                }
                catch (const std::exception &e)
                {
                    g_state.tasks.status = "Error starting scan: " + std::string(e.what());
                    g_state.tasks.running = false;
                }
            }
            else if (!pack_loaded || tree_scanned || g_state.tasks.running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Scan Tree");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !g_state.tasks.running && nk_button_label_styled(ctx, &btn_style, g_state.spine.show_window ? "File Tree" : "Spine Viewer"))
            {
                if (!g_state.spine.show_window)
                {
                    if (!g_state.spine.dictionary.IsBuilt() && !g_state.spine.building)
                    {
                        g_state.spine.building = true;
                        g_state.spine.build_future = std::async(std::launch::async, []()
                                                                {
                                    try {
                                        g_state.spine.dictionary.Build(*g_state.browser.data_pack, g_state.browser.data_pack->GetFileTree());
                                    } catch (...) {}
                                    g_state.spine.building = false; });
                    }
                    set_spine_viewer_mode();
                }
                else
                {
                    set_file_tree_mode();
                }
            }
            else if (!tree_scanned || g_state.tasks.running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Spine Viewer");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !g_state.tasks.running && nk_button_label_styled(ctx, &btn_style, g_state.diff.show_tree ? "File Tree" : "Diff Viewer"))
            {
                if (g_state.diff.show_tree)
                {
                    set_file_tree_mode();
                }
                else
                {
                    activate_diff_viewer();
                }
            }
            else if (!tree_scanned || g_state.tasks.running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, g_state.diff.show_tree ? "File Tree" : "Diff Viewer");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !g_state.diff.show_tree && !g_state.spine.show_window && !g_state.tasks.running && nk_button_label_styled(ctx, &btn_style, "Extract All"))
            {
                try
                {
                    auto d = pfd::select_folder("Select destination folder", ".");
                    if (!d.result().empty())
                    {
                        std::string dest_str = d.result();
                        std::wstring dest_path = Core::Utf8ToWString(dest_str);
                        g_state.tasks.running = true;
                        g_state.tasks.status = "Extracting all files...";
                        g_state.tasks.progress = 0.0f;
                        bool convert_sct = (g_state.common.export_sct_as_png != 0);
                        bool convert_db = (g_state.common.export_db_as_json != 0);
                        g_state.tasks.future = std::async(std::launch::async, [dest_path, convert_sct, convert_db]()
                                                 {
                            try {
                                g_state.browser.data_pack->Extract(g_state.browser.data_pack->GetFileTree(), dest_path, g_state.tasks.progress, convert_sct, convert_db);
                            }
                            catch (...) {} });
                    }
                }
                catch (const std::exception &e)
                {
                    g_state.tasks.status = "Error starting extraction: " + std::string(e.what());
                }
            }
            else if (!tree_scanned || g_state.diff.show_tree || g_state.tasks.running || g_state.spine.show_window)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract All");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !g_state.spine.show_window && has_extract_selection && !g_state.tasks.running &&
                nk_button_label_styled(ctx, &btn_style, "Extract Selected"))
            {

                try
                {
                    auto d = pfd::select_folder("Select destination folder", ".");
                    if (!d.result().empty())
                    {
                        std::string dest_str = d.result();
                        std::wstring dest_path = Core::Utf8ToWString(dest_str);
                        g_state.tasks.running = true;
                        std::vector<const Core::FileNode *> nodes_to_extract;
                        if (g_state.diff.show_tree)
                        {
                            nodes_to_extract.reserve(g_state.diff.selected_nodes.size() + 1);
                            for (const auto *n : g_state.diff.selected_nodes)
                            {
                                if (n)
                                {
                                    if (const Core::FileNode* fn = find_file_node_by_path(g_state.browser.data_pack->GetFileTree(), n->full_path))
                                        nodes_to_extract.push_back(fn);
                                }
                            }
                            if (nodes_to_extract.empty() && g_state.diff.selected_node)
                            {
                                if (const Core::FileNode* fn = find_file_node_by_path(g_state.browser.data_pack->GetFileTree(), g_state.diff.selected_node->full_path))
                                    nodes_to_extract.push_back(fn);
                            }
                        }
                        else
                        {
                            nodes_to_extract.reserve(g_state.browser.selected_nodes.size() + 1);
                            for (const auto *n : g_state.browser.selected_nodes)
                            {
                                if (n)
                                    nodes_to_extract.push_back(n);
                            }
                            if (nodes_to_extract.empty() && g_state.browser.selected_node)
                            {
                                nodes_to_extract.push_back(g_state.browser.selected_node);
                            }
                        }

                        g_state.tasks.status = "Extracting " + std::to_string(nodes_to_extract.size()) + " item(s)...";
                        g_state.tasks.progress = 0.0f;
                        bool convert_sct = (g_state.common.export_sct_as_png != 0);
                        bool convert_db = (g_state.common.export_db_as_json != 0);
                        g_state.tasks.future = std::async(std::launch::async, [dest_path, nodes_to_extract, convert_sct, convert_db]()
                                                 {
                            try {
                                const float total = nodes_to_extract.empty() ? 1.0f : (float)nodes_to_extract.size();
                                for (size_t i = 0; i < nodes_to_extract.size(); i++)
                                {
                                    std::atomic<float> local_progress = 0.0f;
                                    g_state.browser.data_pack->Extract(*nodes_to_extract[i], dest_path, local_progress, convert_sct, convert_db);
                                    g_state.tasks.progress = (float)(i + 1) / total;
                                }
                            }
                            catch (...) {} });
                    }
                }
                catch (const std::exception &e)
                {
                    g_state.tasks.status = "Error starting extraction: " + std::string(e.what());
                }
            }
            else if (!tree_scanned || !has_extract_selection || g_state.tasks.running || g_state.spine.show_window)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract Selected");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !g_state.tasks.running && nk_button_label_styled(ctx, &btn_style, "Export filemap JSON"))
            {
                export_to_json();
            }
            else if (!tree_scanned || g_state.tasks.running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Export filemap JSON");
                nk_widget_disable_end(ctx);
            }

            if (!g_state.tasks.running && nk_button_label_styled(ctx, &btn_style, "Options"))
            {
                g_state.common.show_options = true;
            }
            else if (g_state.tasks.running)
            {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Options");
                nk_widget_disable_end(ctx);
            }

            if (nk_button_label_styled(ctx, &btn_style, "Credits"))
            {
                g_state.credits.show_window = true;
            }

            float content_height = (float)window_height - 110;

            if (!g_state.spine.show_window)
            {
                nk_layout_row_begin(ctx, NK_STATIC, 30, 2);
                nk_layout_row_push(ctx, 80);
                nk_label(ctx, "Search:", NK_TEXT_LEFT);
                nk_layout_row_push(ctx, 300);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, g_state.browser.search_buffer, sizeof(g_state.browser.search_buffer), nk_filter_default);
                g_state.browser.search_query = g_state.browser.search_buffer;
                nk_layout_row_end(ctx);
                bool showing_preview_panel = (g_state.preview.mode != PreviewMode::None || !g_state.preview.error.empty());

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
                    g_state.browser.visible_nodes.clear();

                    if (g_state.browser.data_pack && tree_scanned)
                    {
                        if (g_state.diff.show_tree && g_state.diff.root)
                        {
                            draw_diff_node(ctx, *g_state.diff.root);

                            if (scroll_to_selected && g_state.diff.selected_node)
                            {
                                auto it = std::find(g_state.diff.visible_nodes.begin(), g_state.diff.visible_nodes.end(), g_state.diff.selected_node);
                                if (it != g_state.diff.visible_nodes.end())
                                {
                                    int index = std::distance(g_state.diff.visible_nodes.begin(), it);
                                    nk_uint current_x, current_y;
                                    nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

                                    float row_height = 26.0f + ctx->style.window.spacing.y;
                                    float node_y = index * row_height;
                                    float view_h = nk_window_get_content_region(ctx).h;

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
                        else
                        {
                            draw_file_node(ctx, g_state.browser.data_pack->GetFileTree());

                            if (scroll_to_selected && g_state.browser.selected_node)
                            {
                                auto it = std::find(g_state.browser.visible_nodes.begin(), g_state.browser.visible_nodes.end(), g_state.browser.selected_node);
                                if (it != g_state.browser.visible_nodes.end())
                                {
                                    int index = std::distance(g_state.browser.visible_nodes.begin(), it);
                                    nk_uint current_x, current_y;
                                    nk_group_get_scroll(ctx, "FileTree", &current_x, &current_y);

                                    float row_height = 26.0f + ctx->style.window.spacing.y;
                                    float node_y = index * row_height;
                                    float view_h = nk_window_get_content_region(ctx).h;

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
                    }
                    else if (g_state.browser.data_pack && g_state.tasks.running)
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
                        nk_button_label_styled(ctx, &button_style, g_state.browser.data_pack->GetFileTree().name.c_str());

                        nk_layout_row_push(ctx, 200.0f);
                        std::string info = std::to_string(g_state.browser.data_pack->GetParsedFileCount()) + " items | " + format_size(g_state.browser.data_pack->GetParsedTotalSize());
                        nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));
                        nk_layout_row_end(ctx);
                    }
                    else if (g_state.browser.data_pack)
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

                        if (g_state.preview.mode == PreviewMode::Image)
                        {
                            nk_layout_row_dynamic(ctx, 30, 1);
                            if (g_state.preview.preview_node)
                            {
                                std::string title = "Preview: " + g_state.preview.preview_node->name;
                                nk_label(ctx, title.c_str(), NK_TEXT_CENTERED);
                            }

                            nk_layout_row_dynamic(ctx, 25, 1);
                            std::string dims = std::to_string(g_state.preview.width) + " x " + std::to_string(g_state.preview.height);
                            nk_label_colored(ctx, dims.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

                            if (g_state.preview.preview_node && std::holds_alternative<Core::FileInfo>(g_state.preview.preview_node->data))
                            {
                                const auto &info = std::get<Core::FileInfo>(g_state.preview.preview_node->data);
                                nk_layout_row_dynamic(ctx, 25, 1);
                                std::string size_str = "Size: " + format_size(info.size);
                                nk_label_colored(ctx, size_str.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));
                            }

                            if (g_state.preview.preview_node && std::holds_alternative<Core::FileInfo>(g_state.preview.preview_node->data))
                            {
                                const auto &info = std::get<Core::FileInfo>(g_state.preview.preview_node->data);
                                if (is_previewable_format(info.format))
                                {
                                    nk_layout_row_dynamic(ctx, 30, 1);
                                    if (nk_button_label(ctx, "Open in Window"))
                                    {
                                        open_image_preview_window(*g_state.preview.preview_node);
                                    }
                                }
                            }

                            float max_preview_width = right_width - 40.0f;
                            float max_preview_height = content_height - 180.0f;

                            float scale_w = max_preview_width / g_state.preview.width;
                            float scale_h = max_preview_height / g_state.preview.height;
                            float scale = (scale_w < scale_h) ? scale_w : scale_h;
                            if (scale > 1.0f)
                                scale = 1.0f;

                            float display_width = g_state.preview.width * scale;
                            float display_height = g_state.preview.height * scale;

                            nk_layout_row_begin(ctx, NK_STATIC, display_height, 1);
                            nk_layout_row_push(ctx, display_width);
                            struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                            struct nk_rect bounds = nk_widget_bounds(ctx);
                            struct nk_image img = nk_image_id((int)g_state.preview.texture);
                            nk_draw_image(canvas, bounds, &img, nk_rgb(255, 255, 255));
                            nk_layout_row_end(ctx);
                        }
                        else if (g_state.preview.mode == PreviewMode::DB)
                        {
                            if (g_state.preview.preview_node)
                            {
                                nk_layout_row_dynamic(ctx, 30, 1);
                                std::string title = "Database Preview: " + g_state.database.filename;
                                nk_label_colored(ctx, title.c_str(), NK_TEXT_CENTERED, nk_rgb(150, 200, 255));
                            }

                            nk_layout_row_dynamic(ctx, 25, 1);
                            std::string stats = std::to_string(g_state.database.rows.size()) + " rows x " + std::to_string(g_state.database.column_names.size()) + " columns";
                            nk_label_colored(ctx, stats.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

                            nk_layout_row_dynamic(ctx, 30, 1);
                            if (nk_button_label(ctx, "Export as JSON"))
                            {
                                export_db_as_json_file(*g_state.preview.preview_node);
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

                            if (nk_group_begin(ctx, (std::string("DBPreviewTable_") + g_state.database.filename).c_str(), NK_WINDOW_BORDER))
                            {
                                float base_width = 100.0f;
                                std::vector<float> col_widths(g_state.database.column_names.size(), base_width);
                                for (size_t j = 0; j < g_state.database.column_names.size(); j++)
                                {
                                    size_t max_len = g_state.database.column_names[j].length();
                                    for (const auto &row : g_state.database.rows)
                                    {
                                        if (j < row.size() && row[j].length() > max_len)
                                            max_len = row[j].length();
                                    }
                                    col_widths[j] = std::min(std::max(7.5f * max_len, 120.0f), 400.0f);
                                }

                                float index_col_width = 60.0f;

                                nk_layout_row_begin(ctx, NK_STATIC, 40, (int)g_state.database.column_names.size() + 1);

                                nk_layout_row_push(ctx, index_col_width);
                                struct nk_rect bounds = nk_widget_bounds(ctx);
                                nk_fill_rect(&ctx->current->buffer, bounds, 0, nk_rgb(60, 70, 90));
                                nk_label_colored(ctx, "#", NK_TEXT_CENTERED, nk_rgb(220, 230, 255));

                                for (size_t j = 0; j < g_state.database.column_names.size(); j++)
                                {
                                    nk_layout_row_push(ctx, col_widths[j]);
                                    struct nk_rect col_bounds = nk_widget_bounds(ctx);
                                    nk_fill_rect(&ctx->current->buffer, col_bounds, 0, nk_rgb(60, 70, 90));
                                    nk_label_colored(ctx, g_state.database.column_names[j].c_str(), NK_TEXT_CENTERED, nk_rgb(220, 230, 255));
                                }
                                nk_layout_row_end(ctx);

                                size_t preview_rows = g_state.database.rows.size();
                                int visible_index = 1;
                                int rows_shown = 0;
                                for (size_t i = 0; i < preview_rows; i++)
                                {
                                    if (rows_shown > 200)
                                        break; // rows limit for performance

                                    bool match = false;
                                    if (strlen(g_state.browser.search_buffer) == 0)
                                    {
                                        match = true;
                                    }
                                    else
                                    {
                                        std::string q = g_state.browser.search_buffer;
                                        for (const auto &cell : g_state.database.rows[i])
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
                                    nk_layout_row_begin(ctx, NK_STATIC, 38, (int)g_state.database.column_names.size() + 1);

                                    nk_layout_row_push(ctx, index_col_width);
                                    struct nk_rect index_bounds = nk_widget_bounds(ctx);
                                    nk_fill_rect(&ctx->current->buffer, index_bounds, 0, row_color);

                                    std::string row_index = std::to_string(visible_index++);
                                    nk_label_colored(ctx, row_index.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 200, 255));

                                    for (size_t j = 0; j < g_state.database.rows[i].size(); j++)
                                    {
                                        nk_layout_row_push(ctx, col_widths[j]);
                                        struct nk_rect cell_bounds = nk_widget_bounds(ctx);
                                        nk_fill_rect(&ctx->current->buffer, cell_bounds, 0, row_color);

                                        std::string cell_text = g_state.database.rows[i][j];
                                        int max_chars = (int)(col_widths[j] / 7);
                                        if (cell_text.length() > max_chars)
                                            cell_text = cell_text.substr(0, max_chars - 3) + "...";

                                        nk_label_colored(ctx, cell_text.c_str(), NK_TEXT_LEFT, nk_rgb(200, 200, 200));
                                    }
                                    nk_layout_row_end(ctx);
                                }

                                if (g_state.database.rows.size() > 200)
                                {
                                    nk_layout_row_dynamic(ctx, 20, 1);
                                    nk_label_colored(ctx, "... (preview limit reached)", NK_TEXT_LEFT, nk_rgb(255, 100, 100));
                                }

                                nk_group_end(ctx);
                            }
                        }
                        else if (g_state.preview.mode == PreviewMode::JSON)
                        {
                            nk_layout_row_dynamic(ctx, 25, 1);
                            nk_label(ctx, "JSON Viewer", NK_TEXT_CENTERED);

                            bool is_db_source = g_state.preview.preview_node && std::holds_alternative<Core::FileInfo>(g_state.preview.preview_node->data) && is_db_file(std::get<Core::FileInfo>(g_state.preview.preview_node->data).format);
                            bool is_scsp_source = g_state.preview.preview_node && std::holds_alternative<Core::FileInfo>(g_state.preview.preview_node->data) && is_scsp_file(std::get<Core::FileInfo>(g_state.preview.preview_node->data).format);
                            bool is_json_source = g_state.preview.preview_node && std::holds_alternative<Core::FileInfo>(g_state.preview.preview_node->data) && is_json_file(std::get<Core::FileInfo>(g_state.preview.preview_node->data).format);

                            if (is_scsp_source)
                            {
                                nk_layout_row_dynamic(ctx, 30, 1);
                                if (nk_button_label(ctx, "Export as JSON"))
                                {
                                    export_scsp_as_json_file(*g_state.preview.preview_node);
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
                                            export_db_as_json_file(*g_state.preview.preview_node);
                                        }
                                        else
                                        {
                                            export_json_file(*g_state.preview.preview_node);
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
                                    SDL_SetClipboardText(g_state.preview.json_preview.c_str());
                                }
                                nk_layout_row_push(ctx, 120);
                                if (nk_button_label(ctx, "Save As..."))
                                {
                                    try
                                    {
                                        std::string default_name = g_state.preview.preview_node ? g_state.preview.preview_node->name : "output";
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
                                                out << g_state.preview.json_preview;
                                                out.close();
                                                g_state.tasks.status = "Saved to: " + f.result();
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
                            std::string group_id = "JsonPreview";
                            if (g_state.preview.preview_node) group_id += "_" + g_state.preview.preview_node->name;
                            if (nk_group_begin(ctx, group_id.c_str(), NK_WINDOW_BORDER))
                            {
                                std::stringstream ss(g_state.preview.json_preview);
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
                        else if (g_state.preview.mode == PreviewMode::Text)
                        {
                            nk_layout_row_dynamic(ctx, 25, 1);
                            nk_label(ctx, "Text Viewer", NK_TEXT_CENTERED);

                            nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
                            nk_layout_row_push(ctx, 120);
                            if (nk_button_label(ctx, "Copy All"))
                            {
                                const std::string &data_to_copy = g_state.preview.atlas_full.empty() ? g_state.preview.atlas_preview : g_state.preview.atlas_full;
                                SDL_SetClipboardText(data_to_copy.c_str());
                            }
                            nk_layout_row_push(ctx, 120);
                            if (nk_button_label(ctx, "Save As..."))
                            {
                                try
                                {
                                    std::string default_name = g_state.preview.preview_node ? g_state.preview.preview_node->name : "output";
                                    size_t dot_pos = default_name.find_last_of('.');
                                    if (dot_pos != std::string::npos)
                                    {
                                        default_name = default_name.substr(0, dot_pos);
                                    }

                                    std::string data_to_save = g_state.preview.atlas_full.empty() ? g_state.preview.atlas_preview : g_state.preview.atlas_full;
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
                                            g_state.tasks.status = "Saved to: " + f.result();
                                        }
                                    }
                                }
                                catch (...)
                                {
                                }
                            }
                            nk_layout_row_end(ctx);

                            nk_layout_row_dynamic(ctx, content_height - 130, 1);
                            std::string group_id = "TextPreview";
                            if (g_state.preview.preview_node) group_id += "_" + g_state.preview.preview_node->name;
                            if (nk_group_begin(ctx, group_id.c_str(), NK_WINDOW_BORDER))
                            {
                                std::stringstream ss(g_state.preview.atlas_preview);
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
                        else if (!g_state.preview.error.empty())
                        {
                            nk_layout_row_dynamic(ctx, 30, 1);
                            nk_label_colored(ctx, "Error:", NK_TEXT_CENTERED, nk_rgb(255, 100, 100));
                            nk_layout_row_dynamic(ctx, 20, 1);
                            nk_label_colored(ctx, g_state.preview.error.c_str(), NK_TEXT_CENTERED, nk_rgb(255, 255, 255));
                        }

                        nk_group_end(ctx);
                    }
                }

                nk_layout_row_end(ctx);
            }
            else
            {
                // ====== SPINE VIEWER INLINE ======
                // Update animation
                if (g_state.spine.viewer && g_state.spine.viewer->isLoaded())
                {
                    float dt = 0;
                    if (g_state.spine.playing)
                    {
                        Uint64 now = SDL_GetPerformanceCounter();
                        if (g_state.spine.last_tick > 0)
                        {
                            dt = (float)(now - g_state.spine.last_tick) / (float)SDL_GetPerformanceFrequency();
                            dt *= g_state.spine.speed;
                        }
                        g_state.spine.last_tick = now;
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

                    // Search bar
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

                    // Layout: list | viewport | editor
                    float sw = (float)window_width;
                    float sh = content_height - 30.0f;
                    float iListW = sw * 0.22f;
                    float iEditorW = g_state.spine.edit_mode ? sw * 0.30f : 0;
                    float iViewerW = sw - iListW - iEditorW - 50.0f;

                    nk_layout_row_begin(ctx, NK_STATIC, sh, g_state.spine.edit_mode ? 3 : 2);

                    // Skeleton list
                    g_state.spine.visible_indices.clear();
                    nk_layout_row_push(ctx, iListW);
                    if (nk_group_begin(ctx, "SpineListInline", NK_WINDOW_BORDER))
                    {
                        draw_spine_category(ctx, root_cat, spine_entries_inline, 0);
                        nk_group_end(ctx);
                    }

                    // Viewer panel
                    nk_layout_row_push(ctx, iViewerW);
                    if (nk_group_begin(ctx, "SpineViewInline", NK_WINDOW_BORDER))
                    {
                        if (g_state.spine.viewer && g_state.spine.viewer->isLoaded())
                        {
                            // === Controls row 1: Anim, Skin, Play/Pause ===
                            nk_layout_row_begin(ctx, NK_STATIC, 28, 8);

                            nk_layout_row_push(ctx, 50);
                            nk_label(ctx, "Anim:", NK_TEXT_LEFT);
                            nk_layout_row_push(ctx, 160);
                            auto anim_names = g_state.spine.viewer->getAnimationNames();
                            if (!anim_names.empty())
                            {
                                g_state.spine.selected_animation = g_state.spine.viewer->getCurrentAnimIndex();
                                if (g_state.spine.selected_animation >= (int)anim_names.size())
                                    g_state.spine.selected_animation = 0;
                                if (nk_combo_begin_label(ctx, anim_names[g_state.spine.selected_animation].c_str(), nk_vec2(200, 300)))
                                {
                                    nk_layout_row_dynamic(ctx, 22, 1);
                                    for (int a = 0; a < (int)anim_names.size(); a++)
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
                            auto skin_names = g_state.spine.viewer->getSkinNames();
                            if (!skin_names.empty())
                            {
                                if (g_state.spine.selected_skin >= (int)skin_names.size())
                                    g_state.spine.selected_skin = 0;
                                if (nk_combo_begin_label(ctx, skin_names[g_state.spine.selected_skin].c_str(), nk_vec2(160, 300)))
                                {
                                    nk_layout_row_dynamic(ctx, 22, 1);
                                    for (int s = 0; s < (int)skin_names.size(); s++)
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

                            // === Controls row 2: Speed, Zoom, Flip, Edit, Reset, Export ===
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
                                struct nk_style_button flip_style = ctx->style.button;
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
                                struct nk_style_button flip_style = ctx->style.button;
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
                                struct nk_style_button edit_style = ctx->style.button;
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
                            if (g_state.spine.selected_index >= 0 && g_state.spine.selected_index < (int)spine_entries_inline.size())
                            {
                                if (nk_button_label(ctx, "Export All"))
                                {
                                    const auto &entry = spine_entries_inline[g_state.spine.selected_index];
                                    try
                                    {
                                        auto d = pfd::select_folder("Select destination folder", ".");
                                        if (!d.result().empty())
                                        {
                                            std::string dest = d.result();
                                            int exported = 0;
                                            {
                                                std::vector<uint8_t> data = g_state.browser.data_pack->GetFileData(*entry.scsp_node);
                                                std::string json_str = SCSPParser::ConvertSCSPToJson(data);
                                                if (!json_str.empty())
                                                {
                                                    try
                                                    {
                                                        json parsed = json::parse(json_str);
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
                                                std::string out_name = img->name;
                                                std::string el = fi.format;
                                                std::transform(el.begin(), el.end(), el.begin(), ::tolower);
                                                if (el == ".sct" || el == ".sct2")
                                                {
                                                    std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(data, false);
                                                    if (!png_data.empty())
                                                    {
                                                        size_t dp = out_name.find_last_of('.');
                                                        if (dp != std::string::npos)
                                                            out_name = out_name.substr(0, dp);
                                                        out_name += ".png";
                                                        std::ofstream out(dest + "/" + out_name, std::ios::binary);
                                                        out.write((const char *)png_data.data(), png_data.size());
                                                        exported++;
                                                    }
                                                }
                                                else
                                                {
                                                    std::ofstream out(dest + "/" + out_name, std::ios::binary);
                                                    out.write((const char *)data.data(), data.size());
                                                    exported++;
                                                }
                                            }
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
                                if (nk_button_label_styled(ctx, &ab, spine_autoplay ? "Auto: ON" : "Auto: OFF"))
                                {
                                    spine_autoplay = !spine_autoplay;
                                    g_state.spine.viewer->setAutoplayNext(spine_autoplay);
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
                                struct nk_style_button pb = ctx->style.button;
                                pb.rounding = 3.0f;
                                pb.normal = nk_style_item_color(spine_pma_blend ? nk_rgb(70, 90, 120) : nk_rgb(60, 60, 65));
                                pb.hover = nk_style_item_color(nk_rgb(80, 100, 130));
                                pb.text_normal = nk_rgb(200, 200, 200);
                                if (nk_button_label_styled(ctx, &pb, spine_pma_blend ? "PMA: ON" : "PMA: OFF"))
                                {
                                    spine_pma_blend = !spine_pma_blend;
                                    // Source textures from ASTC are already premultiplied —
                                    // never re-premultiply, just toggle the blend mode
                                    g_state.spine.viewer->setUsePMA(spine_pma_blend);
                                    g_state.spine.viewer->setPremultiplyTextures(false);
                                    if (g_state.spine.selected_index >= 0 && g_state.spine.selected_index < (int)spine_entries_inline.size())
                                    {
                                        g_state.spine.viewer->loadSkeleton(g_state.spine.dictionary, *g_state.browser.data_pack, spine_entries_inline[g_state.spine.selected_index]);
                                    }
                                }
                            }

                            nk_layout_row_push(ctx, 10);
                            nk_spacing(ctx, 1);

                            // Viewport background preset
                            nk_layout_row_push(ctx, 80);
                            {
                                const char *bg_labels[] = {"BG: None", "BG: Dark", "BG: Gray", "BG: White"};
                                const float bg_colors[][3] = {
                                    {0, 0, 0}, {0.12f, 0.12f, 0.14f}, {0.35f, 0.35f, 0.38f}, {1.0f, 1.0f, 1.0f}};
                                struct nk_style_button bb = ctx->style.button;
                                bb.rounding = 3.0f;
                                bb.normal = nk_style_item_color(nk_rgb(60, 60, 65));
                                bb.hover = nk_style_item_color(nk_rgb(75, 75, 80));
                                bb.text_normal = nk_rgb(200, 200, 200);
                                if (nk_button_label_styled(ctx, &bb, bg_labels[spine_bg_preset]))
                                {
                                    spine_bg_preset = (spine_bg_preset + 1) % 4;
                                    g_state.spine.viewer->setBgColor(
                                        bg_colors[spine_bg_preset][0],
                                        bg_colors[spine_bg_preset][1],
                                        bg_colors[spine_bg_preset][2]);
                                }
                            }

                            nk_layout_row_end(ctx);

                            // === Viewport ===
                            float vpH = sh - 120.0f;
                            if (vpH < 100)
                                vpH = 100;
                            nk_layout_row_dynamic(ctx, vpH, 1);
                            struct nk_rect vb = nk_widget_bounds(ctx);
                            int vw = (int)vb.w, vh = (int)vb.h;

                            // Mouse interaction (skip if a combo/popup is active)
                            {
                                nk_input *inp = &ctx->input;
                                bool popup_active = (ctx->current && ctx->current->popup.win);
                                if (!popup_active && nk_input_is_mouse_hovering_rect(inp, vb))
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
                                            float s = g_state.spine.viewer->getZoom() > 0 ? (float)vw / g_state.spine.viewer->getZoom() / vw : 1;
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
                                        static SpineViewer::GizmoHandle ag = SpineViewer::GizmoHandle::None;
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
                                            float s = g_state.spine.viewer->getZoom() > 0 ? (float)vw / g_state.spine.viewer->getZoom() / vw : 1;
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
                                    }
                                }
                            }

                            if (vw > 0 && vh > 0)
                            {
                                g_state.spine.viewer->render(vw, vh);
                                GLuint ft = g_state.spine.viewer->getFBOTexture();
                                if (ft)
                                {
                                    struct nk_image fimg = nk_image_id((int)ft);
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

                    // Bone editor panel (third column, only when editing)
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

                            static bool spine_export_pending = false;
                            if (nk_button_label(ctx, "Export Modified"))
                            {
                                spine_export_pending = true;
                            }

                            auto texList = g_state.spine.viewer->getTextureList();
                            if (texList.empty())
                            {
                                nk_spacing(ctx, 1);
                            }

                            if (spine_export_pending)
                            {
                                spine_export_pending = false;
                                try
                                {
                                    auto d = pfd::select_folder("Select destination folder", ".");
                                    if (!d.result().empty() && g_state.spine.selected_index >= 0)
                                    {
                                        std::string dest = d.result();
                                        const auto &entry = spine_entries_inline[g_state.spine.selected_index];
                                        int exported = 0;
                                        std::string modJson = g_state.spine.viewer->getModifiedSkeletonJson();
                                        if (!modJson.empty())
                                        {
                                            std::ofstream out(dest + "/" + entry.display_name + "_modified.json");
                                            out << modJson;
                                            exported++;
                                        }
                                        if (entry.atlas_node)
                                        {
                                            std::vector<uint8_t> ad = g_state.browser.data_pack->GetFileData(*entry.atlas_node);
                                            std::string as(ad.begin(), ad.end());
                                            size_t p = 0;
                                            while ((p = as.find(".sct", p)) != std::string::npos)
                                            {
                                                as.replace(p, 4, ".png");
                                                p += 4;
                                            }
                                            std::ofstream out(dest + "/" + entry.atlas_node->name, std::ios::binary);
                                            out << as;
                                            exported++;
                                        }
                                        for (const auto *img : entry.image_nodes)
                                        {
                                            const auto &fi = std::get<Core::FileInfo>(img->data);
                                            std::vector<uint8_t> fd = g_state.browser.data_pack->GetFileData(*img);
                                            std::string on = img->name;
                                            std::string el = fi.format;
                                            std::transform(el.begin(), el.end(), el.begin(), ::tolower);
                                            if (el == ".sct" || el == ".sct2")
                                            {
                                                auto png = SCTParser::ConvertToPNG(fd, false);
                                                if (!png.empty())
                                                {
                                                    size_t dp = on.find_last_of('.');
                                                    if (dp != std::string::npos)
                                                        on = on.substr(0, dp);
                                                    on += ".png";
                                                    std::ofstream out(dest + "/" + on, std::ios::binary);
                                                    out.write((const char *)png.data(), png.size());
                                                    exported++;
                                                }
                                            }
                                            else
                                            {
                                                std::ofstream out(dest + "/" + on, std::ios::binary);
                                                out.write((const char *)fd.data(), fd.size());
                                                exported++;
                                            }
                                        }
                                        g_state.tasks.status = "Exported " + std::to_string(exported) + " modified files";
                                    }
                                }
                                catch (...)
                                {
                                }
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
                            if (nk_group_begin(ctx, "BoneList", NK_WINDOW_BORDER))
                            {
                                auto bones = g_state.spine.viewer->getBoneList();
                                float scroll_target_y = -1;
                                static bool spine_bone_just_reset = false;

                                // Build parent->has_children lookup
                                std::unordered_set<std::string> has_children;
                                for (auto &b : bones)
                                {
                                    if (!b.parentName.empty())
                                        has_children.insert(b.parentName);
                                }

                                for (size_t bi_idx = 0; bi_idx < bones.size(); bi_idx++)
                                {
                                    auto &bi = bones[bi_idx];

                                    // Filter by search
                                    if (!bone_query.empty())
                                    {
                                        std::string lower_name = bi.name;
                                        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                                        if (lower_name.find(bone_query) == std::string::npos)
                                            continue;
                                    }

                                    // Skip if an ancestor is collapsed in the list
                                    if (bone_query.empty())
                                    {
                                        bool ancestor_collapsed = false;
                                        // Walk up parents using the bone list data
                                        std::string check = bi.parentName;
                                        int safety = 0;
                                        while (!check.empty() && safety++ < 20)
                                        {
                                            if (g_state.spine.collapsed_bones.count(check))
                                            {
                                                ancestor_collapsed = true;
                                                break;
                                            }
                                            // Find parent's parent
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

                                    // Bone row: indent spacer + name + H + R
                                    float indent_px = bi.depth * 10.0f;
                                    if (indent_px > 0)
                                    {
                                        int cols = 4;
                                        nk_layout_row_begin(ctx, NK_STATIC, 16, cols);
                                        nk_layout_row_push(ctx, indent_px);
                                        // Draw tree line
                                        struct nk_rect sp_bounds = nk_widget_bounds(ctx);
                                        struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
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

                                    // Grab bounds for auto-scroll
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
                                            // Second click on same parent toggles collapse
                                            if (is_collapsed)
                                                g_state.spine.collapsed_bones.erase(bi.name);
                                            else
                                                g_state.spine.collapsed_bones.insert(bi.name);
                                        }
                                        g_state.spine.selected_bone = bi.name;
                                        g_state.spine.viewer->selectedBoneIndex = (int)bi_idx;
                                        spine_bone_just_reset = false;
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
                                            spine_bone_just_reset = true;
                                        }
                                    }
                                    nk_layout_row_end(ctx);

                                    if (is_sel)
                                    {
                                        // Auto-create override if not editing (skip if just reset)
                                        if (!bi.hasOverride && !spine_bone_just_reset)
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
                                            spine_bone_just_reset = false;

                                        const char *labels[] = {"X", "Y", "Rot", "SclX", "SclY", "ShrX", "ShrY"};
                                        float anim[7] = {bi.animX, bi.animY, bi.animRot, bi.animSX, bi.animSY, bi.animShX, bi.animShY};
                                        float setup[7] = {bi.setupX, bi.setupY, bi.setupRot, bi.setupSX, bi.setupSY, bi.setupShX, bi.setupShY};

                                        static bool spine_link_scale = true;
                                        nk_layout_row_dynamic(ctx, 16, 2);
                                        nk_label_colored(ctx, "Editing:", NK_TEXT_LEFT, nk_rgb(255, 200, 80));
                                        {
                                            struct nk_style_button lb = ctx->style.button;
                                            lb.rounding = 1.0f;
                                            lb.padding = nk_vec2(2, 0);
                                            lb.border = 0;
                                            lb.normal = nk_style_item_color(spine_link_scale ? nk_rgb(56, 120, 74) : nk_rgb(60, 60, 65));
                                            lb.hover = nk_style_item_color(spine_link_scale ? nk_rgb(66, 138, 86) : nk_rgb(75, 75, 80));
                                            lb.text_normal = nk_rgb(200, 200, 200);
                                            if (nk_button_label_styled(ctx, &lb, spine_link_scale ? "Scale: Linked" : "Scale: Free"))
                                            {
                                                spine_link_scale = !spine_link_scale;
                                            }
                                        }

                                        float vals[7] = {bi.x, bi.y, bi.rotation, bi.scaleX, bi.scaleY, bi.shearX, bi.shearY};
                                        float oldSclX = vals[3], oldSclY = vals[4];
                                        float steps[7] = {1.0f, 1.0f, 1.0f, 0.05f, 0.05f, 0.5f, 0.5f};
                                        float pxStep[7] = {0.5f, 0.5f, 0.5f, 0.01f, 0.01f, 0.1f, 0.1f};
                                        for (int f = 0; f < 7; f++)
                                        {
                                            float range = fmaxf(fmaxf(fabsf(vals[f]), fabsf(setup[f])) * 3.0f, 10.0f);
                                            nk_layout_row_dynamic(ctx, 18, 1);
                                            vals[f] = nk_propertyf(ctx, labels[f], -range, vals[f], range, steps[f], pxStep[f]);
                                        }

                                        if (spine_link_scale)
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

                                        // Animated values
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

                                        // Setup pose
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

                                // Auto-scroll
                                if (g_state.spine.scroll_to_bone && scroll_target_y >= 0)
                                {
                                    nk_uint scx, scy;
                                    nk_group_get_scroll(ctx, "BoneList", &scx, &scy);
                                    struct nk_rect content = nk_window_get_content_region(ctx);
                                    float rel_y = scroll_target_y - content.y + (float)scy;
                                    float new_scroll = rel_y - content.h * 0.3f;
                                    if (new_scroll < 0)
                                        new_scroll = 0;
                                    nk_group_set_scroll(ctx, "BoneList", scx, (nk_uint)new_scroll);
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
            } // end else (show_spine_viewer)

            nk_layout_row_dynamic(ctx, 4, 1);
            nk_spacing(ctx, 1);

            if (g_state.tasks.running)
            {
                nk_layout_row_begin(ctx, NK_STATIC, 22, 2);
                nk_layout_row_push(ctx, 220);
                nk_size prog_val = static_cast<nk_size>(g_state.tasks.progress.load() * 1000.0f);
                nk_progress(ctx, &prog_val, 1000, NK_FIXED);
                nk_layout_row_push(ctx, (float)window_width - 260);
                int percent = static_cast<int>(g_state.tasks.progress.load() * 100.0f);
                std::string status_text = g_state.tasks.status + " (" + std::to_string(percent) + "%)";
                nk_label_colored(ctx, status_text.c_str(), NK_TEXT_LEFT, nk_rgb(100, 200, 255));
                nk_layout_row_end(ctx);
            }
            else
            {
                nk_layout_row_dynamic(ctx, 22, 1);
                if (selection_exists && g_state.browser.selected_node && std::holds_alternative<Core::FileInfo>(g_state.browser.selected_node->data))
                {
                    const auto &info = std::get<Core::FileInfo>(g_state.browser.selected_node->data);
                    char off_buf[32];
                    snprintf(off_buf, sizeof(off_buf), "0x%llX", (unsigned long long)info.offset);
                    std::string details = "Selected: " + g_state.browser.selected_node->name +
                                         " | Size: " + std::to_string(info.size) + " B" +
                                         " | Offset: " + off_buf +
                                         " | Format: " + info.format;
                    nk_label_colored(ctx, details.c_str(), NK_TEXT_LEFT, nk_rgb(180, 180, 180));
                }
                else
                {
                    nk_label_colored(ctx, g_state.tasks.status.c_str(), NK_TEXT_LEFT, nk_rgb(160, 160, 160));
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

    if (g_state.preview.texture)
        glDeleteTextures(1, &g_state.preview.texture);
    if (g_state.sct.texture)
        glDeleteTextures(1, &g_state.sct.texture);
    nk_sdl_shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(win);
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
    }
    IMG_Quit();
    SDL_Quit();
    return 0;
}