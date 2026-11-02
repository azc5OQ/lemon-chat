#include "ui/ui_layout.h"
#include "ui/ui_headers.h"
#include "widget_registry.h"

static widget_vtable_t g_layout_div;
static widget_vtable_t g_layout_original_div;
static widget_vtable_t g_layout_window;
static widget_vtable_t g_layout_original_window;
static widget_vtable_t g_layout_button;
static widget_vtable_t g_layout_original_button;
static float g_layout_sidebar_width = 286;
static float g_layout_sidebar_grab;
static widget_vtable_t g_layout_pointer[GUI_NODE_TYPE__COUNT];
static void (*g_layout_pointer_down[GUI_NODE_TYPE__COUNT])(gui_node_t*, int64, int64, int64);

// declarations
static void _ui_layout_internal__div(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _ui_layout_internal__window(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _ui_layout_internal__tabs(gui_node_t* node, float x, float y, float width, float height, float scale);
static void _ui_layout_internal__down(gui_node_t* node, int64 x, int64 y, int64 button);
static boole _ui_layout_internal__drag(gui_node_t* node, int64 x, int64 y);
static void _ui_layout_internal__key(gui_node_t* node, int64 key, boole down);
static void _ui_layout_internal__scroll_tabs(gui_event_t* event);
static void _ui_layout_internal__pointer(gui_node_t* node, int64 x, int64 y, int64 button);

/**
 * @brief raises a floating panel while keeping popup menus above all panels.
 * @param gui_node_t* node -> clicked or opened scene-owned panel or one of its descendants
 * @return void
 */
void desktop_ui_layout__raise(gui_node_t* node)
{
    gui_node_t* root = scene__root();
    gui_node_t* previous = NULL;
    gui_node_t* child = NULL;
    int64 order = 10;
    while (node && node->parent != root)
    {
        node = node->parent;
    }
    if (!root || !node || clib__strncmp(node->klass, "column panel", 12) != 0)
    {
        return;
    }
    for (child = root->first_child; child && child != node; child = child->next_sibling)
    {
        previous = child;
    }
    if (child && root->last_child != node)
    {
        if (previous)
        {
            previous->next_sibling = node->next_sibling;
        }
        else
        {
            root->first_child = node->next_sibling;
        }
        root->last_child->next_sibling = node;
        root->last_child = node;
        node->next_sibling = NULL;
    }
    for (child = root->first_child; child; child = child->next_sibling)
    {
        if (clib__strncmp(child->klass, "column panel", 12) == 0)
        {
            child->resolved.z_index = (int)order++;
        }
    }
    root->layout_memo_frame = ~(uint64)0;
}

/**
 * @brief activates the clicked panel before forwarding the original widget input.
 * @param gui_node_t* node -> clicked scene-owned widget
 * @param int64 x -> pointer x coordinate
 * @param int64 y -> pointer y coordinate
 * @param int64 button -> pointer button
 * @return void
 */
static void _ui_layout_internal__pointer(gui_node_t* node, int64 x, int64 y, int64 button)
{
    desktop_ui_layout__raise(node);
    if (g_layout_pointer_down[node->type])
    {
        g_layout_pointer_down[node->type](node, x, y, button);
    }
}

/**
 * @brief fits floating panels and the sidebar to the current viewport before children are measured.
 * @param gui_node_t* node -> scene-owned window
 * @param float x -> physical window origin
 * @param float y -> physical window origin
 * @param float width -> available physical width
 * @param float height -> available physical height
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_layout_internal__window(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    gui_node_t* child = NULL;
    gui_node_t* sidebar = scene__find_by_id("sidebar");
    gui_node_t* editor = scene__find_by_id("channel-editor-sidebar");
    float available_w = width / scale;
    float available_h = height / scale;
    float panel_w = 0;
    float panel_h = 0;
    float left = 0;
    float top = 0;
    float placed = 0;
    int64 order = 10;
    if (sidebar)
    {
        sidebar->resolved.size_w = clib__fminf(g_layout_sidebar_width,
            clib__fmaxf(240, available_w - 346));
    }
    for (child = node->first_child; child; child = child->next_sibling)
    {
        if (clib__strncmp(child->klass, "column panel", 12) != 0)
        {
            continue;
        }
        child->resolved.z_index = (int)order++;
        panel_w = clib__fminf(child->resolved.size_w, clib__fmaxf(1, available_w - 16));
        panel_h = clib__fminf(child->resolved.size_h, clib__fmaxf(1, available_h - 64));
        left = child->resolved.inset_r_set ? available_w - child->resolved.inset_r - panel_w : child->resolved.inset_l;
        top = child->resolved.inset_t;
        placed = clib__fmaxf(8, clib__fminf(left + child->drag_offset_x / scale, available_w - panel_w - 8));
        child->drag_offset_x = (placed - left) * scale;
        placed = clib__fmaxf(48, clib__fminf(top + child->drag_offset_y / scale, available_h - panel_h - 8));
        child->drag_offset_y = (placed - top) * scale;
        child->resolved.size_w = panel_w;
        child->resolved.size_h = panel_h;
        child->resolved.inset_l = left;
        child->resolved.inset_l_set = TRUE;
        child->resolved.inset_r = 0;
        child->resolved.inset_r_set = FALSE;
        if (editor && clib__strcmp(child->id, "channels-panel") == 0)
        {
            editor->resolved.size_w = clib__fminf(200, panel_w * 0.27f);
        }
    }
    g_layout_original_window.layout(node, x, y, width, height, scale);
}

/**
 * @brief clips a horizontal tab strip and brings a newly selected tab fully into view.
 * @param gui_node_t* node -> scene-owned conversation strip
 * @param float x -> physical origin
 * @param float y -> physical origin
 * @param float width -> available physical width
 * @param float height -> available physical height
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_layout_internal__tabs(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    gui_node_t* child = NULL;
    gui_node_t* previous = scene__find_by_id("tabs-previous");
    gui_node_t* next = scene__find_by_id("tabs-next");
    float cursor = 0;
    float selected_left = 0;
    float selected_right = 0;
    float maximum = 0;
    float tab_width = 190 * scale;
    boole committed = width > 0 && height > 0 && !scene__layout_is_measuring();
    node->bounds = (gui_rect_t){x, y, clib__fmaxf(0, width), node->resolved.size_h * scale};
    for (child = node->first_child; child; child = child->next_sibling)
    {
        if (child->value == node->value)
        {
            selected_left = cursor;
            selected_right = cursor + tab_width;
        }
        cursor += tab_width + 4 * scale;
    }
    node->content_w = clib__fmaxf(0, cursor - 4 * scale);
    node->content_h = node->bounds.h;
    maximum = clib__fmaxf(0, node->content_w - node->bounds.w);
    if (committed && node->user_data)
    {
        if (selected_right > node->scroll_x + node->bounds.w)
        {
            node->scroll_x = selected_right - node->bounds.w;
        }
        if (selected_left < node->scroll_x)
        {
            node->scroll_x = selected_left;
        }
        node->user_data = NULL;
    }
    if (committed)
    {
        node->scroll_x = clib__fmaxf(0, clib__fminf(node->scroll_x, maximum));
        node->scroll_x_target = node->scroll_x;
        if (previous)
        {
            previous->state = node->scroll_x <= 0 ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
        }
        if (next)
        {
            next->state = node->scroll_x >= maximum ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
        }
    }
    cursor = x - node->scroll_x;
    for (child = node->first_child; child; child = child->next_sibling)
    {
        child->resolved.size_w = 190;
        scene__layout_node(child, cursor, y, tab_width, node->bounds.h);
        cursor += tab_width + 4 * scale;
    }
}

/**
 * @brief moves the tab viewport without changing the selected chat or its draft.
 * @param gui_event_t* event -> previous/next button click
 * @return void
 */
static void _ui_layout_internal__scroll_tabs(gui_event_t* event)
{
    gui_node_t* tabs = scene__find_by_id("conversation-tabs");
    float direction = 1;
    if (tabs && event && event->sender)
    {
        direction = clib__strcmp(event->sender->id, "tabs-previous") == 0 ? -1 : 1;
        tabs->scroll_x = clib__fmaxf(0, clib__fminf(tabs->scroll_x + direction * tabs->bounds.w * 0.75f,
            clib__fmaxf(0, tabs->content_w - tabs->bounds.w)));
        tabs->scroll_x_target = tabs->scroll_x;
        tabs->user_data = NULL;
    }
}

/**
 * @brief starts a sidebar resize while preserving normal button input elsewhere.
 * @param gui_node_t* node -> pressed button
 * @param int64 x -> physical pointer position
 * @param int64 y -> physical pointer position
 * @param int64 button -> mouse button code, zero for left
 * @return void
 */
static void _ui_layout_internal__down(gui_node_t* node, int64 x, int64 y, int64 button)
{
    if (clib__strcmp(node->id, "sidebar-divider") == 0 && button == 0)
    {
        g_layout_sidebar_grab = (float)x - node->bounds.x;
        scene__set_focus(node);
    }
    else if (g_layout_original_button.on_mouse_down)
    {
        g_layout_original_button.on_mouse_down(node, x, y, button);
    }
}

/**
 * @brief resizes the tree without consuming the minimum conversation space.
 * @param gui_node_t* node -> pressed divider or other button
 * @param int64 x -> physical pointer position
 * @param int64 y -> physical pointer position
 * @return boole -> TRUE when the divider owns this drag
 */
static boole _ui_layout_internal__drag(gui_node_t* node, int64 x, int64 y)
{
    gui_node_t* workspace = scene__find_by_id("workspace");
    float scale = scene__scale();
    if (workspace && clib__strcmp(node->id, "sidebar-divider") == 0)
    {
        g_layout_sidebar_width = clib__fmaxf(240, clib__fminf(480,
            clib__fminf(((float)x - workspace->bounds.x - g_layout_sidebar_grab) / scale, workspace->bounds.w / scale - 346)));
        return TRUE;
    }
    return g_layout_original_button.on_mouse_drag ? g_layout_original_button.on_mouse_drag(node, x, y) : FALSE;
}

/**
 * @brief lets the focused divider move with arrow keys or return to its initial width with Home.
 * @param gui_node_t* node -> focused button
 * @param int64 key -> portable virtual key
 * @param boole down -> TRUE for key down
 * @return void
 */
static void _ui_layout_internal__key(gui_node_t* node, int64 key, boole down)
{
    if (clib__strcmp(node->id, "sidebar-divider") == 0 && down)
    {
        if (key == 37 || key == 39)
        {
            g_layout_sidebar_width = clib__fmaxf(240, clib__fminf(480, g_layout_sidebar_width + (key == 37 ? -8 : 8)));
        }
        else if (key == 36)
        {
            g_layout_sidebar_width = 320;
        }
    }
    else if (g_layout_original_button.on_key)
    {
        g_layout_original_button.on_key(node, key, down);
    }
}

/**
 * @brief sizes application containers while Meek preserves scroll state during measurement.
 * @param gui_node_t* node -> scene-owned container
 * @param float x -> allocated horizontal position
 * @param float y -> allocated vertical position
 * @param float width -> allocated width, negative for intrinsic measurement
 * @param float height -> allocated height
 * @param float scale -> current display scale
 * @return void
 */
static void _ui_layout_internal__div(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    gui_node_t* child = NULL;
    float button_width = 0;
    int64 count = node->child_count;
    if (count > 0 && width > 0 && (clib__strcmp(node->klass, "row audio-buttons") == 0
        || clib__strcmp(node->klass, "row bookmark-buttons") == 0
        || clib__strcmp(node->klass, "row attachment-actions") == 0
        || clib__strcmp(node->klass, "row identity-actions") == 0
        || clib__strcmp(node->id, "identity-file-actions") == 0
        || clib__strcmp(node->id, "chat-settings-buttons") == 0))
    {
        button_width = clib__fmaxf(1, (width / scale - node->resolved.pad_l - node->resolved.pad_r - (count - 1) * node->resolved.gap) / count);
        for (child = node->first_child; child; child = child->next_sibling)
        {
            child->resolved.width_pct = 0;
            child->resolved.size_w = button_width;
        }
    }
    if (clib__strncmp(node->id, "server-key-row-", 15) == 0 && width > 0)
    {
        for (child = node->first_child; child; child = child->next_sibling)
        {
            child->resolved.width_pct = 0;
            if (child->type == GUI_NODE_TYPE__INPUT)
            {
                child->resolved.width_pct = 0;
                child->resolved.size_w = clib__fmaxf(1, width / scale - 18 - 110 - 78 - 3 * node->resolved.gap);
            }
        }
    }
    if (clib__strcmp(node->id, "connection-buttons") == 0 && width > 0)
    {
        button_width = clib__fminf(120, clib__fmaxf(1, (width / scale - (count - 1) * node->resolved.gap) / clib__fmaxf(1, (float)count)));
        for (child = node->first_child; child; child = child->next_sibling)
        {
            child->resolved.width_pct = 0;
            child->resolved.size_w = button_width;
        }
    }
    if (width > 0 && node->first_child && (clib__strcmp(node->klass, "row connection-address-row") == 0
        || clib__strcmp(node->klass, "row connection-key-heading") == 0
        || clib__strcmp(node->klass, "row chat-message-heading") == 0))
    {
        child = node->first_child;
        child->resolved.width_pct = 0;
        child->resolved.size_w = clib__fmaxf(1, width / scale - node->resolved.gap
            - (clib__strcmp(node->klass, "row chat-message-heading") == 0 ? (node->child_count > 1 ? 26 : 0)
                : clib__strcmp(node->klass, "row connection-address-row") == 0 ? 90 : 100));
    }
    if (clib__strcmp(node->id, "conversation-tabs") == 0)
    {
        _ui_layout_internal__tabs(node, x, y, width, height, scale);
    }
    else
    {
        g_layout_original_div.layout(node, x, y, width, height, scale);
    }
    if (clib__strcmp(node->id, "input-meter-fill") == 0 && width > 0 && height > 0)
    {
        node->bounds.w *= clib__fmaxf(0, clib__fminf(1, node->value));
    }
}

/**
 * @brief extends the public widget API while leaving vendored Meek UI sources unchanged.
 * @return void
 */
void desktop_ui_layout__register(void)
{
    const widget_vtable_t* original = widget_registry__get(GUI_NODE_TYPE__DIV);
    int64 type = 0;
    if (!original || !original->layout || original == &g_layout_div)
    {
        return;
    }
    g_layout_original_div = *original;
    g_layout_div = *original;
    g_layout_div.layout = _ui_layout_internal__div;
    widget_registry__register(GUI_NODE_TYPE__DIV, &g_layout_div);
    original = widget_registry__get(GUI_NODE_TYPE__WINDOW);
    if (original && original->layout)
    {
        g_layout_original_window = *original;
        g_layout_window = *original;
        g_layout_window.layout = _ui_layout_internal__window;
        widget_registry__register(GUI_NODE_TYPE__WINDOW, &g_layout_window);
    }
    original = widget_registry__get(GUI_NODE_TYPE__BUTTON);
    if (original)
    {
        g_layout_original_button = *original;
        g_layout_button = *original;
        g_layout_button.on_mouse_down = _ui_layout_internal__down;
        g_layout_button.on_mouse_drag = _ui_layout_internal__drag;
        g_layout_button.on_key = _ui_layout_internal__key;
        widget_registry__register(GUI_NODE_TYPE__BUTTON, &g_layout_button);
    }
    scene__register_handler("on_scroll_tabs", _ui_layout_internal__scroll_tabs);
    for (type = 0; type < GUI_NODE_TYPE__COUNT; type++)
    {
        original = widget_registry__get((gui_node_type_e)type);
        if (original && original->on_mouse_down != _ui_layout_internal__pointer)
        {
            g_layout_pointer[type] = *original;
            g_layout_pointer_down[type] = original->on_mouse_down;
            g_layout_pointer[type].on_mouse_down = _ui_layout_internal__pointer;
            widget_registry__register((gui_node_type_e)type, &g_layout_pointer[type]);
        }
    }
}
