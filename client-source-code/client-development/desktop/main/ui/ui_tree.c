#include "ui/ui_tree.h"
#include "ui/ui_voice.h"
#include "ui/session_view.h"
#include "ui/ui_headers.h"
#include "session/tree_icons.h"
#include "platform/memory.h"
#include "platform/system.h"
#include "widget_registry.h"
#include "renderer.h"

typedef struct desktop_tree_texture_t
{
    void *texture;
    boole attempted;
} desktop_tree_texture_t;

static desktop_session_t *g_tree_session;
static const desktop_session_view_t *g_tree_view;
static desktop_tree_texture_t g_tree_icons[DESKTOP_TREE_GROUPS];
static desktop_tree_texture_t g_tree_flags[26 * 26];
static widget_vtable_t g_tree_original;
static widget_vtable_t g_tree_widget;
static uint64 g_tree_revision;
static uint64 g_tree_frame;
static boole g_tree_songs;
static boole g_tree_caption;

// declarations
static gui_node_t *_ui_tree_internal__node(gui_node_t *parent, gui_node_type_e type, const char *klass, const char *text);
static void _ui_tree_internal__draw(gui_node_t *node, float scale);
static void _ui_tree_internal__layout(gui_node_t *node, float x, float y, float width, float height, float scale);
static void _ui_tree_internal__icon(gui_node_t *parent, int64 id, const char *name);
static void _ui_tree_internal__release(desktop_tree_texture_t *cache, int64 count);
static void _ui_tree_internal__badge(gui_rect_t rect, gui_color_t color, boole channel);
static gui_node_t *_ui_tree_internal__hover(gui_node_t *node);
static void _ui_tree_internal__unread(gui_event_t *event);

/**
 * @brief creates a plain-text scene-owned tree control.
 * @param gui_node_t* parent -> owning row or column
 * @param gui_node_type_e type -> node kind
 * @param const char* klass -> application stylesheet class
 * @param const char* text -> copied literal text
 * @return gui_node_t* -> scene-owned control, or NULL
 */
static gui_node_t *_ui_tree_internal__node(gui_node_t *parent, gui_node_type_e type, const char *klass, const char *text)
{
    gui_node_t *node = NULL;
    if (!parent)
    {
        return NULL;
    }
    node = scene__node_new(type);
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), klass);
        scene__set_text(node, text, clib__strlen(text));
        scene__add_child(parent, node);
    }
    return node;
}

/**
 * @brief releases GUI-thread textures before their renderer is destroyed.
 * @param desktop_tree_texture_t* cache -> application-owned cache
 * @param int64 count -> number of entries
 * @return void
 */
static void _ui_tree_internal__release(desktop_tree_texture_t *cache, int64 count)
{
    int64 index = 0;
    for (index = 0; index < count; index++)
    {
        if (cache[index].texture)
        {
            renderer__destroy_texture(cache[index].texture);
        }
    }
    desktop_memory__erase(cache, (uint64)count * sizeof(*cache));
}

/**
 * @brief supplies a small non-text channel-role badge when a catalog has no custom pixels.
 * @param gui_rect_t rect -> physical icon bounds
 * @param gui_color_t color -> theme foreground
 * @param boole channel -> TRUE for the built-in channel-admin star
 * @return void
 */
static void _ui_tree_internal__badge(gui_rect_t rect, gui_color_t color, boole channel)
{
    gui_vec2_t a = { rect.x + rect.w * 0.15f, rect.y + rect.h * 0.2f };
    gui_vec2_t b = { rect.x + rect.w * 0.85f, a.y };
    gui_vec2_t c = { rect.x + rect.w * 0.5f, rect.y + rect.h * 0.92f };
    gui_color_t light = { 1, 1, 1, 1 };
    renderer__submit_triangle(a, b, c, color);
    renderer__submit_rect((gui_rect_t){ a.x, a.y, rect.w * 0.7f, rect.h * 0.35f }, color, rect.w * 0.08f);
    if (channel)
    {
        renderer__submit_circle((gui_vec2_t){ c.x, rect.y + rect.h * 0.42f }, rect.w * 0.13f, light);
    }
    else
    {
        renderer__submit_line((gui_vec2_t){ rect.x + rect.w * 0.32f, rect.y + rect.h * 0.4f }, (gui_vec2_t){ rect.x + rect.w * 0.46f, rect.y + rect.h * 0.55f }, rect.w * 0.08f, light);
        renderer__submit_line((gui_vec2_t){ rect.x + rect.w * 0.46f, rect.y + rect.h * 0.55f }, (gui_vec2_t){ rect.x + rect.w * 0.72f, rect.y + rect.h * 0.3f }, rect.w * 0.08f, light);
    }
}

/**
 * @brief draws only prepared textures and clipped song text; painting never parses an image.
 * @param gui_node_t* node -> scene-owned tree decoration
 * @param float scale -> physical pixels per UI unit
 * @return void
 */
static void _ui_tree_internal__draw(gui_node_t *node, float scale)
{
    gui_color_t white = { 1, 1, 1, 1 };
    gui_font_t *font = NULL;
    gui_rect_t rect = node->bounds;
    int64 id = (int64)node->value;
    void *texture = NULL;
    float length = 0;
    float offset = 0;
    float travel = 0;
    float cycle = 0;
    float phase = 0;
    boole song = clib__strcmp(node->klass, "tree-song") == 0;
    if (clib__strcmp(node->klass, "tree-unread") == 0)
    {
        white = (gui_color_t){ 0.2f, 0.43f, 0.7f, 1 };
        renderer__submit_rect(rect, white, 3 * scale);
        font = font__at(node->resolved.font_family, 11 * scale);
        if (font)
        {
            length = font__measure(font, node->text);
            font__draw(font, rect.x + (rect.w - length) * 0.5f, rect.y + 14 * scale, (gui_color_t){ 1, 1, 1, 1 }, node->text);
        }
        return;
    }
    if (clib__strcmp(node->klass, "tree-spacer") == 0)
    {
        return;
    }
    if (clib__strcmp(node->klass, "tree-channel-symbol") == 0)
    {
        desktop_ui_voice__draw_channel(node);
        return;
    }
    if (song || clib__strcmp(node->klass, "tree-name") == 0)
    {
        font = font__at(node->resolved.font_family, node->resolved.font_size * scale);
        if (!font || rect.w <= 0)
        {
            return;
        }
        length = font__measure(font, node->text);
        if (song && length > rect.w)
        {
            // Pause at both ends, then slide back. The name, flag and badges never move.
            travel = (length - rect.w) / (24 * scale);
            cycle = 3 + travel * 2;
            phase = (float)(desktop_system__milliseconds() % (uint64)(cycle * 1000)) / 1000;
            offset = phase < 1.5f ? 0 : phase < 1.5f + travel ? (phase - 1.5f) * 24 * scale : phase < 3 + travel ? length - rect.w : (cycle - phase) * 24 * scale;
        }
        renderer__push_scissor(rect);
        font__draw(font, rect.x - offset, rect.y + (rect.h - font__line_height(font)) * 0.5f + font__ascent(font), node->resolved.font_color, node->text);
        renderer__pop_scissor();
        return;
    }
    if (clib__strcmp(node->klass, "tree-icon") == 0)
    {
        texture = id >= 0 && id < DESKTOP_TREE_GROUPS ? g_tree_icons[id].texture : NULL;
    }
    else if (clib__strcmp(node->klass, "tree-flag") == 0)
    {
        texture = id >= 0 && id < 26 * 26 ? g_tree_flags[id].texture : NULL;
    }
    else
    {
        g_tree_original.emit_draws(node, scale);
        return;
    }
    if (texture)
    {
        renderer__submit_image(rect, texture, white);
    }
    else if (clib__strcmp(node->klass, "tree-icon") == 0)
    {
        if (id == -2)
        {
            _ui_tree_internal__badge(rect, node->resolved.font_color, TRUE);
        }
    }
}

/**
 * @brief gives indentation an explicit width and clips tree text to its assigned flex width.
 * @param gui_node_t* node -> scene-owned label
 * @param float x -> assigned origin
 * @param float y -> assigned origin
 * @param float width -> available physical width
 * @param float height -> available physical height
 * @param float scale -> physical pixels per UI unit
 * @return void
 */
static void _ui_tree_internal__layout(gui_node_t *node, float x, float y, float width, float height, float scale)
{
    if (clib__strcmp(node->klass, "tree-spacer") == 0)
    {
        node->bounds = (gui_rect_t){ x, y, node->value * scale, 20 * scale };
        node->content_w = node->bounds.w;
        node->content_h = node->bounds.h;
        return;
    }
    if (clib__strcmp(node->klass, "tree-name") == 0 || clib__strcmp(node->klass, "tree-song") == 0)
    {
        node->bounds = (gui_rect_t){ x, y, width > 0 ? width : 0, (clib__strcmp(node->klass, "tree-song") == 0 ? 16 : 20) * scale };
        node->content_w = node->bounds.w;
        node->content_h = node->bounds.h;
        return;
    }
    g_tree_original.layout(node, x, y, width, height, scale);
}

/**
 * @brief extends the existing text hooks without replacing audio or picture drawing.
 * @param desktop_session_t* session -> borrowed lifetime-stable session
 * @param const desktop_session_view_t* view -> borrowed GUI-owned snapshot
 * @return void
 */
void desktop_ui_tree__initialize(desktop_session_t *session, const desktop_session_view_t *view)
{
    const widget_vtable_t *original = widget_registry__get(GUI_NODE_TYPE__TEXT);
    g_tree_session = session;
    g_tree_view = view;
    scene__register_handler("on_tree_unread", _ui_tree_internal__unread);
    if (original && original != &g_tree_widget)
    {
        g_tree_original = *original;
        g_tree_widget = *original;
        g_tree_widget.layout = _ui_tree_internal__layout;
        g_tree_widget.emit_draws = _ui_tree_internal__draw;
        widget_registry__register(GUI_NODE_TYPE__TEXT, &g_tree_widget);
    }
}

/**
 * @brief invalidates cached server icons when the catalog changes or a connection is replaced.
 * @param const desktop_session_view_t* view -> GUI-owned published snapshot
 * @return void
 */
void desktop_ui_tree__begin(const desktop_session_view_t *view)
{
    if (view->tree_revision != g_tree_revision)
    {
        _ui_tree_internal__release(g_tree_icons, DESKTOP_TREE_GROUPS);
        g_tree_revision = view->tree_revision;
    }
    g_tree_songs = FALSE;
}

/**
 * @brief uploads a server icon once per catalog revision on the GUI thread.
 * @param int64 id -> icon ID, or negative built-in fallback
 * @return void* -> borrowed cached texture, valid until the next catalog refresh
 */
void *desktop_ui_tree__prepare_icon(int64 id)
{
    ubyte pixels[DESKTOP_TREE_ICON_BYTES] = { 0 };
    if (id >= 0 && id < DESKTOP_TREE_GROUPS && !g_tree_icons[id].attempted)
    {
        g_tree_icons[id].attempted = TRUE;
        if (desktop_session__tree_icon(g_tree_session, id, g_tree_revision, pixels, sizeof(pixels)))
        {
            g_tree_icons[id].texture = renderer__create_texture_rgba(pixels, 32, 32);
        }
    }
    desktop_memory__erase(pixels, sizeof(pixels));
    return id >= 0 && id < DESKTOP_TREE_GROUPS ? g_tree_icons[id].texture : NULL;
}

/**
 * @brief adds a scene-owned badge after preparing its shared texture on the GUI thread.
 * @param gui_node_t* parent -> scene-owned row
 * @param int64 id -> icon ID or built-in fallback
 * @param const char* name -> bounded group name
 * @return void
 */
static void _ui_tree_internal__icon(gui_node_t *parent, int64 id, const char *name)
{
    gui_node_t *node = NULL;
    if (id == -1)
    {
        return;
    }
    desktop_ui_tree__prepare_icon(id);
    node = _ui_tree_internal__node(parent, GUI_NODE_TYPE__TEXT, "tree-icon", name);
    if (node)
    {
        node->value = (float)id;
    }
}

/**
 * @brief builds one compact member row with fixed badges and a separate scrolling song line.
 * @param gui_node_t* row -> scene-owned clickable member row
 * @param const desktop_session_view_t* view -> GUI-owned snapshot
 * @param int64 member -> client index in this snapshot
 * @param int64 depth -> channel indentation level
 * @return void
 */
void desktop_ui_tree__member(gui_node_t *row, const desktop_session_view_t *view, int64 member, int64 depth)
{
    const desktop_client_t *client = &view->clients[member];
    const desktop_tree_group_t *group = NULL;
    gui_node_t *node = NULL;
    gui_node_t *label = NULL;
    char name[128] = { 0 };
    char summary[224] = { 0 };
    char extra[24] = { 0 };
    ubyte pixels[DESKTOP_TREE_ICON_BYTES] = { 0 };
    int64 id = 0;
    int64 count = 0;
    int64 shown = 0;
    int64 flag = -1;
    uint64 unread = desktop_session_view__unread(client);
    boole song = client->song[0] && !client->idle && (view->policy_song_titles || client->channel == view->channel);
    if (!row)
    {
        return;
    }
    if (song)
    {
        clib__string_append(row->klass, sizeof(row->klass), " tree-playing");
        g_tree_songs = TRUE;
    }
    node = _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-spacer", "");
    if (node)
    {
        node->value = (float)(depth > 4 ? 32 : depth * 8);
    }
    desktop_ui_voice__append(row, client->idle ? 4 : client->audio, client->muted);
    label = _ui_tree_internal__node(row, GUI_NODE_TYPE__DIV, "tree-label", "");
    clib__string_format(name, sizeof(name), "%s%s%s", client->name, client->music ? " [bot]" : "", client->id == view->local_id ? " (you)" : "");
    node = _ui_tree_internal__node(label, GUI_NODE_TYPE__TEXT, "tree-name", name);
    if (node)
    {
        clib__string_format(node->id, sizeof(node->id), "tree-client-%lld", client->id);
    }
    if (song)
    {
        _ui_tree_internal__node(label, GUI_NODE_TYPE__TEXT, "tree-song", client->song);
    }
    if (unread && client->id != view->local_id)
    {
        clib__string_format(extra, sizeof(extra), unread > 99 ? "99+" : "%llu", unread);
        node = _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-unread", extra);
        if (node)
        {
            node->user_data = (void *)(nuint)(member + 1);
            clib__string_format(node->id, sizeof(node->id), "tree-unread-%lld", client->id);
            scene__set_on_click(node, "on_tree_unread");
        }
    }
    else
    {
        // Keep the other right-aligned indicators still when unread messages arrive.
        _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-unread-space", "");
    }
    // The lowest group ID (normally admin) stays at the same far-right coordinate.
    for (id = 0; id < DESKTOP_TREE_GROUPS; id++)
    {
        if ((client->groups[id / 64] & (1ULL << (id % 64))) && view->groups[id].exists)
        {
            count += view->groups[id].icon != -1;
            if (summary[0])
            {
                clib__string_append(summary, sizeof(summary), ", ");
            }
            if (!clib__string_append(summary, sizeof(summary) - 5, view->groups[id].name))
            {
                clib__string_append(summary, sizeof(summary), "...");
            }
        }
    }
    scene__set_text(row, summary, clib__strlen(summary));
    if (count > 3)
    {
        clib__string_format(extra, sizeof(extra), "+%lld", count - 3);
        _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-more", extra);
    }
    for (id = 0; id < DESKTOP_TREE_GROUPS; id++)
    {
        if ((client->groups[id / 64] & (1ULL << (id % 64))) && view->groups[id].exists && view->groups[id].icon != -1 && ++shown == 3)
        {
            break;
        }
    }
    id = id >= DESKTOP_TREE_GROUPS ? DESKTOP_TREE_GROUPS - 1 : id;
    for (; id >= 0; id--)
    {
        if ((client->groups[id / 64] & (1ULL << (id % 64))) && view->groups[id].exists)
        {
            _ui_tree_internal__icon(row, view->groups[id].icon, view->groups[id].name);
        }
    }
    if (row->last_child && clib__strcmp(row->last_child->klass, "tree-icon") == 0)
    {
        clib__string_format(row->last_child->id, sizeof(row->last_child->id), "tree-badges-%lld", client->id);
    }
    if (client->channel_group > 0 && client->channel_group < DESKTOP_TREE_CHANNEL_GROUPS && !client->idle)
    {
        group = &view->channel_groups[client->channel_group];
        if (group->exists)
        {
            _ui_tree_internal__icon(row, group->icon, group->name);
            if (group->icon == -1)
            {
                _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-icon-space", "");
            }
            clib__string_append(summary, sizeof(summary), summary[0] ? " / " : "");
            clib__string_append(summary, sizeof(summary), group->name);
            scene__set_text(row, summary, clib__strlen(summary));
        }
    }
    if (!group || !group->exists)
    {
        _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-icon-space", "");
    }
    if (client->country[0] >= 'a' && client->country[0] <= 'z' && client->country[1] >= 'a' && client->country[1] <= 'z')
    {
        flag = (client->country[0] - 'a') * 26 + client->country[1] - 'a';
        if (!g_tree_flags[flag].attempted)
        {
            g_tree_flags[flag].attempted = TRUE;
            if (desktop_tree_icons__flag(client->country, pixels, sizeof(pixels)))
            {
                g_tree_flags[flag].texture = renderer__create_texture_rgba(pixels, 32, 32);
            }
        }
        if (g_tree_flags[flag].texture)
        {
            node = _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-flag", client->country);
            if (node)
            {
                node->value = (float)flag;
            }
        }
    }
    if (flag < 0 || !g_tree_flags[flag].texture)
    {
        _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-icon-space", "");
    }
    desktop_memory__erase(pixels, sizeof(pixels));
}

/**
 * @brief adds channel indentation, a custom icon and a plain channel title.
 * @param gui_node_t* row -> clickable channel row
 * @param const desktop_channel_t* channel -> GUI snapshot channel
 * @param int64 depth -> channel nesting level
 * @return void
 */
void desktop_ui_tree__channel(gui_node_t *row, const desktop_channel_t *channel, int64 depth)
{
    gui_node_t *node = NULL;
    char name[160] = { 0 };
    node = _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-spacer", "");
    if (node)
    {
        node->value = (float)(depth > 4 ? 32 : depth * 8);
    }
    if (channel->icon >= 0)
    {
        _ui_tree_internal__icon(row, channel->icon, channel->name);
    }
    else
    {
        _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-channel-symbol", "");
    }
    clib__string_format(name, sizeof(name), "%s%s%s", channel->name, channel->password ? " [locked]" : "", channel->temporary ? " (temp)" : "");
    _ui_tree_internal__node(row, GUI_NODE_TYPE__TEXT, "tree-name", name);
}

/**
 * @brief finds a hovered member without modifying selection or creating a modal popup.
 * @param gui_node_t* node -> tree root
 * @return gui_node_t* -> hovered member row, or NULL
 */
static gui_node_t *_ui_tree_internal__hover(gui_node_t *node)
{
    gui_node_t *child = NULL;
    gui_node_t *found = NULL;
    if (!node)
    {
        return NULL;
    }
    if (node->state == GUI_NODE_STATE__HOVER || node->state == GUI_NODE_STATE__PRESSED)
    {
        found = node;
        while (found && clib__strncmp(found->klass, "member", 6) != 0)
        {
            found = found->parent;
        }
        if (found)
        {
            return found;
        }
    }
    for (child = node->first_child; child; child = child->next_sibling)
    {
        found = _ui_tree_internal__hover(child);
        if (found)
        {
            return found;
        }
    }
    return NULL;
}

/**
 * @brief requests bounded animation ticks and shows badge names in the tree's existing caption.
 * @param boole visible -> FALSE while the simplified overlay preview replaces the tree
 * @return boole -> TRUE when the window needs another paint
 */
boole desktop_ui_tree__tick(boole visible)
{
    uint64 now = desktop_system__milliseconds();
    gui_node_t *caption = NULL;
    gui_node_t *hovered = NULL;
    const char *text = NULL;
    boole changed = FALSE;
    if (!visible || now - g_tree_frame < 33)
    {
        return changed;
    }
    g_tree_frame = now;
    caption = scene__find_by_id("tree-caption");
    hovered = _ui_tree_internal__hover(scene__find_by_id("member-list"));
    text = hovered && hovered->text[0] ? hovered->text : "Right-click a channel or client for actions.";
    if (caption && g_tree_view->connected && (hovered || g_tree_caption))
    {
        changed |= clib__strcmp(caption->text, text) != 0;
        scene__set_text(caption, text, clib__strlen(text));
        g_tree_caption = hovered != NULL;
    }
    return changed || g_tree_songs;
}

/**
 * @brief releases every tree texture on the GUI thread before renderer shutdown.
 * @return void
 */
void desktop_ui_tree__shutdown(void)
{
    _ui_tree_internal__release(g_tree_icons, DESKTOP_TREE_GROUPS);
    _ui_tree_internal__release(g_tree_flags, 26 * 26);
    g_tree_session = NULL;
    g_tree_view = NULL;
}

/**
 * @brief routes unread badges through the same selection and context actions as their client row.
 * @param gui_event_t* event -> scene-owned click event
 * @return void
 */
static void _ui_tree_internal__unread(gui_event_t *event)
{
    if (event && event->sender)
    {
        scene__dispatch_click(event->sender->parent, event->mouse.x, event->mouse.y, event->mouse.button);
    }
}
