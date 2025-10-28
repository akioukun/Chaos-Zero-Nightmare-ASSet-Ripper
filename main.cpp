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


#define INITIAL_WINDOW_WIDTH 1400
#define INITIAL_WINDOW_HEIGHT 900
#define DOUBLE_CLICK_TIME_MS 300

#define ICON_FOLDER ""
#define ICON_FILE ""
#define ICON_IMAGE ""
#define ICON_COMPRESSED ""
#define ICON_ATLAS ""

static std::unique_ptr<DataPack> data_pack = nullptr;
static Core::FileNode const* selected_node = nullptr;
static std::future<void> task_future;
static std::atomic<float> task_progress = 0.0f;
static std::atomic<bool> is_task_running = false;
static std::string status_text = "Select a data.pack file to begin.";
static std::unordered_set<const Core::FileNode*> expanded_folders;
static char search_buffer[256] = { 0 };
static std::string search_query = "";

static GLuint preview_texture = 0;
static int preview_width = 0;
static int preview_height = 0;
static bool has_preview = false;
static std::string preview_error = "";
static std::string preview_atlas_data = "";
static bool atlas_wrap_lines = true;
static bool show_atlas_window = false;
static char atlas_filter[256] = { 0 };
static std::vector<char> atlas_text_buf;


static std::string wstring_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), needed, nullptr, nullptr);
    return s;
}

static bool show_credits_window = false;
static bool show_export_success = false;
static std::string export_success_msg = "";

static Uint32 last_click_time = 0;
static const Core::FileNode* last_clicked_node = nullptr;
static int click_count = 0;


int get_file_count(const Core::FileNode& node) {
    try {
        if (std::holds_alternative<Core::FileInfo>(node.data)) return 1;
        int count = 0;
        const auto& folder = std::get<Core::FolderInfo>(node.data);
        for (const auto& child : folder.children) {
            count += get_file_count(child);
        }
        return count;
    }
    catch (...) { return 0; }
}

uint64_t get_folder_size(const Core::FileNode& node) {
    try {
        if (std::holds_alternative<Core::FileInfo>(node.data)) {
            return std::get<Core::FileInfo>(node.data).size;
        }
        uint64_t size = 0;
        const auto& folder = std::get<Core::FolderInfo>(node.data);
        for (const auto& child : folder.children) {
            size += get_folder_size(child);
        }
        return size;
    }
    catch (...) { return 0; }
}

std::string format_size(uint64_t bytes) {
    const char* units[] = { "B", "KB", "MB", "GB" };
    int unit = 0;
    double size = (double)bytes;
    while (size >= 1024.0 && unit < 3) {
        size /= 1024.0;
        unit++;
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.2f %s", size, units[unit]);
    return buffer;
}

bool matches_search(const Core::FileNode& node, const std::string& query){
    if (query.empty()) return true;
    std::string name_lower = node.name;
    std::string query_lower = query;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
    return name_lower.find(query_lower) != std::string::npos;}

bool has_matching_child(const Core::FileNode& node, const std::string& query) {
    if (query.empty()) return true;
    if (matches_search(node, query)) return true;
    if (std::holds_alternative<Core::FolderInfo>(node.data)) {
        const auto& folder = std::get<Core::FolderInfo>(node.data);
        for (const auto& child : folder.children) {
            if (has_matching_child(child, query)) return true;
        }
    }
    return false;
}

bool is_previewable_format(const std::string& ext) {
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" ||
        ext_lower == ".bmp" || ext_lower == ".tga";
}

bool is_atlas_file(const std::string& ext) {
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
    return ext_lower == ".atlas";
}

std::string get_file_icon(const std::string& ext) {
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

    if (ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" ||
        ext_lower == ".bmp" || ext_lower == ".tga") {
        return ICON_IMAGE;
    }
    else if (ext_lower == ".atlas") {
        return ICON_ATLAS;
    }
    else if (ext_lower == ".srt" || ext_lower == ".zip" || ext_lower == ".pack") {
        return ICON_COMPRESSED;
    }
    return ICON_FILE;
}

void load_atlas_preview(const Core::FileNode& node) {
    try {
        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        if (file_data.empty()) {
            preview_atlas_data = "Failed to read atlas file";
            return;
        }
        preview_atlas_data = std::string(file_data.begin(), file_data.end());
        atlas_text_buf.assign(preview_atlas_data.begin(), preview_atlas_data.end());
        atlas_text_buf.push_back('\0');
        if (preview_atlas_data.length() > 20000) {
            preview_atlas_data = preview_atlas_data.substr(0, 20000) + "\n\n... (truncated)";
        }
    }
    catch (const std::exception& e) {
        preview_atlas_data = "Error loading atlas: " + std::string(e.what());
    }
}


void load_image_preview(const Core::FileNode& node) {
    if (preview_texture) {
        glDeleteTextures(1, &preview_texture);
        preview_texture = 0;
    }
    has_preview = false;
    preview_width = 0;
    preview_height = 0;
    preview_error = "";
    preview_atlas_data = "";
    try {
        if (!std::holds_alternative<Core::FileInfo>(node.data)) {
            preview_error = "Not a file";
            return;
        }
        const auto& info = std::get<Core::FileInfo>(node.data);

        if (is_atlas_file(info.format)) {
            load_atlas_preview(node);
            return;
        }

	if (!is_previewable_format(info.format)) {
            preview_error = "Preview not available for " + info.format + " files";
            return;
        }

        std::vector<uint8_t> file_data = data_pack->GetFileData(node);
        if (file_data.empty()) {
            preview_error = "Failed to read file data";
            return;
        }

        std::string ext_lower = info.format;
        std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

        SDL_Surface* rgba_surface = nullptr;

        {
            SDL_RWops* rw = SDL_RWFromMem(file_data.data(), (int)file_data.size());
            if (!rw) {
                preview_error = "Failed to create memory stream";
                return;
            }

            SDL_Surface* surface = IMG_Load_RW(rw, 1);
            if (!surface) {
                preview_error = "Failed to decode image: " + std::string(IMG_GetError());
                return;
            }

            rgba_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(surface);
        }
        if (!rgba_surface) {
            preview_error = "Failed to convert image format";
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
    }
    catch (const std::exception& e) {
        preview_error = "Error: " + std::string(e.what());
        has_preview = false;
    }
    catch (...) {
        preview_error = "Unknown error occurred";
        has_preview = false;
    }
}

void handle_node_click(const Core::FileNode* node, bool is_folder){
    Uint32 current_time = SDL_GetTicks();
    Uint32 time_diff = current_time - last_click_time;

    if (time_diff < 250 && node == last_clicked_node) {
        last_click_time = current_time;
        return;
    }

    click_count = 0;

    if (!is_folder) {
        if (selected_node != node) {
            selected_node = node;
            load_image_preview(*node);
        }
    }
    else {

        if (selected_node != node) selected_node = node;
        has_preview = false;
        preview_error = "";
        preview_atlas_data = "";
    }

    last_click_time = current_time;
    last_clicked_node = node;
}

void export_file_tree_json(const Core::FileNode& node, std::ofstream& out, int depth = 0) {
    std::string indent(depth * 2, ' ');

    out << indent << "{\n";
    out << indent << "  \"name\": \"" << node.name << "\",\n";
    out << indent << "  \"path\": \"" << node.full_path << "\",\n";

    if (std::holds_alternative<Core::FileInfo>(node.data)) {
        const auto& info = std::get<Core::FileInfo>(node.data);
        out << indent << "  \"type\": \"file\",\n";
        out << indent << "  \"size\": " << info.size << ",\n";
        out << indent << "  \"offset\": " << info.offset << ",\n";
        out << indent << "  \"format\": \"" << info.format << "\"\n";
    }
    else {
        const auto& folder = std::get<Core::FolderInfo>(node.data);
        out << indent << "  \"type\": \"folder\",\n";
        out << indent << "  \"children\": [\n";

        for (size_t i = 0; i < folder.children.size(); ++i) {
            export_file_tree_json(folder.children[i], out, depth + 2);
            if (i < folder.children.size() - 1) out << ",";
            out << "\n";
        }
        out << indent << "  ]\n";
    }

    out << indent << "}";
}

void export_to_json(){
    try {
        auto f = pfd::save_file("Export File Map", "filemap.json",
            { "JSON Files", "*.json", "All Files", "*.*" });

        if (!f.result().empty()) {
            std::ofstream out(f.result());
            if (out.is_open()) {
                export_file_tree_json(data_pack->GetFileTree(), out);
                out.close();
                export_success_msg = "File map exported successfully!";
                show_export_success = true;
                status_text = "Exported to: " + f.result();
            }
        }
    }
    catch (const std::exception& e) {
        status_text = "Export error: " + std::string(e.what());
    }
}


void draw_file_node(nk_context* ctx, const Core::FileNode& node, int depth = 0) {
    try {
        if (!has_matching_child(node, search_query)) return;

        if (std::holds_alternative<Core::FolderInfo>(node.data)) {
            const auto& folder = std::get<Core::FolderInfo>(node.data);
            bool is_expanded = expanded_folders.find(&node) != expanded_folders.end();
            bool is_selected = (selected_node == &node);

            struct nk_color bg_color = (depth % 2 == 0) ? nk_rgb(35, 35, 38) : nk_rgb(40, 40, 43);
            if (is_selected) bg_color = nk_rgb(65, 65, 70);

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

            if (nk_button_label_styled(ctx, &expand_style, is_expanded ? "-" : "+")) {
                if (is_expanded) {
                    expanded_folders.erase(&node);
                }
                else {
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
            if (highlight_match) button_style.text_normal = nk_rgb(100, 255, 100);

            if (nk_button_label_styled(ctx, &button_style, folder_label.c_str())) {
                handle_node_click(&node, true);
            }

            nk_layout_row_push(ctx, 200.0f);
            int file_count = get_file_count(node);
            std::string info = std::to_string(file_count) + " items | " + format_size(get_folder_size(node));
            nk_label_colored(ctx, info.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

            nk_layout_row_end(ctx);

            if (is_expanded) {
                for (const auto& child : folder.children)
                    draw_file_node(ctx, child, depth + 1);
                
            }
        }
        else {
            if (!matches_search(node, search_query)) return;

            const auto& file_info = std::get<Core::FileInfo>(node.data);
            bool is_selected = (selected_node == &node);

            struct nk_color bg_color = (depth % 2 == 0) ? nk_rgb(35, 35, 38) : nk_rgb(40, 40, 43);
            if (is_selected) bg_color = nk_rgb(65, 65, 70);

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
                handle_node_click(&node, false);
            nk_layout_row_push(ctx, 200.0f);
            std::string size_str = format_size(file_info.size) + " | " + file_info.format;
            nk_label_colored(ctx, size_str.c_str(), NK_TEXT_LEFT, nk_rgb(150, 150, 150));

            nk_layout_row_end(ctx);
        }}
    catch (const std::exception& e) {
        std::cerr << "Error drawing node: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "Unknown error drawing node" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* win = SDL_CreateWindow("Chaos Zero Nightmare ASSet Ripper v1",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        INITIAL_WINDOW_WIDTH, INITIAL_WINDOW_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    SDL_GLContext glContext = SDL_GL_CreateContext(win);
    glewInit();

    nk_context* ctx = nk_sdl_init(win);
    {
        struct nk_font_atlas* atlas;
        nk_sdl_font_stash_begin(&atlas);
        nk_sdl_font_stash_end();
    }

    bool running = true;
    while (running) {
        SDL_Event evt;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) running = false;
            nk_sdl_handle_event(&evt);
        }
        nk_input_end(ctx);
        if (is_task_running && task_future.valid() &&
            task_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            is_task_running = false;
            try {
                task_future.get();
            }
            catch (const std::exception& e) {
                status_text = "Error: " + std::string(e.what());
            }
            catch (...) {
                status_text = "Unknown error occurred";
            }
            task_progress = 1.0f;
            if (status_text.find("Scanning") != std::string::npos) {
                status_text = "Scan complete. " + std::to_string(get_file_count(data_pack->GetFileTree())) + " files found.";
            }
            else if (status_text.find("Extracting") != std::string::npos) {
                status_text = "Extraction complete.";
            }
        }
        int window_width, window_height;
        SDL_GetWindowSize(win, &window_width, &window_height);
        if (show_credits_window) {
            if (nk_begin(ctx, "Credits", nk_rect(window_width / 2 - 200, window_height / 2 - 150, 400, 300),
                NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) {

                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, "Chaos Zero Nightmare ASSet Ripper v1", NK_TEXT_CENTERED);

                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "", NK_TEXT_LEFT);
                nk_label(ctx, "made with nuklear, sdl2/opengl, portable-file-dialogs", NK_TEXT_LEFT);
                nk_label(ctx, "by @akioukun (github.com/akioukun)", NK_TEXT_LEFT);

                nk_layout_row_dynamic(ctx, 30, 1);
                if (nk_button_label(ctx, "Close")) {
                    show_credits_window = false;
                }
            }
            else {
                show_credits_window = false;
            }
            nk_end(ctx);
        }


        if (show_export_success) {
            if (nk_begin(ctx, "Success", nk_rect(window_width / 2 - 150, window_height / 2 - 50, 300, 100),
                NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {

                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, export_success_msg.c_str(), NK_TEXT_CENTERED);
                nk_layout_row_dynamic(ctx, 30, 1);
                if (nk_button_label(ctx, "OK")) {
                    show_export_success = false;
                }
            }
            else {
                show_export_success = false;
            }
            nk_end(ctx);
        }

        if (nk_begin(ctx, "Main", nk_rect(0, 0, (float)window_width, (float)window_height), NK_WINDOW_NO_SCROLLBAR)) {
            bool pack_loaded = (data_pack != nullptr);
            bool tree_scanned = pack_loaded && !std::get<Core::FolderInfo>(data_pack->GetFileTree().data).children.empty();
            bool selection_exists = (selected_node != nullptr);


            nk_layout_row_dynamic(ctx, 38, 6);

            struct nk_style_button btn_style = ctx->style.button;
            btn_style.rounding = 4.0f;
            btn_style.padding = nk_vec2(10, 8);

            btn_style.normal = nk_style_item_color(nk_rgb(70, 70, 75));
            btn_style.hover = nk_style_item_color(nk_rgb(90, 90, 95));


            bool pack_already_loaded = (data_pack != nullptr);
            bool can_open_pack = !is_task_running && !pack_already_loaded;

            if (can_open_pack && nk_button_label_styled(ctx, &btn_style, "Open Pack")) {
                try {
                    auto f = pfd::open_file("Select a data.pack file", ".",
                        { "Pack Files", "*.pack", "All Files", "*.*" });
                    if (!f.result().empty()) {
                        std::string selected_path = f.result()[0];
                        int size_needed = MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                            (int)selected_path.size(), NULL, 0);
                        std::wstring wpath(size_needed, 0);
                        MultiByteToWideChar(CP_UTF8, 0, selected_path.c_str(),
                            (int)selected_path.size(), &wpath[0], size_needed);

                        data_pack.reset();
                        selected_node = nullptr;
                        expanded_folders.clear();
                        has_preview = false;
                        preview_error = "";
                        preview_atlas_data = "";
                        search_query = "";
                        memset(search_buffer, 0, sizeof(search_buffer));

                        data_pack = std::make_unique<DataPack>(wpath);
                        if (data_pack->GetType() == DataPack::PackType::Unknown) {
                            status_text = "Error: Invalid or unknown file.";
                            data_pack = nullptr;
                        }
                        else {
                            status_text = "Loaded. Click 'Scan Tree' to analyze contents.";
                        }
                    }
                }
                catch (const std::exception& e) {
                    status_text = "Error opening file: " + std::string(e.what());
                }
            }
            else if (is_task_running || pack_already_loaded) {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Open Pack");
                nk_widget_disable_end(ctx);
            }

            if (pack_loaded && !tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Scan Tree")) {
                try {
                    is_task_running = true;
                    status_text = "Scanning...";
                    task_progress = 0.0f;


                    expanded_folders.clear();
                    selected_node = nullptr;
                    last_clicked_node = nullptr;

                    has_preview = false;
                    preview_error = "";
                    preview_atlas_data = "";

                    if (preview_texture) {
                        glDeleteTextures(1, &preview_texture);
                        preview_texture = 0;
                    }
                    task_future = std::async(std::launch::async, [] {
                        try {
                            data_pack->Scan(task_progress);
                        }
                        catch (...) {}
                        });
                }
                catch (const std::exception& e) {
                    status_text = "Error starting scan: " + std::string(e.what());
                    is_task_running = false;
                }
            }
            else if (!pack_loaded || tree_scanned || is_task_running) {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Scan Tree");
                nk_widget_disable_end(ctx);
            }

            if (tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Extract All")) {
                try {
                    auto d = pfd::select_folder("Select destination folder", ".");
                    if (!d.result().empty()) {
                        std::string dest_str = d.result();
                        std::wstring dest_path(dest_str.begin(), dest_str.end());
                        is_task_running = true;
                        status_text = "Extracting...";
                        task_progress = 0.0f;
                        task_future = std::async(std::launch::async, [dest_path]() {
                            try {
                                data_pack->ExtractAll(dest_path, task_progress);
                            }
                            catch (...) {}
                            });
                    }
                }
                catch (const std::exception& e) {
                    status_text = "Error starting extraction: " + std::string(e.what());
                }
            }
            else if (!tree_scanned || is_task_running) {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract All");
                nk_widget_disable_end(ctx);
            }


            if (tree_scanned && selection_exists && !is_task_running &&
                nk_button_label_styled(ctx, &btn_style, "Extract Selected")) {
                try {
                    auto d = pfd::select_folder("Select destination folder", ".");
                    if (!d.result().empty()) {
                        std::string dest_str = d.result();
                        std::wstring dest_path(dest_str.begin(), dest_str.end());
                        is_task_running = true;
                        status_text = "Extracting...";
                        task_progress = 0.0f;
                        const Core::FileNode* node_to_extract = selected_node;
                        task_future = std::async(std::launch::async, [dest_path, node_to_extract]() {
                            try {
                                data_pack->Extract(*node_to_extract, dest_path, task_progress);
                            }
                            catch (...) {}
                            });
                    }
                }
                catch (const std::exception& e) {
                    status_text = "Error starting extraction: " + std::string(e.what());
                }
            }
            else if (!tree_scanned || !selection_exists || is_task_running) {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Extract Selected");
                nk_widget_disable_end(ctx);
            }


            if (tree_scanned && !is_task_running && nk_button_label_styled(ctx, &btn_style, "Export JSON")) {
                export_to_json();
            }
            else if (!tree_scanned || is_task_running) {
                nk_widget_disable_begin(ctx);
                nk_button_label_styled(ctx, &btn_style, "Export JSON");
                nk_widget_disable_end(ctx);
            }


            if (nk_button_label_styled(ctx, &btn_style, "Credits")) {
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
            bool showing_preview_panel = (has_preview || !preview_error.empty() || !preview_atlas_data.empty());
            float left_width = showing_preview_panel ? (preview_atlas_data.empty() ? (float)window_width * 0.45f : (float)window_width * 0.28f)
                : (float)window_width - 20.0f;
            float right_width = (float)window_width - left_width - 30.0f;

            nk_layout_row_begin(ctx, NK_STATIC, content_height,
                (has_preview || !preview_error.empty() || !preview_atlas_data.empty()) ? 2 : 1);
            nk_layout_row_push(ctx, left_width);

            if (nk_group_begin(ctx, "FileTree", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                if (data_pack && tree_scanned) {
                    draw_file_node(ctx, data_pack->GetFileTree());
                }
                else if (data_pack) {
                    nk_layout_row_dynamic(ctx, 25, 1);
                    nk_label(ctx, "Click 'Scan Tree' to load files...", NK_TEXT_CENTERED);
                }
                else {
                    nk_layout_row_dynamic(ctx, 25, 1);
                    nk_label(ctx, "No pack file loaded.", NK_TEXT_CENTERED);
                }
                nk_group_end(ctx);
            }

            if (has_preview || !preview_error.empty() || !preview_atlas_data.empty()) {
                nk_layout_row_push(ctx, right_width);
                if (nk_group_begin(ctx, "Preview", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                    if (has_preview) {
                        nk_layout_row_dynamic(ctx, 25, 1);
                        std::string preview_title = "Image Preview: " + std::to_string(preview_width) +
                            "x" + std::to_string(preview_height);
                        nk_label(ctx, preview_title.c_str(), NK_TEXT_CENTERED);

                        float panel_width = right_width - 20;
                        float panel_height = content_height - 60;
                        float scale_w = panel_width / preview_width;
                        float scale_h = panel_height / preview_height;
                        float scale = (scale_w < scale_h) ? scale_w : scale_h;
                        if (scale > 1.0f) scale = 1.0f;

                        float img_w = preview_width * scale;
                        float img_h = preview_height * scale;

                        nk_layout_row_static(ctx, img_h, (int)img_w, 1);
                        struct nk_image img = nk_image_id((int)preview_texture);
                        nk_image(ctx, img);
                    }
                    else if (!preview_atlas_data.empty()) {
                        nk_layout_row_dynamic(ctx, 25, 1);
                        nk_label(ctx, "Atlas File Content:", NK_TEXT_CENTERED);


                        nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
                        nk_layout_row_push(ctx, 120);
                        if (nk_button_label(ctx, "Open Window")) show_atlas_window = true;
                        nk_layout_row_push(ctx, 120);
                        if (nk_button_label(ctx, "Copy All")) SDL_SetClipboardText(preview_atlas_data.c_str());
                        nk_layout_row_push(ctx, 120);
                        if (nk_button_label(ctx, "Save As...")) {
                            try {
                                auto f = pfd::save_file("Save Atlas Text", "atlas.txt", { "Text", "*.txt", "All Files", "*.*" });
                                if (!f.result().empty()) {
                                    std::ofstream out(f.result());
                                    if (out.is_open()) { out << preview_atlas_data; out.close(); }
                                }
                            }
                            catch (...) {}
                        }
                        nk_layout_row_end(ctx);


                        if (atlas_text_buf.empty()) { atlas_text_buf.push_back('\0'); }
                        nk_layout_row_dynamic(ctx, content_height - 80, 1);
                        nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY, atlas_text_buf.data(), (int)atlas_text_buf.size(), nk_filter_default);
                    }
                    else if (!preview_error.empty()) {
                        nk_layout_row_dynamic(ctx, 25, 1);
                        nk_label(ctx, "Preview Error:", NK_TEXT_CENTERED);
                        nk_layout_row_dynamic(ctx, 20, 1);

                        size_t pos = 0;
                        std::string error_copy = preview_error;
                        while (pos < error_copy.length()) {
                            size_t newline = error_copy.find('\n', pos);
                            if (newline == std::string::npos) {
                                nk_label_colored(ctx, error_copy.substr(pos).c_str(),
                                    NK_TEXT_CENTERED, nk_rgb(255, 150, 150));
                                break;
                            }
                            nk_label_colored(ctx, error_copy.substr(pos, newline - pos).c_str(),
                                NK_TEXT_CENTERED, nk_rgb(255, 150, 150));
                            pos = newline + 1;
                        }
                    }
                    nk_group_end(ctx);
                }
            }

            nk_layout_row_end(ctx);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_size current_progress = (nk_size)(task_progress * 100.0f);
            nk_progress(ctx, &current_progress, 100, NK_FIXED);
            nk_layout_row_dynamic(ctx, 28, 1);
            std::string full_status = status_text;
            if (selection_exists) {
                try {
                    if (std::holds_alternative<Core::FileInfo>(selected_node->data)) {
                        const auto& info = std::get<Core::FileInfo>(selected_node->data);
                        full_status += " | Selected: " + selected_node->name + " (" + format_size(info.size) + ")";
                    }
                    else {
                        full_status += " | Selected: " + selected_node->name + " (" +
                            std::to_string(get_file_count(*selected_node)) + " files)";
                    }
                }
                catch (...) {}
            }
            nk_label(ctx, full_status.c_str(), NK_TEXT_LEFT);
        }
        nk_end(ctx);

        if (show_atlas_window && !preview_atlas_data.empty()) {
            if (nk_begin(ctx, "Atlas Viewer", nk_rect(40, 40, (float)window_width - 80, (float)window_height - 80),
                NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) {

                nk_layout_row_begin(ctx, NK_STATIC, 30, 3);
                nk_layout_row_push(ctx, 120);
                if (nk_button_label(ctx, "Copy All")) SDL_SetClipboardText(preview_atlas_data.c_str());
                nk_layout_row_push(ctx, 120);
                if (nk_button_label(ctx, "Save As...")) {
                    try {
                        auto f = pfd::save_file("Save Atlas Text", "atlas.txt", { "Text", "*.txt", "All Files", "*.*" });
                        if (!f.result().empty()) {
                            std::ofstream out(f.result());
                            if (out.is_open()) { out << preview_atlas_data; out.close(); }
                        }
                    }
                    catch (...) {}
                }
                nk_layout_row_end(ctx);

                if (atlas_text_buf.empty()) { atlas_text_buf.push_back('\0'); }
                nk_layout_row_dynamic(ctx, (float)window_height - 180, 1);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY, atlas_text_buf.data(), (int)atlas_text_buf.size(), nk_filter_default);
            }
            else {
                show_atlas_window = false;
            }
            nk_end(ctx);
        }

        glViewport(0, 0, window_width, window_height);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
        SDL_GL_SwapWindow(win);
    }

    if (preview_texture) glDeleteTextures(1, &preview_texture);
    nk_sdl_shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
