#include "parsers/SpineRenderer.h"
#include "parsers/SpineDictionary.h"
#include "gui/AppState.h"
#include "gui/PreviewPanel.h"

AppState g_state;

void reset_app_state()
{
    g_state.browser.data_pack.reset();
    g_state.tasks.scan_complete = false;
    g_state.browser.selection.selected_node = nullptr;
    g_state.browser.selection.selected_nodes.clear();
    g_state.browser.selection.expanded_folders.clear();
    clear_preview();
    g_state.browser.search_query = "";
    memset(g_state.browser.search_buffer, 0, sizeof(g_state.browser.search_buffer));
    memset(g_state.database.search_buffer, 0, sizeof(g_state.database.search_buffer));
    if (g_state.spine.build_future.valid())
        g_state.spine.build_future.wait();
    g_state.spine.dictionary.Clear();
    g_state.spine.show_window = false;
    g_state.spine.selected_index = -1;
    memset(g_state.spine.search_buffer, 0, sizeof(g_state.spine.search_buffer));
    g_state.spine.search_query = "";
    memset(g_state.spine.bone_search_buf, 0, sizeof(g_state.spine.bone_search_buf));
    g_state.spine.active_gizmo = 0;
    g_state.spine.export_pending = false;
    g_state.spine.bone_just_reset = false;
    g_state.spine.selected_bone = "";
    if (g_state.spine.viewer)
        g_state.spine.viewer->unload();
    g_state.spine.viewer.reset();
    g_state.spine.expanded_categories.clear();
    g_state.common.dragging_splitter = false;
    g_state.diff.show_tree = false;
    g_state.diff.root.reset();
    g_state.diff.selection.visible_nodes.clear();
    g_state.diff.selection.expanded_folders.clear();
    g_state.diff.selection.selected_nodes.clear();
    g_state.diff.selection.selected_node = nullptr;
}
