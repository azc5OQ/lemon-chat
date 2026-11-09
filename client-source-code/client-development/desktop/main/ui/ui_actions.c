#include "ui/ui_layout.h"
#include "ui/ui_actions.h"
#include "ui/ui_headers.h"
#include "platform/system.h"
#include "clib.h"
#include <stdlib.h>

static desktop_session_t *g_actions_session;
static const desktop_session_view_t *g_actions_view;
static desktop_settings_t *g_actions_settings;
static desktop_action_t g_actions_member;
static desktop_action_t g_actions_pending;
static int64 *g_actions_destinations;
static int64 g_actions_destination_count;
static uint64 g_actions_access_time;
static uint64 g_actions_notice;
static boole g_actions_connected;
static char g_actions_rename[101];

// declarations
static void _ui_actions_internal__text(const char *id, const char *text);
static void _ui_actions_internal__panel(const char *id, boole visible);
static void _ui_actions_internal__enabled(const char *id, boole enabled);
static const desktop_client_t *_ui_actions_internal__member(void);
static void _ui_actions_internal__profile(gui_event_t *event);
static void _ui_actions_internal__idle(gui_event_t *event);
static void _ui_actions_internal__member_action(gui_event_t *event);
static void _ui_actions_internal__submit(gui_event_t *event);
static void _ui_actions_internal__confirm(gui_event_t *event);
static void _ui_actions_internal__close(gui_event_t *event);
static void _ui_actions_internal__move(gui_event_t *event);
static void _ui_actions_internal__destinations(void);
static void _ui_actions_internal__close_forms(void);
static void _ui_actions_internal__toolbar_hint(void);

/**
 * @brief identifies icon-only toolbar controls in the status bar when hovered.
 * @return void
 */
static void _ui_actions_internal__toolbar_hint(void)
{
    static const char *ids[] = { "server-info-button", "channels-button", "profile-button", "idle-button", "audio-settings-button", "local-settings-button" };
    static const char *names[] = { "Server info", "Channels", "Profile and identity", "Go idle", "Audio settings", "Local settings" };
    gui_node_t *node = NULL_POINTER;
    const char *hint = "";
    int64 index = 0;
    for (index = 0; index < 6; index++)
    {
        node = scene__find_by_id((char *)ids[index]);
        if (node && node->state == GUI_NODE_STATE__HOVER)
        {
            hint = index == 3 && g_actions_view->idle ? "Return to channel" : names[index];
            break;
        }
    }
    node = scene__find_by_id("toolbar-hint");
    if (node && clib__strcmp(node->text, hint) != 0)
    {
        scene__set_text(node, hint, clib__strlen(hint));
    }
}

/**
 * @brief replaces control text as plain text, never as parsed markup.
 * @param const char* id -> scene control ID
 * @param const char* text -> bounded display text
 * @return void
 */
static void _ui_actions_internal__text(const char *id, const char *text)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief changes one floating panel without intercepting input in the chat behind it.
 * @param const char* id -> panel ID
 * @param boole visible -> nonzero to show the panel
 * @return void
 */
static void _ui_actions_internal__panel(const char *id, boole visible)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), visible ? "column panel" : "column panel hidden");
        if (visible)
        {
            desktop_ui_layout__raise(node);
        }
    }
#ifdef _WIN32
    platform_win32__request_render();
#endif
}

/**
 * @brief retains action positions while making unavailable controls noninteractive.
 * @param const char* id -> button ID
 * @param boole enabled -> nonzero when this action is currently available
 * @return void
 */
static void _ui_actions_internal__enabled(const char *id, boole enabled)
{
    gui_node_t *node = scene__find_by_id((char *)id);
    if (node)
    {
        if (!enabled)
        {
            node->state = GUI_NODE_STATE__DISABLED;
        }
        else if (node->state == GUI_NODE_STATE__DISABLED)
        {
            node->state = GUI_NODE_STATE__DEFAULT;
        }
    }
}

/**
 * @brief resolves the member selection by connection lifetime and channel.
 * @return const desktop_client_t* -> GUI snapshot record, or NULL after a move/disconnect
 */
static const desktop_client_t *_ui_actions_internal__member(void)
{
    int64 index = 0;
    for (index = 0; g_actions_view->connected && index < g_actions_view->client_count; index++)
    {
        if (g_actions_view->clients[index].id == g_actions_member.peer && g_actions_view->clients[index].serial == g_actions_member.serial && g_actions_view->clients[index].channel == g_actions_member.channel)
        {
            return &g_actions_view->clients[index];
        }
    }
    return NULL;
}

/**
 * @brief captures a clicked member and requests current permission hints.
 * @param const desktop_client_t* member -> GUI snapshot record
 * @return void
 */
void desktop_ui_actions__select(const desktop_client_t *member)
{
    clib__null_memory(&g_actions_member, sizeof(g_actions_member));
    g_actions_member.peer = member->id;
    g_actions_member.serial = member->serial;
    g_actions_member.channel = member->channel;
    g_actions_access_time = 0;
    desktop_ui_actions__update();
}

/**
 * @brief opens a rename form using the current server-confirmed name.
 * @param gui_event_t* event -> unused toolbar event
 * @return void
 */
static void _ui_actions_internal__profile(gui_event_t *event)
{
    int64 index = 0;
    (void)event;
    if (!g_actions_view->connected || g_actions_view->idle || !g_actions_view->policy_rename)
    {
        return;
    }
    _ui_actions_internal__close_forms();
    clib__null_memory(&g_actions_pending, sizeof(g_actions_pending));
    g_actions_pending.type = DESKTOP_ACTION_RENAME;
    _ui_actions_internal__text("action-title", "Change username");
    _ui_actions_internal__text("action-label", "New username");
    _ui_actions_internal__text("action-submit", "Save changes");
    _ui_actions_internal__text("action-result", "Changes apply when saved.");
    for (index = 0; index < g_actions_view->client_count; index++)
    {
        if (g_actions_view->clients[index].id == g_actions_view->local_id)
        {
            _ui_actions_internal__text("action-value", g_actions_view->clients[index].name);
        }
    }
    _ui_actions_internal__panel("action-panel", 1);
}

/**
 * @brief requests idle or the server's normal return destination.
 * @param gui_event_t* event -> unused toolbar event
 * @return void
 */
static void _ui_actions_internal__idle(gui_event_t *event)
{
    desktop_action_t action = { 0 };
    (void)event;
    action.type = g_actions_view->idle ? DESKTOP_ACTION_RETURN : DESKTOP_ACTION_IDLE;
    action.value = -1;
    if (!desktop_session__action(g_actions_session, &action))
    {
        _ui_actions_internal__text("connection-status", "Idle request could not be queued.");
    }
}

/**
 * @brief creates a channel picker whose button values remain stable while it is open.
 * @return void
 */
static void _ui_actions_internal__destinations(void)
{
    gui_node_t *parent = scene__find_by_id("move-destinations");
    gui_node_t *node = NULL_POINTER;
    gui_node_t *next = NULL_POINTER;
    int64 index = 0;
    if (!parent)
    {
        return;
    }
    node = parent->first_child;
    parent->first_child = NULL;
    parent->last_child = NULL;
    parent->child_count = 0;
    while (node)
    {
        next = node->next_sibling;
        node->parent = NULL;
        node->next_sibling = NULL;
        scene__node_free(node);
        node = next;
    }
    free(g_actions_destinations);
    g_actions_destinations = NULL;
    g_actions_destination_count = 0;
    if (g_actions_view->channel_count > 0)
    {
        g_actions_destinations = malloc((size_t)g_actions_view->channel_count * sizeof(*g_actions_destinations));
        if (!g_actions_destinations)
        {
            _ui_actions_internal__text("connection-status", "Not enough memory to display move destinations.");
            return;
        }
    }
    g_actions_destination_count = g_actions_view->channel_count;
    for (index = 0; index < g_actions_destination_count; index++)
    {
        g_actions_destinations[index] = g_actions_view->channels[index].id;
        node = scene__node_new(GUI_NODE_TYPE__BUTTON);
        if (node)
        {
            scene__set_text(node, g_actions_view->channels[index].name, clib__strlen(g_actions_view->channels[index].name));
            clib__string_copy(node->klass, sizeof(node->klass), "destination");
            scene__set_on_click(node, "on_action_move");
            node->user_data = (void *)(nuint)(index + 1);
            node->state = g_actions_destinations[index] == g_actions_pending.channel ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
            scene__add_child(parent, node);
        }
    }
}

/**
 * @brief opens bounded action forms or a kick confirmation against a captured identity.
 * @param gui_event_t* event -> clicked member action button
 * @return void
 */
static void _ui_actions_internal__member_action(gui_event_t *event)
{
    desktop_ui_actions__activate(event && event->sender ? event->sender->id : "");
}

/**
 * @brief opens the same captured action from a member panel or context menu.
 * @param const char* id -> existing member action identifier
 * @return void
 */
void desktop_ui_actions__activate(const char *id)
{
    const desktop_client_t *member = _ui_actions_internal__member();
    char title[160] = { 0 };
    if (!member || g_actions_view->idle || member->id == g_actions_view->local_id)
    {
        return;
    }
    _ui_actions_internal__close_forms();
    g_actions_pending = g_actions_member;
    if (clib__strcmp(id, "member-poke") == 0 && !member->music)
    {
        g_actions_pending.type = DESKTOP_ACTION_POKE;
        clib__string_format(title, sizeof(title), "Poke %s", member->name);
        _ui_actions_internal__text("action-title", title);
        _ui_actions_internal__text("action-label", "Message");
        _ui_actions_internal__text("action-value", "");
        _ui_actions_internal__text("action-submit", "Send poke");
        _ui_actions_internal__text("action-result", "");
        _ui_actions_internal__panel("action-panel", 1);
    }
    else if ((clib__strcmp(id, "member-kick") == 0 || clib__strcmp(id, "member-ban") == 0) && desktop_session__allows(g_actions_view, member, clib__strcmp(id, "member-ban") == 0 ? DESKTOP_ACTION_BAN : DESKTOP_ACTION_KICK_SERVER, 0))
    {
        g_actions_pending.type = clib__strcmp(id, "member-ban") == 0 ? DESKTOP_ACTION_BAN : DESKTOP_ACTION_KICK_SERVER;
        clib__string_format(title, sizeof(title), "%s %s from server?", g_actions_pending.type == DESKTOP_ACTION_BAN ? "Ban" : "Kick", member->name);
        _ui_actions_internal__text("confirm-question", title);
        _ui_actions_internal__text("confirm-description", g_actions_pending.type == DESKTOP_ACTION_BAN ? "They will be disconnected and blocked until the ban is removed." : "They will be disconnected, but can connect again.");
        _ui_actions_internal__panel("confirm-panel", TRUE);
    }
    else if (!member->idle && !member->music && member->access_known)
    {
        if (clib__strcmp(id, "member-move") == 0 && (member->access & DESKTOP_ACCESS_MOVE))
        {
            g_actions_pending.type = DESKTOP_ACTION_MOVE;
            clib__string_format(title, sizeof(title), "Move %s to channel", member->name);
            _ui_actions_internal__text("move-title", title);
            _ui_actions_internal__destinations();
            _ui_actions_internal__panel("move-panel", 1);
        }
        else if (clib__strcmp(id, "member-kick-channel") == 0 && member->channel != 0 && (member->access & DESKTOP_ACCESS_KICK_CHANNEL))
        {
            g_actions_pending.type = DESKTOP_ACTION_KICK_CHANNEL;
            clib__string_format(title, sizeof(title), "Kick %s from channel?", member->name);
            _ui_actions_internal__text("confirm-question", title);
            _ui_actions_internal__text("confirm-description", "They will be moved to root and remain connected to the server.");
            _ui_actions_internal__panel("confirm-panel", 1);
        }
        else if (clib__strcmp(id, "member-mute-channel") == 0 && (member->access & DESKTOP_ACCESS_MUTE_CHANNEL))
        {
            g_actions_pending.type = DESKTOP_ACTION_MUTE_CHANNEL;
            clib__string_format(title, sizeof(title), "Mute %s for everyone", member->name);
            _ui_actions_internal__text("action-title", title);
            _ui_actions_internal__text("action-label", "Minutes (0 removes the mute, maximum 1440)");
            _ui_actions_internal__text("action-value", "1");
            _ui_actions_internal__text("action-submit", "Apply mute");
            _ui_actions_internal__text("action-result", "");
            _ui_actions_internal__panel("action-panel", 1);
        }
        else if (clib__strcmp(id, "member-stop-stream") == 0 && (member->access & DESKTOP_ACCESS_STOP_STREAM))
        {
            g_actions_pending.type = DESKTOP_ACTION_STOP_STREAM;
            desktop_session__action(g_actions_session, &g_actions_pending);
        }
    }
}

/**
 * @brief submits an explicitly saved form after checking its whole numeric or text value.
 * @param gui_event_t* event -> unused form button event
 * @return void
 */
static void _ui_actions_internal__submit(gui_event_t *event)
{
    gui_node_t *value = scene__find_by_id("action-value");
    char *end = NULL_POINTER;
    long minutes = 0;
    uint64 index = 0;
    (void)event;
    if (!value || !value->text[0])
    {
        _ui_actions_internal__text("action-result", "Enter a value first.");
        return;
    }
    if (g_actions_pending.type == DESKTOP_ACTION_MUTE_CHANNEL)
    {
        for (index = 0; value->text[index]; index++)
        {
            if (!clib__isdigit((ubyte)value->text[index]) || index > 3)
            {
                _ui_actions_internal__text("action-result", "Enter a whole number from 0 to 1440.");
                return;
            }
        }
        minutes = clib__strtol(value->text, &end, 10);
        if (!end || *end || minutes < 0 || minutes > 1440)
        {
            _ui_actions_internal__text("action-result", "Enter a whole number from 0 to 1440.");
            return;
        }
        g_actions_pending.value = minutes * 60;
    }
    else if (g_actions_pending.type != DESKTOP_ACTION_RENAME && g_actions_pending.type != DESKTOP_ACTION_POKE)
    {
        return;
    }
    if (g_actions_pending.type == DESKTOP_ACTION_RENAME && clib__strlen(value->text) > 100)
    {
        _ui_actions_internal__text("action-result", "The name must fit in 100 UTF-8 bytes.");
        return;
    }
    clib__string_copy(g_actions_pending.text, sizeof(g_actions_pending.text), value->text);
    if (desktop_session__action(g_actions_session, &g_actions_pending))
    {
        if (g_actions_pending.type == DESKTOP_ACTION_RENAME)
        {
            clib__string_copy(g_actions_rename, sizeof(g_actions_rename), value->text);
        }
        _ui_actions_internal__text("action-value", "");
        _ui_actions_internal__panel("action-panel", 0);
    }
    else
    {
        _ui_actions_internal__text("action-result", "The client moved or disconnected, or the request queue is full.");
    }
}

/**
 * @brief confirms only the captured kick, never a newly selected member.
 * @param gui_event_t* event -> unused confirmation event
 * @return void
 */
static void _ui_actions_internal__confirm(gui_event_t *event)
{
    (void)event;
    if (g_actions_pending.type == DESKTOP_ACTION_KICK_CHANNEL || g_actions_pending.type == DESKTOP_ACTION_KICK_SERVER || g_actions_pending.type == DESKTOP_ACTION_BAN)
    {
        if (!desktop_session__action(g_actions_session, &g_actions_pending))
        {
            _ui_actions_internal__text("connection-status", "The client moved or disconnected. Select them again.");
        }
    }
    g_actions_pending.type = DESKTOP_ACTION_NONE;
    _ui_actions_internal__panel("confirm-panel", 0);
}

/**
 * @brief chooses a channel from the captured picker rather than a changing array index.
 * @param gui_event_t* event -> button with one-based destination slot
 * @return void
 */
static void _ui_actions_internal__move(gui_event_t *event)
{
    nuint index = event && event->sender ? (nuint)event->sender->user_data : 0;
    if (g_actions_pending.type != DESKTOP_ACTION_MOVE || !index || index > g_actions_destination_count)
    {
        return;
    }
    g_actions_pending.value = g_actions_destinations[index - 1];
    if (!desktop_session__action(g_actions_session, &g_actions_pending))
    {
        _ui_actions_internal__text("connection-status", "The client moved or disconnected. Select them again.");
    }
    g_actions_pending.type = DESKTOP_ACTION_NONE;
    _ui_actions_internal__panel("move-panel", 0);
}

/**
 * @brief closes just the invoking panel and discards unsubmitted action values.
 * @param gui_event_t* event -> close/cancel button inside a panel
 * @return void
 */
static void _ui_actions_internal__close(gui_event_t *event)
{
    gui_node_t *node = event ? event->sender : NULL;
    while (node && !clib__strstr(node->klass, "panel hidden") && clib__strcmp(node->klass, "column panel") != 0)
    {
        node = node->parent;
    }
    if (node)
    {
        _ui_actions_internal__panel(node->id, 0);
        if (clib__strcmp(node->id, "notice-panel") != 0)
        {
            g_actions_pending.type = DESKTOP_ACTION_NONE;
            _ui_actions_internal__text("action-value", "");
        }
    }
}

/**
 * @brief closes mutually exclusive action forms without touching member or notification panels.
 * @return void
 */
static void _ui_actions_internal__close_forms(void)
{
    free(g_actions_destinations);
    g_actions_destinations = NULL;
    g_actions_destination_count = 0;
    _ui_actions_internal__panel("action-panel", 0);
    _ui_actions_internal__panel("move-panel", 0);
    _ui_actions_internal__panel("confirm-panel", 0);
    g_actions_pending.type = DESKTOP_ACTION_NONE;
}

void desktop_ui_actions__shutdown(void)
{
    free(g_actions_destinations);
    g_actions_destinations = NULL;
    g_actions_destination_count = 0;
}

/**
 * @brief refreshes action availability and displays a new poke without stealing the chat focus.
 * @return void
 */
void desktop_ui_actions__update(void)
{
    const desktop_client_t *member = _ui_actions_internal__member();
    gui_node_t *panel = scene__find_by_id("member-panel");
    gui_node_t *menu = scene__find_by_id("context-menu");
    uint64 access = member && !member->idle && !member->music && !g_actions_view->idle ? member->access : 0;
    int64 index = 0;
    uint64 now = desktop_system__milliseconds();
    char title[160] = { 0 };
    desktop_action_t action = g_actions_member;
    _ui_actions_internal__enabled("profile-rename", g_actions_view->connected && !g_actions_view->idle && g_actions_view->policy_rename);
    _ui_actions_internal__enabled("idle-button", g_actions_view->connected && (g_actions_view->policy_idle || g_actions_view->idle));
    _ui_actions_internal__toolbar_hint();
    _ui_actions_internal__enabled("member-poke", member && !member->music && !g_actions_view->idle && member->id != g_actions_view->local_id);
    _ui_actions_internal__enabled("member-move", (access & DESKTOP_ACCESS_MOVE) != 0);
    _ui_actions_internal__enabled("member-kick-channel", member && member->channel != 0 && (access & DESKTOP_ACCESS_KICK_CHANNEL));
    _ui_actions_internal__enabled("member-mute-channel", (access & DESKTOP_ACCESS_MUTE_CHANNEL) != 0);
    _ui_actions_internal__enabled("member-stop-stream", (access & DESKTOP_ACCESS_STOP_STREAM) != 0);
    if (member && !member->idle && !member->music && !g_actions_view->idle && ((panel && !clib__strstr(panel->klass, "hidden")) || (menu && !clib__strstr(menu->klass, "hidden"))) && now - g_actions_access_time >= 5000)
    {
        action.type = DESKTOP_ACTION_ACCESS;
        if (desktop_session__action(g_actions_session, &action))
        {
            g_actions_access_time = now;
        }
    }
    if (!g_actions_view->connected && g_actions_connected)
    {
        _ui_actions_internal__close_forms();
    }
    if (g_actions_view->connected && !g_actions_connected)
    {
        g_actions_notice = 0;
        g_actions_rename[0] = 0;
    }
    g_actions_connected = g_actions_view->connected;
    if (g_actions_view->notice_sequence > g_actions_notice)
    {
        g_actions_notice = g_actions_view->notice_sequence;
        clib__string_format(title, sizeof(title), "%s poked you", g_actions_view->notice_sender);
        _ui_actions_internal__text("notice-title", title);
        _ui_actions_internal__text("notice-text", g_actions_view->notice_text);
        _ui_actions_internal__panel("notice-panel", 1);
    }
    for (index = 0; g_actions_rename[0] && index < g_actions_view->client_count; index++)
    {
        if (g_actions_view->clients[index].id == g_actions_view->local_id && clib__strcmp(g_actions_view->clients[index].name, g_actions_rename) == 0)
        {
            clib__string_copy(g_actions_settings->username, sizeof(g_actions_settings->username), g_actions_rename);
            _ui_actions_internal__text("username", g_actions_rename);
            if (!desktop_settings__save(g_actions_settings))
            {
                _ui_actions_internal__text("connection-status", "Name changed, but local settings could not be saved.");
            }
            g_actions_rename[0] = 0;
        }
    }
}

/**
 * @brief registers standalone action handlers before parsing the Meek UI layout.
 * @param desktop_session_t* session -> application-owned session
 * @param const desktop_session_view_t* view -> stable GUI snapshot storage
 * @param desktop_settings_t* settings -> application-owned saved preferences
 * @return void
 */
void desktop_ui_actions__initialize(desktop_session_t *session, const desktop_session_view_t *view, desktop_settings_t *settings)
{
    g_actions_session = session;
    g_actions_view = view;
    g_actions_settings = settings;
    g_actions_member.peer = -1;
    scene__register_handler("on_profile", _ui_actions_internal__profile);
    scene__register_handler("on_idle", _ui_actions_internal__idle);
    scene__register_handler("on_member_action", _ui_actions_internal__member_action);
    scene__register_handler("on_action_submit", _ui_actions_internal__submit);
    scene__register_handler("on_action_confirm", _ui_actions_internal__confirm);
    scene__register_handler("on_action_close", _ui_actions_internal__close);
    scene__register_handler("on_action_move", _ui_actions_internal__move);
}
