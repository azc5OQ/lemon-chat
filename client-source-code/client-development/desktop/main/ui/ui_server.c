#include "ui/ui_layout.h"
#include "ui/ui_server.h"
#include "ui/ui_headers.h"
#include "platform/system.h"

static desktop_session_t *g_server_session;
static const desktop_session_view_t *g_server_view;
static boole g_server_open;
static uint64 g_server_refresh;
static uint64 g_server_revision;

// declarations
static void _ui_server_internal__text(const char *id, const char *text);
static void _ui_server_internal__show(gui_event_t *event);
static void _ui_server_internal__close(gui_event_t *event);
static void _ui_server_internal__refresh(gui_event_t *event);
static void _ui_server_internal__rows(void);

/**
 * @brief copies plain server information into a control without interpreting markup.
 * @param const char* id -> control ID
 * @param const char* text -> copied display text
 * @return void
 */
static void _ui_server_internal__text(const char *id, const char *text)
{
    gui_node_t *node = scene__find_by_id((char *)id);

    // set the new text
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief creates a bounded public-feature list once for this scene.
 * @return void
 */
static void _ui_server_internal__rows(void)
{
    gui_node_t *parent = scene__find_by_id("server-info-features");
    gui_node_t *row = NULL_POINTER;
    gui_node_t *label = NULL_POINTER;
    gui_node_t *value = NULL_POINTER;
    int64 index = 0;

    // skip existing rows
    if (!parent || scene__find_by_id("server-info-feature-0"))
    {
        return;
    }

    // create the feature rows
    for (index = 0; index < DESKTOP_SERVER_FEATURES; index++)
    {
        // create three nodes
        row = scene__node_new(GUI_NODE_TYPE__DIV);
        label = scene__node_new(GUI_NODE_TYPE__TEXT);
        value = scene__node_new(GUI_NODE_TYPE__TEXT);
        if (!row || !label || !value)
        {
            scene__node_free(row);
            scene__node_free(label);
            scene__node_free(value);
            return;
        }

        // set classes and text
        clib__string_copy(row->klass, sizeof(row->klass), "row server-info-row");
        clib__string_copy(label->klass, sizeof(label->klass), "server-info-label");
        scene__set_text(label, desktop_server_info__feature(index)->label, clib__strlen(desktop_server_info__feature(index)->label));
        clib__string_copy(value->klass, sizeof(value->klass), "server-info-value");
        clib__string_format(value->id, sizeof(value->id), "server-info-feature-%lld", index);

        // add the row
        scene__add_child(row, label);
        scene__add_child(row, value);
        scene__add_child(parent, row);
    }
}

/**
 * @brief requests public information through the normal bounded command queue.
 * @param gui_event_t* event -> unused refresh event
 * @return void
 */
static void _ui_server_internal__refresh(gui_event_t *event)
{
    desktop_action_t action = { 0 };
    (void)event;

    // check the connection
    if (!g_server_view->connected || g_server_view->idle)
    {
        return;
    }

    // request the server information
    action.type = DESKTOP_ACTION_SERVER_INFO;
    g_server_refresh = desktop_system__milliseconds() + 5000;
    if (!desktop_session__action(g_server_session, &action))
    {
        _ui_server_internal__text("server-info-result", "Could not queue the refresh.");
    }
}

/**
 * @brief opens a floating read-only panel while leaving chat and channel controls accessible.
 * @param gui_event_t* event -> unused toolbar event
 * @return void
 */
static void _ui_server_internal__show(gui_event_t *event)
{
    gui_node_t *panel = scene__find_by_id("server-info-panel");

    // check panel and connection
    if (!panel || !g_server_view->connected)
    {
        return;
    }

    // show the panel
    clib__string_copy(panel->klass, sizeof(panel->klass), "column panel");
    desktop_ui_layout__raise(panel);
    g_server_open = TRUE;
    g_server_revision = 0;

    // update the panel content
    _ui_server_internal__rows();
    _ui_server_internal__refresh(event);
    desktop_ui_server__update();
#ifdef _WIN32
    // request a redraw
    platform_win32__request_render();
#endif
}

/**
 * @brief closes the information panel and stops its periodic refresh requests.
 * @param gui_event_t* event -> unused close event
 * @return void
 */
static void _ui_server_internal__close(gui_event_t *event)
{
    gui_node_t *panel = scene__find_by_id("server-info-panel");
    (void)event;

    // hide the information panel
    g_server_open = FALSE;
    if (panel)
    {
        clib__string_copy(panel->klass, sizeof(panel->klass), "column panel hidden");
    }
}

/**
 * @brief displays only published values and refreshes while the panel is open.
 * @return void
 */
void desktop_ui_server__update(void)
{
    gui_node_t *button = scene__find_by_id("server-info-button");
    gui_node_t *refresh = scene__find_by_id("server-info-refresh");
    const desktop_server_info_t *info = &g_server_view->server_info;
    char text[128] = { 0 };
    char id[48] = { 0 };
    int64 index = 0;

    // update the info button
    if (button && !g_server_view->connected)
    {
        button->state = GUI_NODE_STATE__DISABLED;
    }
    else if (button && button->state == GUI_NODE_STATE__DISABLED)
    {
        button->state = GUI_NODE_STATE__DEFAULT;
    }

    // check panel is open
    if (!g_server_open)
    {
        return;
    }

    // close after a disconnect
    if (!g_server_view->connected)
    {
        _ui_server_internal__close(NULL);
        return;
    }

    // update the refresh button
    if (refresh && g_server_view->idle)
    {
        refresh->state = GUI_NODE_STATE__DISABLED;
    }
    else if (refresh && refresh->state == GUI_NODE_STATE__DISABLED)
    {
        refresh->state = GUI_NODE_STATE__DEFAULT;
    }

    // refresh after the delay
    if (desktop_system__milliseconds() >= g_server_refresh)
    {
        _ui_server_internal__refresh(NULL);
    }

    // skip unchanged information
    if (g_server_revision == info->revision && info->ready)
    {
        return;
    }

    // store the new revision
    g_server_revision = info->revision;

    // show the used slots
    clib__string_copy(text, sizeof(text), info->ready ? "Not shared" : "Loading...");
    if (info->ready && info->used_slots >= 0 && info->max_slots >= 0)
    {
        clib__string_format(text, sizeof(text), "%lld / %lld", info->used_slots, info->max_slots);
    }
    _ui_server_internal__text("server-info-slots", text);

    // show the uptime
    clib__string_copy(text, sizeof(text), info->ready ? "Not shared" : "Loading...");
    if (info->ready && info->uptime >= 0)
    {
        if (info->uptime >= 86400)
        {
            clib__string_format(text, sizeof(text), "%lld %s %lld:%02lld hours", info->uptime / 86400, info->uptime < 172800 ? "day" : "days", info->uptime / 3600 % 24, info->uptime / 60 % 60);
        }
        else
        {
            clib__string_format(text, sizeof(text), "%lld:%02lld hours", info->uptime / 3600, info->uptime / 60 % 60);
        }
    }
    _ui_server_internal__text("server-info-uptime", text);

    // show the features
    for (index = 0; index < DESKTOP_SERVER_FEATURES; index++)
    {
        clib__string_format(id, sizeof(id), "server-info-feature-%lld", index);
        _ui_server_internal__text(id, !info->ready ? "Loading..." : !(info->features_known & (1ULL << index)) ? "Not shared" : (info->features_enabled & (1ULL << index)) ? "Enabled" : "Disabled");
    }

    // set the result text
    _ui_server_internal__text("server-info-result", "Server-wide settings. Your group permissions may be more restrictive.");
}

/**
 * @brief registers the public server information panel before layout parsing.
 * @param desktop_session_t* session -> GUI-owned session
 * @param const desktop_session_view_t* view -> stable GUI snapshot
 * @return void
 */
void desktop_ui_server__initialize(desktop_session_t *session, const desktop_session_view_t *view)
{
    // store the arguments
    g_server_session = session;
    g_server_view = view;

    // register the handlers
    scene__register_handler("on_show_server_info", _ui_server_internal__show);
    scene__register_handler("on_close_server_info", _ui_server_internal__close);
    scene__register_handler("on_refresh_server_info", _ui_server_internal__refresh);
}
