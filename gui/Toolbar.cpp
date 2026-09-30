#include <future>
#include <string>
#include <vector>

#include <SDL.h>

#include "core/Core.h"
#include "core/FileTree.h"
#include "core/DialogPaths.h"
#include "archive/IArchive.h"
#include "gui/AppState.h"
#include "gui/FileBrowserPanel.h"
#include "gui/PreviewPanel.h"
#include "gui/Toolbar.h"

#include "nuklear.h"

void draw_toolbar(nk_context *ctx)
{
    bool pack_loaded = (g_state.browser.data_pack != nullptr);
    bool tree_scanned = pack_loaded && g_state.tasks.scan_complete.load();
    bool selection_exists = g_state.diff.show_tree ? (g_state.diff.selection.selected_node != nullptr) : (g_state.browser.selection.selected_node != nullptr);
    bool has_file_selection = g_state.diff.show_tree ? !g_state.diff.selection.selected_nodes.empty() : !g_state.browser.selection.selected_nodes.empty();
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
            const std::string selected_path = DialogPaths::OpenFile(DialogPaths::Slot::Open, "Select an archive or manifest file",
                                                                    {"Pack / Manifest Files", "*.pack;*.ssra", "All Files", "*.*"});
            if (!selected_path.empty())
            {
                const std::wstring wpath = Core::Utf8ToWString(selected_path);
                reset_app_state();

                g_state.browser.data_pack = IArchive::Create(wpath);
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
                const std::string selected_path = DialogPaths::SelectFolder(DialogPaths::Slot::Open, "Select a folder to view");
                if (!selected_path.empty())
                {
                    const std::wstring wpath = Core::Utf8ToWString(selected_path);
                    reset_app_state();

                    g_state.browser.data_pack = IArchive::Create(wpath);
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
            g_state.tasks.kind = TaskKind::Scan;
            g_state.tasks.status = "Scanning...";
            g_state.tasks.progress = 0.0f;

            g_state.browser.selection.expanded_folders.clear();
            g_state.browser.selection.selected_node = nullptr;
            g_state.browser.selection.selected_nodes.clear();
            g_state.browser.selection.last_clicked_node = nullptr;
            clear_preview();

            g_state.tasks.future = std::async(std::launch::async, []{
                try {
                    g_state.browser.data_pack->Scan(g_state.tasks.progress);
                }catch (...) {} });
        }
        catch (const std::exception &e)
        {
            g_state.tasks.status = "Error starting scan: " + std::string(e.what());
            g_state.tasks.running = false;
            g_state.tasks.kind = TaskKind::None;
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
            set_diff_viewer_mode();
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
            const std::string dest_str = DialogPaths::SelectFolder(DialogPaths::Slot::Extract, "Select destination folder");
            if (!dest_str.empty())
            {
                std::wstring dest_path = Core::Utf8ToWString(dest_str);
                g_state.tasks.running = true;
                g_state.tasks.kind = TaskKind::Extract;
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
            g_state.tasks.running = false;
            g_state.tasks.kind = TaskKind::None;
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
            if (const std::string dest_str = DialogPaths::SelectFolder(DialogPaths::Slot::Extract, "Select destination folder"); !dest_str.empty())
            {
                std::wstring dest_path = Core::Utf8ToWString(dest_str);
                g_state.tasks.running = true;
                g_state.tasks.kind = TaskKind::Extract;
                std::vector<const Core::FileNode *> nodes_to_extract;
                if (g_state.diff.show_tree)
                {
                    nodes_to_extract.reserve(g_state.diff.selection.selected_nodes.size() + 1);
                    for (const auto *n : g_state.diff.selection.selected_nodes)
                    {
                        if (n)
                        {
                            if (const Core::FileNode* fn = Core::FindNodeByPath(g_state.browser.data_pack->GetFileTree(), n->full_path))
                                nodes_to_extract.push_back(fn);
                        }
                    }
                    if (nodes_to_extract.empty() && g_state.diff.selection.selected_node)
                    {
                        if (const Core::FileNode* fn = Core::FindNodeByPath(g_state.browser.data_pack->GetFileTree(), g_state.diff.selection.selected_node->full_path))
                            nodes_to_extract.push_back(fn);
                    }
                }
                else
                {
                    nodes_to_extract.reserve(g_state.browser.selection.selected_nodes.size() + 1);
                    for (const auto *n : g_state.browser.selection.selected_nodes)
                    {
                        if (n)
                            nodes_to_extract.push_back(n);
                    }
                    if (nodes_to_extract.empty() && g_state.browser.selection.selected_node)
                    {
                        nodes_to_extract.push_back(g_state.browser.selection.selected_node);
                    }
                }

                g_state.tasks.status = "Extracting " + std::to_string(nodes_to_extract.size()) + " item(s)...";
                g_state.tasks.progress = 0.0f;
                bool convert_sct = (g_state.common.export_sct_as_png != 0);
                bool convert_db = (g_state.common.export_db_as_json != 0);
                g_state.tasks.future = std::async(std::launch::async, [dest_path, nodes_to_extract, convert_sct, convert_db]()
                                         {
                    try {
                        const float total = nodes_to_extract.empty() ? 1.0f : static_cast<float>(nodes_to_extract.size());
                        for (size_t i = 0; i < nodes_to_extract.size(); i++)
                        {
                            std::atomic local_progress = 0.0f;
                            g_state.browser.data_pack->Extract(*nodes_to_extract[i], dest_path, local_progress, convert_sct, convert_db);
                            g_state.tasks.progress = static_cast<float>(i + 1) / total;
                        }
                    }
                    catch (...) {} });
            }
        }
        catch (const std::exception &e)
        {
            g_state.tasks.status = "Error starting extraction: " + std::string(e.what());
            g_state.tasks.running = false;
            g_state.tasks.kind = TaskKind::None;
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
}
