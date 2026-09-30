#include "gui/PreviewPanel.h"
#include "gui/IPreviewLoader.h"
#include "gui/UIHelpers.h"
#include "core/Core.h"
#include "core/FileTree.h"
#include "parsers/SCTParser.h"
#include "parsers/DBParser.h"
#include "parsers/SCSPParser.h"
#include "core/DialogPaths.h"

#include <GL/glew.h>
#include <SDL_image.h>
#include "json.hpp"
#include "nuklear.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
    void load_json_preview(const Core::FileNode &node, const std::string &content = "")
    {
        try
        {
            g_state.preview.text_full = "";
            std::string json_content = content;

            if (json_content.empty())
            {
                if (std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node); !file_data.empty())
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
                const nlohmann::json parsed = nlohmann::json::parse(json_content);
                g_state.preview.text_full = parsed.dump(2);
            }
            catch (...)
            {
                g_state.preview.text_full = json_content;
            }
            g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
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
            g_state.preview.text_full = "";
            g_state.preview.mode = PreviewMode::None;

            const std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            if (file_data.empty())
            {
                g_state.preview.error = "Failed to read DB file";
                return;
            }

            std::string json_str = DBParser::ConvertToJson(file_data);
            if (json_str.empty() || json_str == "{}")
            {
                g_state.preview.text_full = json_str;
                g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
                return;
            }

            try
            {
                g_state.database.json_data = nlohmann::json::parse(json_str);
            }
            catch (const nlohmann::json::parse_error &e)
            {
                g_state.preview.text_full = json_str;
                g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
                return;
            }

            g_state.database.filename = node.name;

            if (!g_state.database.json_data.is_array() || g_state.database.json_data.empty())
            {
                g_state.preview.text_full = g_state.database.json_data.dump(2);
                g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
                return;
            }

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
                        for (const auto &col_name : g_state.database.column_names)
                        {
                            if (row.contains(col_name))
                            {
                                if (row[col_name].is_string())
                                {
                                    row_data.push_back(row[col_name].get<std::string>());
                                }
                                else if (row[col_name].is_number())
                                {
                                    row_data.push_back(row[col_name].dump());
                                }
                                else if (row[col_name].is_boolean())
                                {
                                    row_data.emplace_back(row[col_name].get<bool>() ? "true" : "false");
                                }
                                else if (row[col_name].is_null())
                                {
                                    row_data.emplace_back("NULL");
                                }
                                else
                                {
                                    row_data.push_back(row[col_name].dump());
                                }
                            }
                            else
                            {
                                row_data.emplace_back("");
                            }
                        }
                        g_state.database.rows.push_back(row_data);
                    }
                }
                g_state.preview.mode = PreviewMode::DB;
            }
            else
            {
                g_state.preview.text_full = g_state.database.json_data.dump(2);
                g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
            }
        }
        catch (const std::exception &e)
        {
            g_state.preview.error = "DB parsing error: " + std::string(e.what());
            g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
        }
    }

    void load_scsp_preview(const Core::FileNode &node)
    {
        try
        {
            g_state.preview.text_full = "";
            g_state.preview.mode = PreviewMode::None;

            const std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            if (file_data.empty())
            {
                g_state.preview.error = "Failed to read SCSP file";
                return;
            }

            if (std::string json_str = SCSPParser::ConvertToJson(file_data); !json_str.empty())
            {
                try
                {
                    const nlohmann::json parsed = nlohmann::json::parse(json_str);
                    g_state.preview.text_full = parsed.dump(2);
                }
                catch (...)
                {
                    g_state.preview.text_full = json_str;
                }
                g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = true;
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
                g_state.preview.text_preview = "Failed to read file";
                g_state.preview.text_full = "";
                g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = false;
                return;
            }
            g_state.preview.text_full = std::string(file_data.begin(), file_data.end());
            g_state.preview.text_preview = g_state.preview.text_full;
            if (g_state.preview.text_preview.length() > 20000)
            {
                g_state.preview.text_preview = g_state.preview.text_preview.substr(0, 20000) + "\n\n... (truncated)";
            }
            g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = false;
        }
        catch (const std::exception &e)
        {
            g_state.preview.text_preview = "Error loading text: " + std::string(e.what());
            g_state.preview.text_full = "";
            g_state.preview.mode = PreviewMode::Text; g_state.preview.is_json = false;
        }
    }

    SDL_Surface *load_surface_from_node(const Core::FileNode &node, std::string &out_error)
    {
        if (!std::holds_alternative<Core::FileInfo>(node.data))
        {
            out_error = "Not a file";
            return nullptr;
        }
        const auto &info = std::get<Core::FileInfo>(node.data);

        if (!g_state.browser.data_pack)
        {
            out_error = "No pack opened";
            return nullptr;
        }

        std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
        if (file_data.empty())
        {
            out_error = "Failed to read file data";
            return nullptr;
        }

        if (is_sct_format(info.format))
        {
            try
            {
                std::vector<uint8_t> png_data = SCTParser::ConvertToPNG(file_data);
                if (png_data.empty())
                {
                    out_error = "Failed to convert SCT/SCT2 file";
                    return nullptr;
                }
                SDL_RWops *rw = SDL_RWFromMem(png_data.data(), static_cast<int>(png_data.size()));
                if (!rw)
                {
                    out_error = "Failed to create memory stream for SCT";
                    return nullptr;
                }
                SDL_Surface *surface = IMG_Load_RW(rw, 1);
                if (!surface)
                {
                    out_error = "Failed to decode converted SCT image: " + std::string(IMG_GetError());
                    return nullptr;
                }
                return surface;
            }
            catch (const std::exception &e)
            {
                out_error = "SCT parsing error: " + std::string(e.what());
                return nullptr;
            }
        }
        else
        {
            SDL_RWops *rw = SDL_RWFromMem(file_data.data(), static_cast<int>(file_data.size()));
            if (!rw)
            {
                out_error = "Failed to create memory stream";
                return nullptr;
            }
            SDL_Surface *surface = IMG_Load_RW(rw, 1);
            if (!surface)
            {
                out_error = "Failed to decode image: " + std::string(IMG_GetError());
                return nullptr;
            }
            return surface;
        }
    }

    template <typename ConverterFunc>
    void export_json_file(const Core::FileNode &node, const std::string &dialog_title, const std::string &format_name, ConverterFunc converter)
    {
        try
        {
            const std::string default_name = Core::ReplaceExtension(node.name, ".json");

            if (const std::string save_path = DialogPaths::SaveFile(dialog_title, default_name, {"JSON Files", "*.json", "All Files", "*.*"}); !save_path.empty())
            {
                std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);

                if (std::string json_str = converter(file_data); !json_str.empty() && json_str != "{}")
                {
                    try
                    {
                        nlohmann::json parsed = nlohmann::json::parse(json_str);
                        std::string formatted_json = parsed.dump(2);

                        std::ofstream out(save_path);
                        out << formatted_json;
                        out.close();
                        g_state.tasks.status = "Exported " + format_name + " to JSON: " + save_path;
                    }
                    catch (const nlohmann::json::parse_error &e)
                    {
                        std::ofstream out(save_path);
                        out << json_str;
                        out.close();
                        g_state.tasks.status = "Exported " + format_name + " to JSON (unformatted): " + save_path;
                    }
                }
                else
                {
                    g_state.tasks.status = "Failed to convert " + format_name + " to JSON";
                }
            }
        }
        catch (const std::exception &e)
        {
            g_state.tasks.status = "Export error: " + std::string(e.what());
        }
    }
    class DBPreviewLoader : public IPreviewLoader {
    public:
        [[nodiscard]] bool CanLoad(const std::string& format) const override { return is_db_file(format); }
        void Load(const Core::FileNode& node) override { load_db_preview(node); }
    };

    class SCSPPreviewLoader : public IPreviewLoader {
    public:
        [[nodiscard]] bool CanLoad(const std::string& format) const override { return is_scsp_file(format); }
        void Load(const Core::FileNode& node) override { load_scsp_preview(node); }
    };

    class JSONPreviewLoader : public IPreviewLoader {
    public:
        [[nodiscard]] bool CanLoad(const std::string& format) const override { return is_json_file(format); }
        void Load(const Core::FileNode& node) override { load_json_preview(node); }
    };

    class TextPreviewLoader : public IPreviewLoader {
    public:
        [[nodiscard]] bool CanLoad(const std::string& format) const override { return is_text_file(format); }
        void Load(const Core::FileNode& node) override { load_text_preview(node); }
    };

    class AnimatedWebPPreviewLoader : public IPreviewLoader {
    public:
        [[nodiscard]] bool CanLoad(const std::string& format) const override { return is_animated_webp(format); }
        void Load(const Core::FileNode& node) override {
            g_state.preview.error = "Animated WebP preview not supported. Use 'Export' to save the file.";
            g_state.preview.mode = PreviewMode::None;
        }
    };

    class ImagePreviewLoader : public IPreviewLoader {
    public:
        [[nodiscard]] bool CanLoad(const std::string& format) const override { return is_previewable_format(format); }
        void Load(const Core::FileNode& node) override {
            std::string err;
            SDL_Surface *surface = load_surface_from_node(node, err);
            if (!surface)
            {
                g_state.preview.error = err;
                g_state.preview.mode = PreviewMode::None;
                return;
            }

            SDL_Surface *rgba_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(surface);

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
            
            g_state.preview.mode = PreviewMode::Image;
        }
    };

    const std::vector<std::unique_ptr<IPreviewLoader>>& get_preview_loaders() {
        static std::vector<std::unique_ptr<IPreviewLoader>> loaders;
        if (loaders.empty()) {
            loaders.push_back(std::make_unique<DBPreviewLoader>());
            loaders.push_back(std::make_unique<SCSPPreviewLoader>());
            loaders.push_back(std::make_unique<JSONPreviewLoader>());
            loaders.push_back(std::make_unique<TextPreviewLoader>());
            loaders.push_back(std::make_unique<AnimatedWebPPreviewLoader>());
            loaders.push_back(std::make_unique<ImagePreviewLoader>());
        }
        return loaders;
    }
}

void clear_preview()
{
    if (g_state.preview.texture != 0)
    {
        glDeleteTextures(1, &g_state.preview.texture);
        g_state.preview.texture = 0;
    }
    
    g_state.preview.width = 0;
    g_state.preview.height = 0;
    g_state.preview.error = "";
    g_state.preview.text_preview = "";
    g_state.preview.text_full = "";
    g_state.preview.text_full = "";
    g_state.database.column_names.clear();
    g_state.database.rows.clear();
    g_state.preview.mode = PreviewMode::None;
    g_state.preview.preview_node = nullptr;
}

void load_preview(const Core::FileNode &node)
{
    clear_preview();
    g_state.preview.preview_node = &node;

    try
    {
        if (!std::holds_alternative<Core::FileInfo>(node.data))
        {
            g_state.preview.error = "Not a file";
            return;
        }
        const auto &info = std::get<Core::FileInfo>(node.data);

        for (const auto& loader : get_preview_loaders())
        {
            if (loader->CanLoad(info.format))
            {
                loader->Load(node);
                return;
            }
        }

        g_state.preview.error = "Preview not available for " + info.format + " files";
    }
    catch (const std::exception &e)
    {
        g_state.preview.error = "Error: " + std::string(e.what());
        
        g_state.preview.mode = PreviewMode::None;
    }
    catch (...)
    {
        g_state.preview.error = "Unknown error occurred";
        
        g_state.preview.mode = PreviewMode::None;
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

        std::string err;
        SDL_Surface *raw_surface = load_surface_from_node(node, err);
        if (!raw_surface)
        {
            g_state.tasks.status = err;
            return;
        }
        const std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(raw_surface, SDL_FreeSurface);

        const int original_width = surface->w;
        const int original_height = surface->h;

        SDL_DisplayMode display_mode;
        SDL_GetCurrentDisplayMode(0, &display_mode);
        const int screen_width = display_mode.w;
        const int screen_height = display_mode.h;

        const int max_width = static_cast<int>(screen_width * 0.9f);
        const int max_height = static_cast<int>(screen_height * 0.9f);

        g_state.image.width = original_width;
        g_state.image.height = original_height;

        if (g_state.image.width > max_width || g_state.image.height > max_height)
        {
            const float scale_w = static_cast<float>(max_width) / original_width;
            const float scale_h = static_cast<float>(max_height) / original_height;
            const float scale = (scale_w < scale_h) ? scale_w : scale_h;

            g_state.image.width = static_cast<int>(original_width * scale);
            g_state.image.height = static_cast<int>(original_height * scale);
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
            g_state.tasks.status = "Failed to create preview window";
            return;
        }

        g_state.image.renderer = SDL_CreateRenderer(g_state.image.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!g_state.image.renderer)
        {
            SDL_DestroyWindow(g_state.image.window);
            g_state.image.window = nullptr;
            g_state.tasks.status = "Failed to create renderer";
            return;
        }

        g_state.image.texture = SDL_CreateTextureFromSurface(g_state.image.renderer, surface.get());

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
        const std::string default_name = Core::ReplaceExtension(node.name, ".png");

        if (const std::string save_path = DialogPaths::SaveFile("Export as PNG", default_name, {"PNG Files", "*.png", "All Files", "*.*"}); !save_path.empty())
        {
            const std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            std::vector<uint8_t> png_data;

            if (is_sct_format(info.format))
            {
                png_data = SCTParser::ConvertToPNG(file_data);
            }
            else
            {
                png_data = file_data;
            }

            if (!png_data.empty())
            {
                std::ofstream out(save_path, std::ios::binary);
                out.write(reinterpret_cast<const char *>(png_data.data()), png_data.size());
                out.close();
                g_state.tasks.status = "Exported to: " + save_path;
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
        if (const std::string save_path = DialogPaths::SaveFile("Export as SCT", node.name, {"SCT Files", "*.sct;*.sct2", "All Files", "*.*"}); !save_path.empty())
        {
            const std::vector<uint8_t> file_data = g_state.browser.data_pack->GetFileData(node);
            std::ofstream out(save_path, std::ios::binary);
            out.write(reinterpret_cast<const char *>(file_data.data()), file_data.size());
            out.close();
            g_state.tasks.status = "Exported to: " + save_path;
        }
    }
    catch (const std::exception &e)
    {
        g_state.tasks.status = "Export error: " + std::string(e.what());
    }
}

void export_db_as_json_file(const Core::FileNode &node)
{
    export_json_file(node, "Export DB as JSON", "DB", DBParser::ConvertToJson);
}

void export_scsp_as_json_file(const Core::FileNode &node)
{
    export_json_file(node, "Export SCSP as JSON", "SCSP", SCSPParser::ConvertToJson);
}

void draw_preview_panel(nk_context *ctx, float right_width, float content_height)
{
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
                std::string size_str = "Size: " + Core::FormatSize(info.size);
                nk_label_colored(ctx, size_str.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));
            }

            if (g_state.preview.preview_node && std::holds_alternative<Core::FileInfo>(g_state.preview.preview_node->data))
            {
                if (const auto &info = std::get<Core::FileInfo>(g_state.preview.preview_node->data); is_previewable_format(info.format))
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
            nk_command_buffer *canvas = nk_window_get_canvas(ctx);
            struct nk_rect bounds = nk_widget_bounds(ctx);
            struct nk_image img = nk_image_id(static_cast<int>(g_state.preview.texture));
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

            nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
            nk_layout_row_push(ctx, 80);
            nk_label(ctx, "Search:", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, right_width - 100);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, g_state.database.search_buffer, sizeof(g_state.database.search_buffer), nk_filter_default);
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
                    col_widths[j] = (std::min)((std::max)(7.5f * max_len, 120.0f), 400.0f);
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
                        break;

                    bool match = false;
                    if (strlen(g_state.database.search_buffer) == 0)
                    {
                        match = true;
                    }
                    else
                    {
                        std::string q = g_state.database.search_buffer;
                        for (const auto &cell : g_state.database.rows[i])
                        {
                            if (cell.find(q) != std::string::npos)
                            {
                                match = true;
                                break;
                            }
                        }
                    }

                    if (match)
                    {
                        nk_layout_row_begin(ctx, NK_STATIC, 30, (int)g_state.database.column_names.size() + 1);

                        nk_layout_row_push(ctx, index_col_width);
                        std::string idx_str = std::to_string(visible_index++);
                        nk_label_colored(ctx, idx_str.c_str(), NK_TEXT_CENTERED, nk_rgb(180, 180, 180));

                        for (size_t j = 0; j < g_state.database.column_names.size(); j++)
                        {
                            nk_layout_row_push(ctx, col_widths[j]);
                            std::string val = (j < g_state.database.rows[i].size()) ? g_state.database.rows[i][j] : "";
                            nk_label_colored(ctx, val.c_str(), NK_TEXT_LEFT, nk_rgb(240, 240, 240));
                        }
                        nk_layout_row_end(ctx);
                        rows_shown++;
                    }
                }
                nk_group_end(ctx);
            }
        }
        else if (g_state.preview.mode == PreviewMode::Text && g_state.preview.is_json)
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "JSON Preview", NK_TEXT_CENTERED);

            if (g_state.preview.preview_node && (is_json_file(std::get<Core::FileInfo>(g_state.preview.preview_node->data).format) ||
                                                 is_scsp_file(std::get<Core::FileInfo>(g_state.preview.preview_node->data).format)))
            {
                nk_layout_row_begin(ctx, NK_STATIC, 30, 2);
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
                        const std::string base_name = g_state.preview.preview_node ? g_state.preview.preview_node->name : "output";
                        const std::string default_name = Core::ReplaceExtension(base_name, ".json");
                        if (const std::string save_path = DialogPaths::SaveFile("Save JSON", default_name, {"JSON Files", "*.json", "All Files", "*.*"}); !save_path.empty())
                        {
                            if (std::ofstream out(save_path); out.is_open())
                            {
                                out << g_state.preview.text_full;
                                out.close();
                                g_state.tasks.status = "Saved to: " + save_path;
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
            if (g_state.preview.preview_node)
                group_id += "_" + g_state.preview.preview_node->name;
            if (nk_group_begin(ctx, group_id.c_str(), NK_WINDOW_BORDER))
            {
                std::stringstream ss(g_state.preview.text_full);
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
        else if (g_state.preview.mode == PreviewMode::Text && !g_state.preview.is_json)
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, "Text Viewer", NK_TEXT_CENTERED);

            nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
            nk_layout_row_push(ctx, 120);
            if (nk_button_label(ctx, "Copy All"))
            {
                const std::string &data_to_copy = g_state.preview.text_full.empty() ? g_state.preview.text_preview : g_state.preview.text_full;
                SDL_SetClipboardText(data_to_copy.c_str());
            }
            nk_layout_row_push(ctx, 120);
            if (nk_button_label(ctx, "Save As..."))
            {
                try
                {
                    std::string data_to_save = g_state.preview.text_full.empty() ? g_state.preview.text_preview : g_state.preview.text_full;
                    bool is_atlas = data_to_save.find("format: ") != std::string::npos &&
                                    data_to_save.find("filter: ") != std::string::npos;

                    if (is_atlas)
                    {
                        size_t pos = 0;
                        while ((pos = data_to_save.find(".sct", pos)) != std::string::npos)
                        {
                            data_to_save.replace(pos, 4, ".png");
                            pos += 4;
                        }
                    }

                    const std::string base_name = g_state.preview.preview_node ? g_state.preview.preview_node->name : "output";
                    const std::string default_name = Core::ReplaceExtension(base_name, is_atlas ? ".atlas" : ".txt");

                    const std::string save_path = DialogPaths::SaveFile(is_atlas ? "Save Atlas" : "Save Text", default_name,
                                                                        is_atlas ? std::vector<std::string>{"Atlas Files", "*.atlas", "Text Files", "*.txt", "All Files", "*.*"}
                                                                                 : std::vector<std::string>{"Text Files", "*.txt", "All Files", "*.*"});

                    if (!save_path.empty())
                    {
                        std::ofstream out(save_path, std::ios::binary);
                        if (out.is_open())
                        {
                            out << data_to_save;
                            out.close();
                            g_state.tasks.status = "Saved to: " + save_path;
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
            if (g_state.preview.preview_node)
                group_id += "_" + g_state.preview.preview_node->name;
            if (nk_group_begin(ctx, group_id.c_str(), NK_WINDOW_BORDER))
            {
                std::stringstream ss(g_state.preview.text_preview);
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
