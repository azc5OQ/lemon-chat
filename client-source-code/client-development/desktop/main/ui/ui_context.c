#include "ui/ui_layout.h"
#include "ui/ui_context.h"
#include "ui/ui_effects.h"
#include "ui/ui_headers.h"
#include "ui/ui_actions.h"
#include "ui/ui_tree.h"
#include "ui/ui_channels.h"
#include "ui/session_view.h"
#include "platform/memory.h"
#include "widget_registry.h"
#include "renderer.h"

enum
{
    CONTEXT_IGNORE,
    CONTEXT_MUTE_LOCAL,
    CONTEXT_VOLUME,
    CONTEXT_SERVER_GROUPS,
    CONTEXT_CHANNEL_GROUPS,
    CONTEXT_INFO,
    CONTEXT_POKE,
    CONTEXT_MOVE,
    CONTEXT_MUTE,
    CONTEXT_KICK_CHANNEL,
    CONTEXT_STOP,
    CONTEXT_KICK,
    CONTEXT_BAN,
    CONTEXT_RENAME,
    CONTEXT_AVATAR_SET,
    CONTEXT_AVATAR_DELETE,
    CONTEXT_CALL,
    CONTEXT_MUSIC_MANAGE,
    CONTEXT_MUSIC_DELETE,
    CONTEXT_MUSIC_RENAME,
    CONTEXT_CHANNEL_CREATE,
    CONTEXT_CHANNEL_DELETE,
    CONTEXT_CHANNEL_EDIT,
    CONTEXT_CHANNEL_JOIN,
    CONTEXT_SERVER_INFO,
    CONTEXT_SERVER_MODERATION,
    CONTEXT_MUSIC_CREATE,
    CONTEXT_COUNT
};

typedef struct desktop_menu_entry_t
{
    char label[128];
    int64 value;
    int64 icon;
    void *texture;
    boole enabled;
    boole checked;
} desktop_menu_entry_t;

typedef struct desktop_menu_t
{
    desktop_menu_entry_t entries[DESKTOP_TREE_GROUPS];
    int64 count;
    int64 hover;
    int64 kind;
    float x;
    float y;
    boole open;
} desktop_menu_t;

static desktop_menu_t g_menus[2];
static desktop_action_t g_context_target;
static desktop_session_t *g_context_session;
static const desktop_session_view_t *g_context_view;
static void (*g_context_info)(void);
static void (*g_context_join)(int64);
static boole g_context_is_channel;
static desktop_channel_t g_context_channel;
static widget_vtable_t g_context_original;
static widget_vtable_t g_context_widget;
static boole g_context_pending_close;
static boole g_context_changed;
static uint64 g_context_revision;
static desktop_action_t g_context_volume_target;
static int64 g_context_pointer_x;
static int64 g_context_pointer_y;

// declarations
static int64 _ui_context_internal__index(gui_node_t *node);
static const desktop_client_t *_ui_context_internal__member(void);
static boole _ui_context_internal__allowed(int64 kind, const desktop_client_t *member);
static const desktop_channel_t *_ui_context_internal__channel(void);
static void _ui_context_internal__submenu(int64 kind);
static void _ui_context_internal__activate(int64 menu, int64 row);
static void _ui_context_internal__click(gui_event_t *event);
static void _ui_context_internal__layout(gui_node_t *node, float x, float y, float width, float height, float scale);
static void _ui_context_internal__draw(gui_node_t *node, float scale);
static void _ui_context_internal__post(gui_node_t *node, float scale);
static void _ui_context_internal__down(gui_node_t *node, int64 x, int64 y, int64 button);
static void _ui_context_internal__key(gui_node_t *node, int64 key, boole down);
static void _ui_context_internal__glyph(int64 kind, gui_rect_t rect, gui_color_t color, gui_color_t background);
static float _ui_context_internal__row_height(gui_node_t *node);
static void _ui_context_internal__volume_change(gui_event_t *event);
static void _ui_context_internal__volume_save(gui_event_t *event);
static void _ui_context_internal__volume_close(gui_event_t *event);

/**
 * @brief maps only the two owned popup nodes; other divs keep their original behavior.
 * @param gui_node_t* node -> scene-owned node
 * @return int64 -> menu index or -1
 */
static int64 _ui_context_internal__index(gui_node_t *node)
{
    return !node ? -1 : clib__strcmp(node->id, "context-menu") == 0 ? 0 : clib__strcmp(node->id, "context-submenu") == 0 ? 1 : -1;
}

/**
 * @brief resolves the captured client lifetime and channel before every action.
 * @return const desktop_client_t* -> borrowed GUI snapshot or NULL when stale
 */
static const desktop_client_t *_ui_context_internal__member(void)
{
    int64 index = 0;
    for (index = 0; g_context_view->connected && index < g_context_view->client_count; index++)
    {
        if (g_context_view->clients[index].id == g_context_target.peer && g_context_view->clients[index].serial == g_context_target.serial && g_context_view->clients[index].channel == g_context_target.channel)
        {
            return &g_context_view->clients[index];
        }
    }
    return NULL;
}

/**
 * @brief keeps restricted actions visible while checking current server hints.
 * @param int64 kind -> menu action
 * @param const desktop_client_t* member -> current target
 * @return boole -> whether activation is allowed
 */
static boole _ui_context_internal__allowed(int64 kind, const desktop_client_t *member)
{
    const desktop_channel_t *channel = g_context_is_channel ? _ui_context_internal__channel() : NULL;
    boole remote = member && member->id != g_context_view->local_id;
    uint64 access = remote && !member->music && !member->idle && !g_context_view->idle && member->access_known ? member->access : 0;
    if (g_context_is_channel)
    {
        if (!channel)
        {
            return FALSE;
        }
        switch (kind)
        {
        case CONTEXT_CHANNEL_CREATE:
            return !g_context_view->idle && !channel->temporary && (g_context_view->can_create_channel || g_context_view->can_create_temp_channel);
        case CONTEXT_CHANNEL_DELETE:
            return !g_context_view->idle && channel->id != 0 && channel->access_known && (channel->access & DESKTOP_CHANNEL_DELETE);
        case CONTEXT_CHANNEL_EDIT:
            return !g_context_view->idle && channel->access_known && (channel->access & (DESKTOP_CHANNEL_DELETE - 1));
        case CONTEXT_CHANNEL_JOIN:
            return g_context_view->idle || channel->id != g_context_view->channel;
        case CONTEXT_SERVER_INFO:
            return channel->id == 0;
        default:
            return FALSE;
        }
    }
    if (!member)
    {
        return FALSE;
    }
    switch (kind)
    {
    case CONTEXT_RENAME:
        return !remote && !g_context_view->idle && g_context_view->policy_rename;
    case CONTEXT_INFO:
        return TRUE;
    case CONTEXT_IGNORE:
    case CONTEXT_VOLUME:
    case CONTEXT_MUTE_LOCAL:
        return remote;
    case CONTEXT_SERVER_GROUPS:
        return g_context_view->permission_system && !member->music;
    case CONTEXT_CHANNEL_GROUPS:
        return g_context_view->permission_system && !member->music && !member->idle;
    case CONTEXT_POKE:
        return remote && !member->music && !g_context_view->idle;
    case CONTEXT_MOVE:
        return (access & DESKTOP_ACCESS_MOVE) != 0;
    case CONTEXT_MUTE:
        return (access & DESKTOP_ACCESS_MUTE_CHANNEL) != 0;
    case CONTEXT_KICK_CHANNEL:
        return member->channel != 0 && (access & DESKTOP_ACCESS_KICK_CHANNEL);
    case CONTEXT_STOP:
        return (access & DESKTOP_ACCESS_STOP_STREAM) != 0;
    case CONTEXT_KICK:
        return desktop_session__allows(g_context_view, member, DESKTOP_ACTION_KICK_SERVER, 0);
    case CONTEXT_BAN:
        return desktop_session__allows(g_context_view, member, DESKTOP_ACTION_BAN, 0);
    default:
        return FALSE;
    }
}

/**
 * @brief clears fixed menu storage without retaining client names or borrowed textures.
 * @return void
 */
void desktop_ui_context__close(void)
{
    gui_node_t *node = scene__find_by_id("context-menu");
    gui_node_t *submenu = scene__find_by_id("context-submenu");
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), "context-menu hidden");
    }
    if (submenu)
    {
        clib__string_copy(submenu->klass, sizeof(submenu->klass), "context-menu hidden");
    }
    desktop_memory__erase(g_menus, sizeof(g_menus));
    desktop_memory__erase(&g_context_target, sizeof(g_context_target));
    desktop_memory__erase(&g_context_channel, sizeof(g_context_channel));
    g_context_is_channel = FALSE;
    g_context_pending_close = FALSE;
}

/**
 * @brief resolves a channel only while the captured catalog entry remains unchanged.
 * @return const desktop_channel_t* -> borrowed current channel or NULL
 */
static const desktop_channel_t *_ui_context_internal__channel(void)
{
    int64 index = 0;
    for (index = 0; g_context_view->connected && index < g_context_view->channel_count; index++)
    {
        if (g_context_view->channels[index].id == g_context_channel.id && g_context_view->channels[index].revision == g_context_channel.revision)
        {
            return &g_context_view->channels[index];
        }
    }
    return NULL;
}

/**
 * @brief opens channel actions without joining; unknown rights remain disabled until the reply arrives.
 * @param const desktop_channel_t* channel -> current channel
 * @param int64 x -> physical pointer x
 * @param int64 y -> physical pointer y
 * @return void
 */
void desktop_ui_context__channel(const desktop_channel_t *channel, int64 x, int64 y)
{
    const char *labels[7] = { "create", "delete", "edit", "join", "create music bot", "server info", "server moderation" };
    int64 kinds[7] = { CONTEXT_CHANNEL_CREATE, CONTEXT_CHANNEL_DELETE, CONTEXT_CHANNEL_EDIT, CONTEXT_CHANNEL_JOIN, CONTEXT_MUSIC_CREATE, CONTEXT_SERVER_INFO, CONTEXT_SERVER_MODERATION };
    gui_node_t *node = scene__find_by_id("context-menu");
    desktop_channel_request_t request = { 0 };
    int64 index = 0;
    desktop_ui_context__close();
    if (!channel || !node || !g_context_view->connected)
    {
        return;
    }
    g_context_is_channel = TRUE;
    g_context_channel = *channel;
    g_context_revision = g_context_view->tree_revision;
    g_menus[0].open = TRUE;
    g_menus[0].x = (float)x;
    g_menus[0].y = (float)y;
    g_menus[0].count = channel->id == 0 ? 7 : 5;
    g_menus[0].hover = -1;
    for (index = 0; index < g_menus[0].count; index++)
    {
        g_menus[0].entries[index].value = kinds[index];
        clib__string_copy(g_menus[0].entries[index].label, sizeof(g_menus[0].entries[index].label), labels[index]);
        g_menus[0].entries[index].enabled = _ui_context_internal__allowed(kinds[index], NULL);
    }
    request.action = DESKTOP_CHANNEL_ACCESS;
    request.channel = *channel;
    desktop_session__channel_request(g_context_session, &request);
    node->scroll_y = node->scroll_y_target = 0;
    clib__string_copy(node->klass, sizeof(node->klass), "context-menu");
    scene__set_focus(node);
    scene__pointer_position(&g_context_pointer_x, &g_context_pointer_y);
}

/**
 * @brief opens a right-click menu using the same ordering as the browser member menu.
 * @param const desktop_client_t* member -> current target, copied by identity lifetime
 * @param int64 x -> physical pointer coordinate
 * @param int64 y -> physical pointer coordinate
 * @return void
 */
void desktop_ui_context__open(const desktop_client_t *member, int64 x, int64 y)
{
    const char *labels[CONTEXT_COUNT] = { "ignore", "mute locally", "adjust volume", "server groups", "channel groups", "info", "poke", "move to channel", "mute for everyone", "kick from channel", "stop stream", "kick", "ban", "set username", "set avatar", "delete avatar", "call", "manage", "delete", "set name" };
    gui_node_t *node = scene__find_by_id("context-menu");
    int64 index = 0;
    int64 kind = 0;
    int64 local_items[6] = { CONTEXT_RENAME, CONTEXT_AVATAR_SET, CONTEXT_AVATAR_DELETE, CONTEXT_SERVER_GROUPS, CONTEXT_CHANNEL_GROUPS, CONTEXT_INFO };
    int64 idle_items[10] = { CONTEXT_CALL, CONTEXT_POKE, CONTEXT_MOVE, CONTEXT_MUTE, CONTEXT_KICK_CHANNEL, CONTEXT_STOP, CONTEXT_KICK, CONTEXT_BAN, CONTEXT_SERVER_GROUPS, CONTEXT_CHANNEL_GROUPS };
    int64 music_items[5] = { CONTEXT_MUSIC_MANAGE, CONTEXT_MUTE_LOCAL, CONTEXT_VOLUME, CONTEXT_MUSIC_DELETE, CONTEXT_MUSIC_RENAME };
    desktop_ui_context__close();
    if (!member || !node || !g_context_view->connected)
    {
        return;
    }
    g_context_target.peer = member->id;
    g_context_target.serial = member->serial;
    g_context_target.channel = member->channel;
    g_context_revision = g_context_view->tree_revision;
    g_menus[0].open = TRUE;
    g_menus[0].x = (float)x;
    g_menus[0].y = (float)y;
    g_menus[0].hover = -1;
    g_menus[0].count = member->id == g_context_view->local_id ? 6 : member->music ? 5 : member->idle ? 10 : CONTEXT_RENAME;
    for (index = 0; index < g_menus[0].count; index++)
    {
        kind = member->id == g_context_view->local_id ? local_items[index] : member->music ? music_items[index] : member->idle ? idle_items[index] : index;
        g_menus[0].entries[index].value = kind;
        clib__string_copy(g_menus[0].entries[index].label, sizeof(g_menus[0].entries[index].label), kind == CONTEXT_MUTE_LOCAL && member->muted ? "unmute locally" : kind == CONTEXT_IGNORE && member->ignored ? "stop ignoring" : labels[kind]);
        g_menus[0].entries[index].enabled = _ui_context_internal__allowed(kind, member);
    }
    node->scroll_y = node->scroll_y_target = 0;
    clib__string_copy(node->klass, sizeof(node->klass), "context-menu");
    desktop_ui_actions__select(member);
    scene__set_focus(node);
    scene__pointer_position(&g_context_pointer_x, &g_context_pointer_y);
}

/**
 * @brief fills a bounded submenu; checked channel groups are removed by choosing them again.
 * @param int64 kind -> server groups, channel groups, move, or timed mute
 * @return void
 */
static void _ui_context_internal__submenu(int64 kind)
{
    const desktop_client_t *member = _ui_context_internal__member();
    const desktop_tree_group_t *group = NULL_POINTER;
    desktop_menu_entry_t *entry = NULL_POINTER;
    desktop_menu_t *menu = &g_menus[1];
    gui_node_t *node = scene__find_by_id("context-submenu");
    gui_node_t *parent = scene__find_by_id("context-menu");
    int64 index = 0;
    int64 limit = kind == CONTEXT_SERVER_GROUPS ? DESKTOP_TREE_GROUPS : kind == CONTEXT_CHANNEL_GROUPS ? DESKTOP_TREE_CHANNEL_GROUPS : kind == CONTEXT_MOVE ? g_context_view->channel_count : 6;
    int64 durations[6] = { 0, 60, 300, 900, 3600, 86400 };
    const char *duration_names[6] = { "remove mute", "1 minute", "5 minutes", "15 minutes", "1 hour", "24 hours" };
    if (!member || !node || !parent || !_ui_context_internal__allowed(kind, member))
    {
        return;
    }
    desktop_memory__erase(menu, sizeof(*menu));
    menu->kind = kind;
    menu->hover = -1;
    menu->open = TRUE;
    menu->x = parent->bounds.x + parent->bounds.w - 1;
    for (index = 0; index < g_menus[0].count && g_menus[0].entries[index].value != kind; index++)
    {
        // Find the actual parent row; the local-client menu has a different ordering.
    }
    menu->y = parent->bounds.y + 4 * scene__scale() + (float)index * _ui_context_internal__row_height(parent) * scene__scale() - parent->scroll_y;
    for (index = 0; index < limit; index++)
    {
        group = kind == CONTEXT_SERVER_GROUPS ? &g_context_view->groups[index] : kind == CONTEXT_CHANNEL_GROUPS ? &g_context_view->channel_groups[index] : NULL;
        if (group && (!group->exists || (kind == CONTEXT_CHANNEL_GROUPS && index == 0)))
        {
            continue;
        }
        entry = &menu->entries[menu->count++];
        entry->value = index;
        entry->icon = -1;
        if (group)
        {
            entry->checked = kind == CONTEXT_CHANNEL_GROUPS ? member->channel_group == index : (member->groups[index / 64] & (1ULL << (index % 64))) != 0;
            entry->enabled = desktop_session__allows(g_context_view, member, kind == CONTEXT_CHANNEL_GROUPS ? DESKTOP_ACTION_CHANNEL_GROUP : entry->checked ? DESKTOP_ACTION_GROUP_REMOVE : DESKTOP_ACTION_GROUP_ADD, kind == CONTEXT_CHANNEL_GROUPS && entry->checked ? 0 : index);
            entry->icon = group->icon;
            entry->texture = desktop_ui_tree__prepare_icon(group->icon);
            clib__string_copy(entry->label, sizeof(entry->label), group->name);
        }
        else if (kind == CONTEXT_MOVE)
        {
            entry->value = g_context_view->channels[index].id;
            entry->enabled = entry->value != member->channel;
            clib__string_copy(entry->label, sizeof(entry->label), g_context_view->channels[index].name);
        }
        else
        {
            entry->value = durations[index];
            entry->enabled = TRUE;
            clib__string_copy(entry->label, sizeof(entry->label), duration_names[index]);
        }
    }
    if (!menu->count)
    {
        menu->count = 1;
        menu->entries[0].value = -1;
        clib__string_copy(menu->entries[0].label, sizeof(menu->entries[0].label), "No groups available");
    }
    node->scroll_y = node->scroll_y_target = 0;
    clib__string_copy(node->klass, sizeof(node->klass), "context-menu");
}

/**
 * @brief queues an action against the captured target without mutating server-owned membership locally.
 * @param int64 menu -> root or submenu index
 * @param int64 row -> bounded entry index
 * @return void
 */
static void _ui_context_internal__activate(int64 menu, int64 row)
{
    const desktop_client_t *member = _ui_context_internal__member();
    desktop_action_t action = g_context_target;
    desktop_menu_entry_t *entry = NULL_POINTER;
    gui_node_t *node = NULL_POINTER;
    int64 kind = menu ? g_menus[1].kind : row >= 0 && row < g_menus[0].count ? g_menus[0].entries[row].value : -1;
    const char *action_ids[CONTEXT_COUNT] = { "", "", "", "", "", "", "member-poke", "", "", "member-kick-channel", "member-stop-stream", "member-kick", "member-ban" };
    if (g_context_is_channel && !menu && row >= 0 && row < g_menus[0].count && _ui_context_internal__allowed(kind, NULL))
    {
        if (kind == CONTEXT_CHANNEL_JOIN && g_context_join)
        {
            g_context_join(g_context_channel.id);
        }
        else if (kind == CONTEXT_SERVER_INFO)
        {
            node = scene__find_by_id("server-info-button");
            if (node && node->state != GUI_NODE_STATE__DISABLED)
            {
                scene__dispatch_click(node, 0, 0, 0);
            }
        }
        else if (kind == CONTEXT_CHANNEL_CREATE || kind == CONTEXT_CHANNEL_DELETE || kind == CONTEXT_CHANNEL_EDIT)
        {
            desktop_ui_channels__open(g_context_channel.id, kind == CONTEXT_CHANNEL_CREATE ? DESKTOP_CHANNEL_CREATE : kind == CONTEXT_CHANNEL_DELETE ? DESKTOP_CHANNEL_REMOVE : DESKTOP_CHANNEL_EDIT);
        }
        g_context_pending_close = TRUE;
        g_context_changed = TRUE;
        return;
    }
    if (g_context_is_channel)
    {
        return;
    }
    if (!member || menu < 0 || menu > 1 || row < 0 || row >= g_menus[menu].count || !g_menus[menu].entries[row].enabled || !_ui_context_internal__allowed(kind, member))
    {
        return;
    }
    if (!menu && (kind == CONTEXT_SERVER_GROUPS || kind == CONTEXT_CHANNEL_GROUPS || kind == CONTEXT_MOVE || kind == CONTEXT_MUTE))
    {
        _ui_context_internal__submenu(kind);
        return;
    }
    if (menu)
    {
        entry = &g_menus[menu].entries[row];
        action.catalog_revision = g_context_view->tree_revision;
        action.value = entry->value;
        action.type = kind == CONTEXT_SERVER_GROUPS ? (entry->checked ? DESKTOP_ACTION_GROUP_REMOVE : DESKTOP_ACTION_GROUP_ADD) : kind == CONTEXT_CHANNEL_GROUPS ? DESKTOP_ACTION_CHANNEL_GROUP : kind == CONTEXT_MOVE ? DESKTOP_ACTION_MOVE : DESKTOP_ACTION_MUTE_CHANNEL;
        if (kind == CONTEXT_CHANNEL_GROUPS && entry->checked)
        {
            action.value = 0;
        }
        desktop_session__action(g_context_session, &action);
    }
    else if (kind == CONTEXT_RENAME)
    {
        node = scene__find_by_id("profile-rename");
        if (node)
        {
            scene__dispatch_click(node, 0, 0, 0);
        }
    }
    else if (kind == CONTEXT_IGNORE)
    {
        action.type = DESKTOP_ACTION_IGNORE;
        action.value = !member->ignored;
        desktop_session__action(g_context_session, &action);
    }
    else if (kind == CONTEXT_VOLUME)
    {
        g_context_volume_target = g_context_target;
        g_context_volume_target.type = DESKTOP_ACTION_VOLUME;
        node = scene__find_by_id("member-volume-panel");
        if (node)
        {
            clib__string_copy(node->klass, sizeof(node->klass), "column panel");
            desktop_ui_layout__raise(node);
        }
        node = scene__find_by_id("member-volume-name");
        if (node)
        {
            scene__set_text(node, member->name, clib__strlen(member->name));
        }
        node = scene__find_by_id("member-volume");
        if (node)
        {
            node->value = (float)(100 + member->volume_offset);
        }
        _ui_context_internal__volume_change(NULL);
    }
    else if (kind == CONTEXT_MUTE_LOCAL)
    {
        desktop_session__mute(g_context_session, member->id, member->serial, !member->muted);
    }
    else if (kind == CONTEXT_INFO && g_context_info)
    {
        g_context_info();
    }
    else if (kind < CONTEXT_COUNT && action_ids[kind] && action_ids[kind][0])
    {
        desktop_ui_actions__activate(action_ids[kind]);
    }
    g_context_pending_close = TRUE;
    g_context_changed = TRUE;
}

/**
 * @brief maps a visible row click to the bounded menu data, including its scroll offset.
 * @param gui_event_t* event -> pointer click supplied by Meek
 * @return void
 */
static void _ui_context_internal__click(gui_event_t *event)
{
    int64 menu = _ui_context_internal__index(event ? event->sender : NULL);
    int64 row = -1;
    if (menu >= 0 && event->mouse.button == 0)
    {
        row = (int64)(((float)event->mouse.y - event->sender->bounds.y + event->sender->scroll_y - 4 * scene__scale()) / (_ui_context_internal__row_height(event->sender) * scene__scale()));
        _ui_context_internal__activate(menu, row);
    }
}

/**
 * @brief reads shared popup row geometry from the selected stylesheet.
 * @param gui_node_t* node -> popup with resolved theme properties
 * @return float -> logical row height clamped to readable bounds
 */
static float _ui_context_internal__row_height(gui_node_t *node)
{
    return clib__fmaxf(22, clib__fminf(48, node->resolved.item_height > 0 ? node->resolved.item_height : 22));
}

/**
 * @brief uses fixed storage and draws only visible rows, even for a thousand-group server.
 * @param gui_node_t* node -> popup or ordinary div
 * @param float x -> allocated origin
 * @param float y -> allocated origin
 * @param float width -> allocated width
 * @param float height -> allocated height
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_context_internal__layout(gui_node_t *node, float x, float y, float width, float height, float scale)
{
    int64 index = _ui_context_internal__index(node);
    int64 viewport_w = 0;
    int64 viewport_h = 0;
    gui_node_t *parent = scene__find_by_id("context-menu");
    float desired_x = 0;
    if (index < 0)
    {
        g_context_original.layout(node, x, y, width, height, scale);
        return;
    }
    scene__viewport(&viewport_w, &viewport_h);
    node->bounds.w = clib__fmaxf(1, clib__fminf(clib__fmaxf(180, node->resolved.size_w) * scale, (float)viewport_w - 16 * scale));
    node->bounds.h = clib__fmaxf(1, clib__fminf((8 + _ui_context_internal__row_height(node) * g_menus[index].count) * scale, (float)viewport_h - 16 * scale));
    desired_x = g_menus[index].x;
    if (index && parent && desired_x + node->bounds.w > viewport_w - 8 * scale)
    {
        desired_x = parent->bounds.x - node->bounds.w + 1;
    }
    node->bounds.x = clib__fmaxf(8 * scale, clib__fminf(desired_x, viewport_w - node->bounds.w - 8 * scale));
    node->bounds.y = clib__fmaxf(8 * scale, clib__fminf(g_menus[index].y, viewport_h - node->bounds.h - 8 * scale));
    node->content_w = node->bounds.w;
    node->content_h = (8 + _ui_context_internal__row_height(node) * g_menus[index].count) * scale;
    node->scroll_y = node->scroll_y_target = clib__fmaxf(0, clib__fminf(node->scroll_y_target, node->content_h - node->bounds.h));
}

/**
 * @brief draws small geometric menu symbols without font-dependent icon characters.
 * @param int64 kind -> action icon
 * @param gui_rect_t rect -> icon slot
 * @param gui_color_t color -> enabled or disabled foreground
 * @return void
 */
static void _ui_context_internal__glyph(int64 kind, gui_rect_t rect, gui_color_t color, gui_color_t background)
{
    float x = rect.x;
    float y = rect.y;
    float unit = rect.w / 20;
    if (kind == CONTEXT_VOLUME)
    {
        renderer__submit_rect((gui_rect_t){ x + 2 * unit, y + 8 * unit, 4 * unit, 5 * unit }, color, unit);
        renderer__submit_triangle((gui_vec2_t){ x + 5 * unit, y + 10.5f * unit }, (gui_vec2_t){ x + 11 * unit, y + 4 * unit }, (gui_vec2_t){ x + 11 * unit, y + 17 * unit }, color);
        renderer__submit_line((gui_vec2_t){ x + 15 * unit, y + 7 * unit }, (gui_vec2_t){ x + 15 * unit, y + 14 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 18 * unit, y + 4 * unit }, (gui_vec2_t){ x + 18 * unit, y + 17 * unit }, 1.5f * unit, color);
    }
    else if (kind == CONTEXT_POKE)
    {
        renderer__submit_rect((gui_rect_t){ x + 3 * unit, y + 3 * unit, 14 * unit, 12 * unit }, color, 3 * unit);
        renderer__submit_triangle((gui_vec2_t){ x + 5 * unit, y + 13 * unit }, (gui_vec2_t){ x + 5 * unit, y + 19 * unit }, (gui_vec2_t){ x + 11 * unit, y + 13 * unit }, color);
        renderer__submit_line((gui_vec2_t){ x + 10 * unit, y + 5 * unit }, (gui_vec2_t){ x + 10 * unit, y + 9 * unit }, 1.5f * unit, background);
        renderer__submit_circle((gui_vec2_t){ x + 10 * unit, y + 12 * unit }, unit, background);
    }
    else if (kind == CONTEXT_RENAME || kind == CONTEXT_CHANNEL_EDIT || kind == CONTEXT_MUSIC_RENAME)
    {
        renderer__submit_line((gui_vec2_t){ x + 5 * unit, y + 15 * unit }, (gui_vec2_t){ x + 15 * unit, y + 5 * unit }, 3 * unit, color);
        renderer__submit_triangle((gui_vec2_t){ x + 2 * unit, y + 18 * unit }, (gui_vec2_t){ x + 3 * unit, y + 13 * unit }, (gui_vec2_t){ x + 7 * unit, y + 17 * unit }, color);
    }
    else if (kind == CONTEXT_SERVER_GROUPS || kind == CONTEXT_CHANNEL_GROUPS)
    {
        renderer__submit_circle((gui_vec2_t){ x + 7 * unit, y + 5 * unit }, 3 * unit, color);
        renderer__submit_circle((gui_vec2_t){ x + 15 * unit, y + 6 * unit }, 2.5f * unit, color);
        renderer__submit_rect((gui_rect_t){ x + 2 * unit, y + 10 * unit, 10 * unit, 7 * unit }, color, 3 * unit);
        renderer__submit_rect((gui_rect_t){ x + 13 * unit, y + 11 * unit, 5 * unit, 6 * unit }, color, 2 * unit);
    }
    else if (kind == CONTEXT_MOVE || kind == CONTEXT_KICK || kind == CONTEXT_KICK_CHANNEL || kind == CONTEXT_CHANNEL_JOIN)
    {
        renderer__submit_line((gui_vec2_t){ x + 3 * unit, y + 3 * unit }, (gui_vec2_t){ x + 3 * unit, y + 17 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 6 * unit, y + 10 * unit }, (gui_vec2_t){ x + 18 * unit, y + 10 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 13 * unit, y + 5 * unit }, (gui_vec2_t){ x + 18 * unit, y + 10 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 18 * unit, y + 10 * unit }, (gui_vec2_t){ x + 13 * unit, y + 15 * unit }, 1.5f * unit, color);
    }
    else if (kind == CONTEXT_CHANNEL_CREATE)
    {
        renderer__submit_line((gui_vec2_t){ x + 10 * unit, y + 3 * unit }, (gui_vec2_t){ x + 10 * unit, y + 17 * unit }, 2 * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 3 * unit, y + 10 * unit }, (gui_vec2_t){ x + 17 * unit, y + 10 * unit }, 2 * unit, color);
    }
    else if (kind == CONTEXT_CHANNEL_DELETE || kind == CONTEXT_AVATAR_DELETE || kind == CONTEXT_MUSIC_DELETE)
    {
        renderer__submit_line((gui_vec2_t){ x + 3 * unit, y + 5 * unit }, (gui_vec2_t){ x + 17 * unit, y + 5 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 7 * unit, y + 2 * unit }, (gui_vec2_t){ x + 13 * unit, y + 2 * unit }, 1.5f * unit, color);
        renderer__submit_rect((gui_rect_t){ x + 5 * unit, y + 7 * unit, 10 * unit, 11 * unit }, color, unit);
        renderer__submit_line((gui_vec2_t){ x + 8 * unit, y + 9 * unit }, (gui_vec2_t){ x + 8 * unit, y + 16 * unit }, unit, background);
        renderer__submit_line((gui_vec2_t){ x + 12 * unit, y + 9 * unit }, (gui_vec2_t){ x + 12 * unit, y + 16 * unit }, unit, background);
    }
    else if (kind == CONTEXT_AVATAR_SET)
    {
        renderer__submit_rect((gui_rect_t){ x + 2 * unit, y + 3 * unit, 16 * unit, 14 * unit }, color, unit);
        renderer__submit_rect((gui_rect_t){ x + 3.5f * unit, y + 4.5f * unit, 13 * unit, 11 * unit }, background, 0);
        renderer__submit_circle((gui_vec2_t){ x + 7 * unit, y + 8 * unit }, 2 * unit, color);
        renderer__submit_triangle((gui_vec2_t){ x + 5 * unit, y + 15 * unit }, (gui_vec2_t){ x + 12 * unit, y + 8 * unit }, (gui_vec2_t){ x + 17 * unit, y + 15 * unit }, color);
    }
    else if (kind == CONTEXT_MUSIC_CREATE || kind == CONTEXT_MUSIC_MANAGE)
    {
        renderer__submit_line((gui_vec2_t){ x + 7 * unit, y + 5 * unit }, (gui_vec2_t){ x + 7 * unit, y + 15 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 7 * unit, y + 5 * unit }, (gui_vec2_t){ x + 16 * unit, y + 3 * unit }, 2 * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 16 * unit, y + 3 * unit }, (gui_vec2_t){ x + 16 * unit, y + 13 * unit }, 1.5f * unit, color);
        renderer__submit_circle((gui_vec2_t){ x + 4.5f * unit, y + 15.5f * unit }, 3 * unit, color);
        renderer__submit_circle((gui_vec2_t){ x + 13.5f * unit, y + 13.5f * unit }, 3 * unit, color);
    }
    else if (kind == CONTEXT_SERVER_MODERATION)
    {
        renderer__submit_line((gui_vec2_t){ x + 2 * unit, y + 5 * unit }, (gui_vec2_t){ x + 18 * unit, y + 5 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 2 * unit, y + 10 * unit }, (gui_vec2_t){ x + 18 * unit, y + 10 * unit }, 1.5f * unit, color);
        renderer__submit_line((gui_vec2_t){ x + 2 * unit, y + 15 * unit }, (gui_vec2_t){ x + 18 * unit, y + 15 * unit }, 1.5f * unit, color);
        renderer__submit_rect((gui_rect_t){ x + 5 * unit, y + 2 * unit, 3 * unit, 6 * unit }, color, unit);
        renderer__submit_rect((gui_rect_t){ x + 13 * unit, y + 7 * unit, 3 * unit, 6 * unit }, color, unit);
        renderer__submit_rect((gui_rect_t){ x + 8 * unit, y + 12 * unit, 3 * unit, 6 * unit }, color, unit);
    }
    else if (kind == CONTEXT_STOP)
    {
        renderer__submit_rect((gui_rect_t){ x + 2 * unit, y + 3 * unit, 16 * unit, 12 * unit }, color, unit);
        renderer__submit_rect((gui_rect_t){ x + 7 * unit, y + 6 * unit, 6 * unit, 6 * unit }, background, 0);
        renderer__submit_line((gui_vec2_t){ x + 6 * unit, y + 18 * unit }, (gui_vec2_t){ x + 14 * unit, y + 18 * unit }, unit, color);
    }
    else
    {
        renderer__submit_circle((gui_vec2_t){ x + 10 * unit, y + 10 * unit }, 7 * unit, color);
        renderer__submit_circle((gui_vec2_t){ x + 10 * unit, y + 10 * unit }, 5.5f * unit, background);
        if (kind == CONTEXT_MUTE || kind == CONTEXT_MUTE_LOCAL || kind == CONTEXT_BAN || kind == CONTEXT_IGNORE)
        {
            renderer__submit_line((gui_vec2_t){ x + 5 * unit, y + 15 * unit }, (gui_vec2_t){ x + 15 * unit, y + 5 * unit }, 1.5f * unit, color);
        }
        else
        {
            renderer__submit_line((gui_vec2_t){ x + 10 * unit, y + 8 * unit }, (gui_vec2_t){ x + 10 * unit, y + 14 * unit }, 1.5f * unit, color);
            renderer__submit_circle((gui_vec2_t){ x + 10 * unit, y + 5.5f * unit }, unit, color);
        }
    }
}

/**
 * @brief paints matching main/submenu rows, checkmarks, real group textures and disabled labels.
 * @param gui_node_t* node -> popup or ordinary div
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_context_internal__draw(gui_node_t *node, float scale)
{
    int64 index = _ui_context_internal__index(node);
    int64 row = 0;
    int64 kind = 0;
    desktop_menu_t *menu = NULL_POINTER;
    desktop_menu_entry_t *entry = NULL_POINTER;
    gui_rect_t rect = { 0 };
    gui_rect_t icon = { 0 };
    gui_color_t color = { 0 };
    gui_font_t *font = NULL_POINTER;
    float text_x = 0;
    float baseline = 0;
    float row_height = _ui_context_internal__row_height(node);
    gui_color_t background = node->resolved.background_color;
    if (index < 0)
    {
        g_context_original.emit_draws(node, scale);
        return;
    }
    menu = &g_menus[index];
    font = font__at(node->resolved.font_family, node->resolved.font_size * scale);
    desktop_ui_effects__border(node, scale);
    scene__emit_default_bg(node, scale);
    renderer__push_scissor((gui_rect_t){ node->bounds.x + 2 * scale, node->bounds.y + 2 * scale, node->bounds.w - 4 * scale, node->bounds.h - 4 * scale });
    for (row = 0; row < menu->count; row++)
    {
        entry = &menu->entries[row];
        kind = entry->value;
        rect = (gui_rect_t){ node->bounds.x + 4 * scale, node->bounds.y + (4 + row * row_height) * scale - node->scroll_y, node->bounds.w - 8 * scale, row_height * scale };
        if (rect.y + rect.h <= node->bounds.y || rect.y >= node->bounds.y + node->bounds.h)
        {
            continue;
        }
        background = node->resolved.background_color;
        if (menu->hover == row && entry->enabled)
        {
            background = node->resolved.accent_color;
            renderer__submit_rect(rect, background, 0);
        }
        color = node->resolved.font_color;
        color.a *= entry->enabled ? 1 : 0.45f;
        icon = (gui_rect_t){ rect.x + 5 * scale, rect.y + (row_height - 16) * scale * 0.5f, 16 * scale, 16 * scale };
        text_x = rect.x + 28 * scale;
        if (!index)
        {
            _ui_context_internal__glyph(kind, icon, color, background);
        }
        else if (menu->kind == CONTEXT_SERVER_GROUPS || menu->kind == CONTEXT_CHANNEL_GROUPS)
        {
            if (entry->checked)
            {
                renderer__submit_line((gui_vec2_t){ icon.x, icon.y + 10 * scale }, (gui_vec2_t){ icon.x + 4 * scale, icon.y + 14 * scale }, 1.5f * scale, color);
                renderer__submit_line((gui_vec2_t){ icon.x + 4 * scale, icon.y + 14 * scale }, (gui_vec2_t){ icon.x + 12 * scale, icon.y + 5 * scale }, 1.5f * scale, color);
            }
            icon.x += 18 * scale;
            if (entry->texture)
            {
                renderer__submit_image(icon, entry->texture, (gui_color_t){ 1, 1, 1, entry->enabled ? 1 : 0.4f });
            }
            else if (entry->icon == -2)
            {
                _ui_context_internal__glyph(menu->kind, icon, color, background);
            }
            text_x += 18 * scale;
        }
        if (font)
        {
            baseline = rect.y + (rect.h - font__line_height(font)) * 0.5f + font__ascent(font);
            renderer__push_scissor((gui_rect_t){ text_x, rect.y, rect.x + rect.w - text_x - 20 * scale, rect.h });
            font__draw(font, text_x, baseline, color, entry->label);
            renderer__pop_scissor();
        }
        if (!index && !g_context_is_channel && (kind == CONTEXT_SERVER_GROUPS || kind == CONTEXT_CHANNEL_GROUPS || kind == CONTEXT_MOVE || kind == CONTEXT_MUTE))
        {
            renderer__submit_line((gui_vec2_t){ rect.x + rect.w - 13 * scale, rect.y + rect.h * 0.5f - 4 * scale }, (gui_vec2_t){ rect.x + rect.w - 9 * scale, rect.y + rect.h * 0.5f }, scale, color);
            renderer__submit_line((gui_vec2_t){ rect.x + rect.w - 9 * scale, rect.y + rect.h * 0.5f }, (gui_vec2_t){ rect.x + rect.w - 13 * scale, rect.y + rect.h * 0.5f + 4 * scale }, scale, color);
        }
    }
    if (node->content_h > node->bounds.h)
    {
        renderer__submit_rect((gui_rect_t){ node->bounds.x + node->bounds.w - 4 * scale, node->bounds.y + node->scroll_y / node->content_h * node->bounds.h, 3 * scale, node->bounds.h * node->bounds.h / node->content_h }, node->resolved.border_color, scale);
    }
    renderer__pop_scissor();
}

/**
 * @brief avoids a second scrollbar/scissor pass for custom menus.
 * @param gui_node_t* node -> scene-owned node
 * @param float scale -> display scale
 * @return void
 */
static void _ui_context_internal__post(gui_node_t *node, float scale)
{
    if (_ui_context_internal__index(node) < 0 && g_context_original.emit_draws_post)
    {
        g_context_original.emit_draws_post(node, scale);
    }
}

/**
 * @brief focuses a menu for arrow-key navigation without changing other div behavior.
 * @param gui_node_t* node -> pressed node
 * @param int64 x -> pointer x
 * @param int64 y -> pointer y
 * @param int64 button -> mouse button
 * @return void
 */
static void _ui_context_internal__down(gui_node_t *node, int64 x, int64 y, int64 button)
{
    if (_ui_context_internal__index(node) >= 0)
    {
        scene__set_focus(node);
    }
    else if (g_context_original.on_mouse_down)
    {
        g_context_original.on_mouse_down(node, x, y, button);
    }
}

/**
 * @brief supports arrows, Enter, Space and Escape using the same action validation as clicks.
 * @param gui_node_t* node -> focused menu
 * @param int64 key -> portable virtual key
 * @param boole down -> TRUE for key press
 * @return void
 */
static void _ui_context_internal__key(gui_node_t *node, int64 key, boole down)
{
    int64 index = _ui_context_internal__index(node);
    int64 attempts = 0;
    desktop_menu_t *menu = index >= 0 ? &g_menus[index] : NULL;
    gui_node_t *submenu = scene__find_by_id("context-submenu");
    if (!menu)
    {
        if (g_context_original.on_key)
        {
            g_context_original.on_key(node, key, down);
        }
        return;
    }
    if (!down)
    {
        return;
    }
    if (key == 27)
    {
        g_context_pending_close = TRUE;
    }
    else if (key == 37 && index)
    {
        menu->open = FALSE;
        clib__string_copy(node->klass, sizeof(node->klass), "context-menu hidden");
        scene__set_focus(scene__find_by_id("context-menu"));
    }
    else if (key == 38 || key == 40)
    {
        for (attempts = 0; attempts < menu->count; attempts++)
        {
            menu->hover = (menu->hover + (key == 38 ? menu->count - 1 : 1)) % menu->count;
            if (menu->entries[menu->hover].enabled)
            {
                break;
            }
        }
        node->scroll_y_target = clib__fmaxf(0, clib__fminf(node->scroll_y_target, (4 + menu->hover * _ui_context_internal__row_height(node)) * scene__scale()));
        if ((4 + (menu->hover + 1) * _ui_context_internal__row_height(node)) * scene__scale() > node->scroll_y_target + node->bounds.h)
        {
            node->scroll_y_target = (4 + (menu->hover + 1) * _ui_context_internal__row_height(node)) * scene__scale() - node->bounds.h;
        }
    }
    else if (key == 13 || key == 32 || (key == 39 && !index))
    {
        _ui_context_internal__activate(index, menu->hover);
        if (g_menus[1].open && submenu)
        {
            scene__set_focus(submenu);
        }
    }
}

/**
 * @brief refreshes permissions and hover submenus; outside clicks retain their ordinary chat action.
 * @return boole -> TRUE when the caller should redraw or refresh its conversation view
 */
boole desktop_ui_context__tick(void)
{
    const desktop_client_t *member = NULL_POINTER;
    gui_node_t *node = NULL_POINTER;
    gui_node_t *pressed = scene__pressed();
    int64 index = 0;
    int64 row = 0;
    int64 x = 0;
    int64 y = 0;
    boole changed = g_context_changed;
    boole pointer = scene__pointer_position(&x, &y);
    boole moved = pointer && (x != g_context_pointer_x || y != g_context_pointer_y);
    desktop_menu_entry_t *entry = NULL_POINTER;
    desktop_action_e action = DESKTOP_ACTION_NONE;
    boole checked = FALSE;
    boole enabled = FALSE;
    int64 kind = 0;
    g_context_changed = FALSE;
    if (!g_menus[0].open)
    {
        return changed;
    }
    member = _ui_context_internal__member();
    if (g_context_pending_close || (g_context_is_channel ? !_ui_context_internal__channel() : !member) || g_context_view->tree_revision != g_context_revision || (pressed && _ui_context_internal__index(pressed) < 0))
    {
        desktop_ui_context__close();
        return TRUE;
    }
    for (row = 0; row < g_menus[0].count; row++)
    {
        kind = g_menus[0].entries[row].value;
        changed |= g_menus[0].entries[row].enabled != _ui_context_internal__allowed(kind, member);
        g_menus[0].entries[row].enabled = _ui_context_internal__allowed(kind, member);
    }
    for (row = 0; g_menus[1].open && row < g_menus[1].count; row++)
    {
        entry = &g_menus[1].entries[row];
        if (g_menus[1].kind == CONTEXT_CHANNEL_GROUPS || g_menus[1].kind == CONTEXT_SERVER_GROUPS)
        {
            if (entry->value < 0)
            {
                continue;
            }
            checked = g_menus[1].kind == CONTEXT_CHANNEL_GROUPS ? member->channel_group == entry->value : (member->groups[entry->value / 64] & (1ULL << (entry->value % 64))) != 0;
            changed |= entry->checked != checked;
            entry->checked = checked;
            action = g_menus[1].kind == CONTEXT_CHANNEL_GROUPS ? DESKTOP_ACTION_CHANNEL_GROUP : entry->checked ? DESKTOP_ACTION_GROUP_REMOVE : DESKTOP_ACTION_GROUP_ADD;
            enabled = desktop_session__allows(g_context_view, member, action, action == DESKTOP_ACTION_CHANNEL_GROUP && entry->checked ? 0 : entry->value);
            changed |= entry->enabled != enabled;
            entry->enabled = enabled;
        }
    }
    g_context_pointer_x = x;
    g_context_pointer_y = y;
    for (index = 1; index >= 0 && moved; index--)
    {
        if (!g_menus[index].open)
        {
            continue;
        }
        node = scene__find_by_id(index ? "context-submenu" : "context-menu");
        if (!node || x < node->bounds.x || x >= node->bounds.x + node->bounds.w || y < node->bounds.y || y >= node->bounds.y + node->bounds.h)
        {
            continue;
        }
        row = (int64)(((float)y - node->bounds.y + node->scroll_y - 4 * scene__scale()) / (_ui_context_internal__row_height(node) * scene__scale()));
        if (row >= 0 && row < g_menus[index].count && g_menus[index].hover != row)
        {
            g_menus[index].hover = row;
            changed = TRUE;
            if (!index)
            {
                node = scene__find_by_id("context-submenu");
                g_menus[1].open = FALSE;
                if (node)
                {
                    clib__string_copy(node->klass, sizeof(node->klass), "context-menu hidden");
                }
                kind = g_menus[0].entries[row].value;
                if (!g_context_is_channel && (kind == CONTEXT_SERVER_GROUPS || kind == CONTEXT_CHANNEL_GROUPS || kind == CONTEXT_MOVE || kind == CONTEXT_MUTE))
                {
                    _ui_context_internal__submenu(kind);
                }
            }
        }
        break;
    }
    return changed;
}

/**
 * @brief labels the unsaved local volume draft.
 * @param gui_event_t* event -> unused slider event
 * @return void
 */
static void _ui_context_internal__volume_change(gui_event_t *event)
{
    gui_node_t *value = scene__find_by_id("member-volume");
    gui_node_t *label = scene__find_by_id("member-volume-label");
    char caption[64] = { 0 };
    (void)event;
    if (value && label)
    {
        clib__string_format(caption, sizeof(caption), "Listening volume: %lld%%", (int64)value->value);
        scene__set_text(label, caption, clib__strlen(caption));
        clib__null_memory(caption, sizeof(caption));
    }
}

/**
 * @brief discards the volume draft without applying it.
 * @param gui_event_t* event -> unused close event
 * @return void
 */
static void _ui_context_internal__volume_close(gui_event_t *event)
{
    gui_node_t *panel = scene__find_by_id("member-volume-panel");
    (void)event;
    if (panel)
    {
        clib__string_copy(panel->klass, sizeof(panel->klass), "column panel hidden");
    }
    desktop_memory__erase(&g_context_volume_target, sizeof(g_context_volume_target));
}

/**
 * @brief applies only to the captured peer lifetime and reports stale targets.
 * @param gui_event_t* event -> unused Save event
 * @return void
 */
static void _ui_context_internal__volume_save(gui_event_t *event)
{
    gui_node_t *value = scene__find_by_id("member-volume");
    gui_node_t *result = scene__find_by_id("member-volume-result");
    (void)event;
    if (!value || g_context_volume_target.type != DESKTOP_ACTION_VOLUME)
    {
        return;
    }
    g_context_volume_target.value = (int64)value->value;
    if (desktop_session__action(g_context_session, &g_context_volume_target))
    {
        _ui_context_internal__volume_close(NULL);
    }
    else if (result)
    {
        scene__set_text(result, "This client changed. Close this window and select them again.", clib__strlen("This client changed. Close this window and select them again."));
    }
}

/**
 * @brief installs the menu adapter using only public Meek UI widget hooks.
 * @param desktop_session_t* session -> borrowed session
 * @param const desktop_session_view_t* view -> stable GUI-owned snapshot
 * @param void (*show_info)(void) -> opens the existing selected-member panel
 * @param void (*join_channel)(int64) -> existing password-aware channel join
 * @return void
 */
void desktop_ui_context__initialize(desktop_session_t *session, const desktop_session_view_t *view, void (*show_info)(void), void (*join_channel)(int64))
{
    const widget_vtable_t *original = widget_registry__get(GUI_NODE_TYPE__DIV);
    g_context_session = session;
    g_context_view = view;
    g_context_info = show_info;
    g_context_join = join_channel;
    g_context_original = *original;
    g_context_widget = *original;
    g_context_widget.layout = _ui_context_internal__layout;
    g_context_widget.emit_draws = _ui_context_internal__draw;
    g_context_widget.emit_draws_post = _ui_context_internal__post;
    g_context_widget.on_mouse_down = _ui_context_internal__down;
    g_context_widget.on_key = _ui_context_internal__key;
    widget_registry__register(GUI_NODE_TYPE__DIV, &g_context_widget);
    scene__register_handler("on_context_choose", _ui_context_internal__click);
    scene__register_handler("on_context_volume_change", _ui_context_internal__volume_change);
    scene__register_handler("on_context_volume_save", _ui_context_internal__volume_save);
    scene__register_handler("on_context_volume_close", _ui_context_internal__volume_close);
}
