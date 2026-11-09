// Standalone channel tree and chat history. The game uses its own compact scene.
#include "ui/session_view.h"
#include "ui/ui_voice.h"
#include "ui/ui_attachments.h"
#include "ui/ui_headers.h"
#include "clib.h"
#include "ui/ui_tree.h"
#include "ui/ui_chat.h"
#include "ui/ui_chat_format.h"
#include "platform/memory.h"
#include <stdlib.h>

static desktop_session_view_t g_displayed;
typedef struct view_conversation_t
{
    int64 peer;
    uint64 serial;
    char name[101];
    char draft[DESKTOP_CHAT_CAPACITY];
    boole open;
} view_conversation_t;

typedef struct view_receipt_t
{
    int64 peer;
    uint64 serial;
    uint64 count;
} view_receipt_t;

static view_receipt_t g_receipts[DESKTOP_SESSION_CLIENTS];
static view_receipt_t g_next_receipts[DESKTOP_SESSION_CLIENTS];
static int64 g_receipt_count;

static view_conversation_t g_conversations[16];
static int64 g_conversation_count;
static int64 g_selected;
static boole g_changed;
static char g_channel_draft[DESKTOP_CHAT_CAPACITY];
static uint64 g_seen_chat;
static int64 g_tree_selection = -1;
static uint64 g_tree_selection_serial;
static boole g_tree_selection_member;

// declarations
static void _session_view_internal__clear(gui_node_t *parent);
static gui_node_t *_session_view_internal__node(gui_node_t *parent, gui_node_type_e type, const char *klass, const char *text);
static void _session_view_internal__tree(const desktop_session_view_t *view);
static void _session_view_internal__messages(const desktop_session_view_t *view);
static void _session_view_internal__tabs(const desktop_session_view_t *view);
static int64 _session_view_internal__conversation(int64 peer, uint64 serial, const char *name);
static void _session_view_internal__text(gui_node_t *parent, const char *text, const desktop_chat_format_t *format);

/**
 * @brief clears retained GUI message snapshots, drafts and read counts after their controls close.
 * @return void
 */
void desktop_session_view__shutdown(void)
{
    desktop_session__view_release(&g_displayed);
    desktop_memory__erase(&g_displayed, sizeof(g_displayed));
    desktop_memory__erase(g_conversations, sizeof(g_conversations));
    desktop_memory__erase(g_receipts, sizeof(g_receipts));
    desktop_memory__erase(g_next_receipts, sizeof(g_next_receipts));
    desktop_memory__erase(g_channel_draft, sizeof(g_channel_draft));
    g_receipt_count = 0;
    g_conversation_count = 0;
    g_selected = 0;
    g_changed = FALSE;
    g_seen_chat = 0;
    g_tree_selection = -1;
    g_tree_selection_serial = 0;
    g_tree_selection_member = FALSE;
}

/**
 * @brief marks a client row without opening a conversation or information window.
 * @param const desktop_client_t* client -> displayed client with its connection lifetime
 * @return void
 */
void desktop_session_view__select_member(const desktop_client_t *client)
{
    if (client)
    {
        g_tree_selection = client->id;
        g_tree_selection_serial = client->serial;
        g_tree_selection_member = TRUE;
        g_changed = TRUE;
    }
}

/**
 * @brief marks a channel row without joining it or opening its editor.
 * @param const desktop_channel_t* channel -> displayed channel
 * @return void
 */
void desktop_session_view__select_channel(const desktop_channel_t *channel)
{
    if (channel)
    {
        g_tree_selection = channel->id;
        g_tree_selection_serial = channel->revision;
        g_tree_selection_member = FALSE;
        g_changed = TRUE;
    }
}

/**
 * @brief finds or adds a bounded private conversation using a connected peer's lifetime.
 * @param int64 peer -> server client ID
 * @param uint64 serial -> connection lifetime; numeric ID reuse creates a separate conversation
 * @param const char* name -> display name copied into the tab
 * @return int64 -> one-based tab index; zero when capacity is exhausted
 */
static int64 _session_view_internal__conversation(int64 peer, uint64 serial, const char *name)
{
    int64 index = 0;
    int64 available = 16;
    for (index = 0; index < g_conversation_count; index++)
    {
        if (g_conversations[index].peer == peer && g_conversations[index].serial == serial)
        {
            clib__string_copy(g_conversations[index].name, sizeof(g_conversations[index].name), name);
            g_changed |= !g_conversations[index].open;
            g_conversations[index].open = 1;
            return index + 1;
        }
        if (!g_conversations[index].open)
        {
            available = index;
        }
    }
    if (g_conversation_count == 16 && available == 16)
    {
        return 0;
    }
    index = available < 16 ? available : g_conversation_count++;
    clib__null_memory(&g_conversations[index], sizeof(g_conversations[index]));
    g_conversations[index].open = 1;
    g_conversations[index].peer = peer;
    g_conversations[index].serial = serial;
    clib__string_copy(g_conversations[index].name, sizeof(g_conversations[index].name), name);
    g_changed = 1;
    return index + 1;
}

/**
 * @brief selects an online member's private conversation without changing the voice channel.
 * @param const desktop_client_t* client -> GUI-owned member snapshot
 * @return boole -> 1 when selected, zero when all sixteen conversation slots are open
 */
boole desktop_session_view__open(const desktop_client_t *client)
{
    int64 tab = _session_view_internal__conversation(client->id, client->serial, client->name);
    if (tab)
    {
        desktop_session_view__select(tab);
    }
    return tab != 0 && g_selected == tab;
}

/**
 * @brief changes the visible text conversation; tab zero is the current voice channel.
 * @param int64 tab -> zero-based channel/private tab index
 * @return void
 */
void desktop_session_view__select(int64 tab)
{
    gui_node_t *input = scene__find_by_id("message-input");
    if (tab >= 0 && tab <= g_conversation_count && (!tab || g_conversations[tab - 1].open))
    {
        if (input && tab != g_selected)
        {
            if (!clib__string_copy(g_selected ? g_conversations[g_selected - 1].draft : g_channel_draft, sizeof(g_channel_draft), input->text) || !scene__set_text(input, tab ? g_conversations[tab - 1].draft : g_channel_draft, clib__strlen(tab ? g_conversations[tab - 1].draft : g_channel_draft)))
            {
                return;
            }
        }
        g_selected = tab;
        g_changed = 1;
    }
}

/**
 * @brief closes the current private tab, preserving bounded history until it ages out.
 * @return void
 */
void desktop_session_view__close(void)
{
    desktop_session_view__close_tab(g_selected);
}

/**
 * @brief closes one private tab without changing the conversation being read in another tab.
 * @param int64 tab -> one-based private tab index; the channel tab cannot be closed
 * @return void
 */
void desktop_session_view__close_tab(int64 tab)
{
    if (tab > 0 && tab <= g_conversation_count && g_conversations[tab - 1].open)
    {
        if (tab == g_selected)
        {
            desktop_session_view__select(0);
            if (g_selected == tab)
            {
                return;
            }
        }
        g_conversations[tab - 1].open = FALSE;
        g_changed = TRUE;
    }
}

/**
 * @brief returns the selected private destination and its lifetime to the composer.
 * @param uint64* serial -> receives peer lifetime, or zero for channel chat
 * @return int64 -> private client ID, or -1 for current channel
 */
int64 desktop_session_view__peer(uint64 *serial)
{
    *serial = g_selected ? g_conversations[g_selected - 1].serial : 0;
    return g_selected ? g_conversations[g_selected - 1].peer : -1;
}

/**
 * @brief counts received private messages for this peer lifetime, excluding sent messages.
 * @param const desktop_client_t* client -> GUI-owned client snapshot
 * @return uint64 -> messages received since the conversation was last viewed
 */
uint64 desktop_session_view__unread(const desktop_client_t *client)
{
    int64 index = 0;
    for (index = 0; index < g_receipt_count; index++)
    {
        if (g_receipts[index].peer == client->id && g_receipts[index].serial == client->serial)
        {
            return client->direct_received > g_receipts[index].count ? client->direct_received - g_receipts[index].count : 0;
        }
    }
    return client->direct_received;
}

/**
 * @brief renders a complete bounded message, leaving line wrapping to the text widget.
 * @param gui_node_t* parent -> message row
 * @param const char* text -> bounded decoded message text
 * @return void
 */
static void _session_view_internal__text(gui_node_t *parent, const char *text, const desktop_chat_format_t *format)
{
    gui_node_t *node = _session_view_internal__node(parent, GUI_NODE_TYPE__TEXT, "chat-text", text);
    desktop_ui_chat_format__message(node, format);
}

/**
 * @brief renders channel/private tabs; incoming private messages add a tab without taking focus.
 * @param const desktop_session_view_t* view -> current GUI snapshot
 * @return void
 */
static void _session_view_internal__tabs(const desktop_session_view_t *view)
{
    gui_node_t *parent = scene__find_by_id("conversation-tabs");
    gui_node_t *button = NULL;
    gui_node_t *tab = NULL;
    int64 index = 0;
    int64 member = 0;
    uint64 unread = 0;
    const char *name = "Channel";
    char label[128] = { 0 };
    for (index = 0; index < view->channel_count; index++)
    {
        if (view->channels[index].id == view->channel)
        {
            name = view->channels[index].name;
        }
    }
    _session_view_internal__clear(parent);
    for (index = 0; index <= g_conversation_count; index++)
    {
        if (!index && !view->connected)
        {
            continue;
        }
        if (index && !g_conversations[index - 1].open)
        {
            continue;
        }
        clib__string_format(label, sizeof(label), "%s%s", index ? "@ " : "# ", index ? g_conversations[index - 1].name : name);
        unread = 0;
        if (index && index != g_selected)
        {
            for (member = 0; member < view->client_count; member++)
            {
                if (view->clients[member].id == g_conversations[index - 1].peer && view->clients[member].serial == g_conversations[index - 1].serial)
                {
                    unread = desktop_session_view__unread(&view->clients[member]);
                    break;
                }
            }
        }
        if (unread)
        {
            clib__string_format(label, sizeof(label), "@ %.80s (%llu%s)", g_conversations[index - 1].name, unread > 99 ? 99ULL : unread, unread > 99 ? "+" : "");
        }
        tab = _session_view_internal__node(parent, GUI_NODE_TYPE__DIV, g_selected == index ? "row conversation-tab selected" : "row conversation-tab", "");
        if (tab)
        {
            tab->value = (float)index;
        }
        button = _session_view_internal__node(tab, GUI_NODE_TYPE__BUTTON, "tab-label", label);
        if (button)
        {
            button->user_data = (void *)(nuint)(index + 1);
            scene__set_on_click(button, "on_choose_conversation");
        }
        if (index)
        {
            button = _session_view_internal__node(tab, GUI_NODE_TYPE__BUTTON, "tab-close", "x");
            if (button)
            {
                button->user_data = (void *)(nuint)index;
                scene__set_on_click(button, "on_close_conversation");
            }
        }
    }
    if (parent)
    {
        parent->value = (float)g_selected;
        parent->user_data = (void *)(nuint)1;
    }
}

/**
 * @brief releases only scene-owned child rows, leaving the container and composer alive.
 * @param gui_node_t* parent -> scene-owned container, or NULL
 * @return void
 */
static void _session_view_internal__clear(gui_node_t *parent)
{
    gui_node_t *child = NULL;
    gui_node_t *next = NULL;
    if (!parent)
    {
        return;
    }
    child = parent->first_child;
    parent->first_child = NULL;
    parent->last_child = NULL;
    parent->child_count = 0;
    while (child)
    {
        next = child->next_sibling;
        child->parent = NULL;
        child->next_sibling = NULL;
        scene__node_free(child);
        child = next;
    }
}

/**
 * @brief appends a bounded plain-text control without parsing user text as markup.
 * @param gui_node_t* parent -> scene-owned container, or NULL
 * @param gui_node_type_e type -> new control or protocol message type
 * @param const char* klass -> style classes copied into the control
 * @param const char* text -> borrowed NUL-terminated text; copied before returning when retained
 * @return gui_node_t* -> scene-owned control, or NULL on allocation failure
 */
static gui_node_t *_session_view_internal__node(gui_node_t *parent, gui_node_type_e type, const char *klass, const char *text)
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
        if (!scene__set_text(node, text, clib__strlen(text)))
        {
            scene__node_free(node);
            return NULL;
        }
        scene__add_child(parent, node);
    }
    return node;
}

/**
 * @brief renders all visible channels and their members independently of the overlay row limit.
 * @param const desktop_session_view_t* view -> GUI-owned session snapshot
 * @return void
 */
static void _session_view_internal__tree(const desktop_session_view_t *view)
{
    gui_node_t *container = scene__find_by_id("member-list");
    gui_node_t *button = NULL;
    gui_node_t *row = NULL;
    const desktop_channel_t *channel = NULL;
    const desktop_client_t *client = NULL;
    int64 index = 0;
    int64 member = 0;
    int64 parent_index = 0;
    int64 depth = 0;
    int64 pass = 0;
    boole idle_heading = 0;
    ubyte *visited = NULL;
    int64 parent = -1;
    const char *klass = NULL;

    desktop_ui_tree__begin(view);
    _session_view_internal__clear(container);
    if (!view->connected)
    {
        return;
    }
    visited = calloc(view->channel_count > 0 ? (size_t)view->channel_count : 1, sizeof(*visited));
    if (!visited)
    {
        _session_view_internal__node(container, GUI_NODE_TYPE__TEXT, "muted", "Not enough memory to display the channels.");
        return;
    }
    // Parent-first passes also handle out-of-order server lists without recursive traversal.
    for (pass = 0; pass < view->channel_count; pass++)
    {
        for (index = 0; index < view->channel_count; index++)
        {
            if (visited[index])
            {
                continue;
            }
            channel = &view->channels[index];
            parent = channel->parent;
            for (parent_index = 0; parent_index < view->channel_count; parent_index++)
            {
                if (view->channels[parent_index].id == parent && parent_index != index)
                {
                    break;
                }
            }
            if (parent_index < view->channel_count && !visited[parent_index] && pass + 1 < view->channel_count)
            {
                continue;
            }
            visited[index] = 1;
            depth = 0;
            while (parent >= 0 && parent != channel->id && depth < 8)
            {
                for (parent_index = 0; parent_index < view->channel_count; parent_index++)
                {
                    if (view->channels[parent_index].id == parent)
                    {
                        break;
                    }
                }
                if (parent_index == view->channel_count || view->channels[parent_index].parent == parent)
                {
                    break;
                }
                depth++;
                parent = view->channels[parent_index].parent;
            }
            button = _session_view_internal__node(container, GUI_NODE_TYPE__DIV, channel->id == view->channel && !view->idle ? "channel-row selected" : "channel-row", "");
            desktop_ui_tree__channel(button, channel, depth);
            if (button)
            {
                clib__string_format(button->id, sizeof(button->id), "tree-channel-%lld", channel->id);
                if (!g_tree_selection_member && g_tree_selection == channel->id && g_tree_selection_serial == channel->revision)
                {
                    clib__string_append(button->klass, sizeof(button->klass), " marked");
                }
                // The owner resolves this index against the same GUI snapshot on click.
                button->user_data = (void *)(nuint)(index + 1);
                scene__set_on_click(button, "on_join_channel");
            }
            for (member = 0; member < view->client_count; member++)
            {
                client = &view->clients[member];
                if (client->channel != channel->id || client->idle)
                {
                    continue;
                }
                klass = client->id == view->local_id ? (client->audio == 1 ? "member local speaking" : "member local") : (client->audio == 1 ? "member speaking" : "member");
                row = _session_view_internal__node(container, GUI_NODE_TYPE__DIV, klass, "");
                if (row)
                {
                    if (g_tree_selection_member && g_tree_selection == client->id && g_tree_selection_serial == client->serial)
                    {
                        clib__string_append(row->klass, sizeof(row->klass), " marked");
                    }
                    row->user_data = (void *)(nuint)(member + 1);
                    scene__set_on_click(row, "on_member");
                }
                desktop_ui_tree__member(row, view, member, depth + 1);
            }
        }
    }
    for (member = 0; member < view->client_count; member++)
    {
        client = &view->clients[member];
        if (!client->idle)
        {
            continue;
        }
        if (!idle_heading)
        {
            _session_view_internal__node(container, GUI_NODE_TYPE__TEXT, "section", "Idle");
            idle_heading = 1;
        }
        row = _session_view_internal__node(container, GUI_NODE_TYPE__DIV, client->id == view->local_id ? "member local" : "member", "");
        if (row)
        {
            if (g_tree_selection_member && g_tree_selection == client->id && g_tree_selection_serial == client->serial)
            {
                clib__string_append(row->klass, sizeof(row->klass), " marked");
            }
            row->user_data = (void *)(nuint)(member + 1);
            scene__set_on_click(row, "on_member");
        }
        desktop_ui_tree__member(row, view, member, 1);
    }
    if (!view->channel_count)
    {
        _session_view_internal__node(container, GUI_NODE_TYPE__TEXT, "muted", "Connect to see channels and members.");
    }
    free(visited);
}

/**
 * @brief updates the selected channel's bounded text history without touching the composer.
 * @param const desktop_session_view_t* view -> GUI-owned session snapshot
 * @return void
 */
static void _session_view_internal__messages(const desktop_session_view_t *view)
{
    gui_node_t *container = scene__find_by_id("chat-history");
    gui_node_t *row = NULL;
    gui_node_t *heading = NULL;
    gui_node_t *edit = NULL;
    const desktop_chat_t *chat = NULL;
    int64 index = 0;
    char sender[160] = { 0 };
    uint64 serial = 0;
    int64 peer = desktop_session_view__peer(&serial);
    _session_view_internal__clear(container);
    for (index = 0; index < view->chat_count; index++)
    {
        chat = &view->chats[index];
        if (peer >= 0 ? (chat->peer != peer || chat->peer_serial != serial) : (chat->peer >= 0 || chat->channel != view->channel))
        {
            continue;
        }
        row = _session_view_internal__node(container, GUI_NODE_TYPE__DIV, "column chat-message", "");
        clib__string_format(sender, sizeof(sender), "%s%s", chat->sender, chat->edit_local_id ? " (saving...)" : chat->pending && !chat->attachment_id ? " (sending...)" : chat->edited ? " (edited)" : "");
        heading = _session_view_internal__node(row, GUI_NODE_TYPE__DIV, "row chat-message-heading", "");
        _session_view_internal__node(heading, GUI_NODE_TYPE__TEXT, "chat-sender", sender);
        if (desktop_session__can_edit(view, chat))
        {
            edit = _session_view_internal__node(heading, GUI_NODE_TYPE__BUTTON, "chat-edit-button", "");
            if (edit)
            {
                edit->user_data = (void *)(nint)chat->sequence;
                clib__string_format(edit->id, sizeof(edit->id), "edit-message-%llu", chat->sequence);
                scene__set_on_click(edit, "on_edit_message");
            }
        }
        if (chat->attachment_id)
        {
            desktop_ui_attachments__append(row, chat->attachment_id);
        }
        else
        {
            _session_view_internal__text(row, chat->text, &chat->format);
        }
    }
    if (container && !container->first_child)
    {
        row = _session_view_internal__node(container, GUI_NODE_TYPE__DIV, "column chat-empty", "");
        _session_view_internal__node(row, GUI_NODE_TYPE__TEXT, "notice-title", view->connected ? "No messages yet" : "Welcome");
        _session_view_internal__node(row, GUI_NODE_TYPE__TEXT, "muted", view->connected ? "Start the conversation below. Pictures and files can be shared here too." : "Open Connections to join your server.");
    }
}

/**
 * @brief rebuilds only the standalone portions whose data changed, on the GUI thread.
 * @param const desktop_session_view_t* view -> GUI-owned session snapshot
 * @param boole force -> nonzero to rebuild after switching out of preview
 * @return void
 */
void desktop_session_view__update(const desktop_session_view_t *view, boole force)
{
    int64 index = 0;
    gui_node_t *caption = scene__find_by_id("view-caption");
    gui_node_t *input = scene__find_by_id("message-input");
    gui_node_t *send = scene__find_by_id("send-button");
    gui_node_t *picture = scene__find_by_id("picture-button");
    gui_node_t *file = scene__find_by_id("file-button");
    boole online = 0;
    boole edit_state_changed = view->keys_ready != g_displayed.keys_ready || view->idle != g_displayed.idle || view->policy_private != g_displayed.policy_private || view->connection_serial != g_displayed.connection_serial || view->client_count != g_displayed.client_count;
    for (index = 0; !edit_state_changed && index < view->client_count; index++)
    {
        edit_state_changed = view->clients[index].id != g_displayed.clients[index].id || view->clients[index].serial != g_displayed.clients[index].serial || view->clients[index].music != g_displayed.clients[index].music;
    }
    if (!g_displayed.connected && view->connected)
    {
        g_tree_selection = -1;
        g_tree_selection_serial = 0;
        g_tree_selection_member = FALSE;
        clib__null_memory(g_conversations, sizeof(g_conversations));
        g_conversation_count = 0;
        g_selected = 0;
        g_seen_chat = 0;
        clib__null_memory(g_receipts, sizeof(g_receipts));
        g_receipt_count = 0;
        clib__null_memory(g_channel_draft, sizeof(g_channel_draft));
        if (input)
        {
            scene__set_text(input, NULL, 0);
        }
        g_changed = 1;
    }
    for (index = 0; index < view->chat_count; index++)
    {
        if (view->chats[index].sequence > g_seen_chat)
        {
            g_seen_chat = view->chats[index].sequence;
            if (view->chats[index].peer >= 0)
            {
                _session_view_internal__conversation(view->chats[index].peer, view->chats[index].peer_serial, view->chats[index].peer_name);
            }
        }
    }
    clib__null_memory(g_next_receipts, sizeof(g_next_receipts));
    for (index = 0; index < view->client_count; index++)
    {
        g_next_receipts[index].peer = view->clients[index].id;
        g_next_receipts[index].serial = view->clients[index].serial;
        g_next_receipts[index].count = view->clients[index].direct_received - desktop_session_view__unread(&view->clients[index]);
        if (g_selected && view->clients[index].id == g_conversations[g_selected - 1].peer && view->clients[index].serial == g_conversations[g_selected - 1].serial)
        {
            g_next_receipts[index].count = view->clients[index].direct_received;
        }
    }
    clib__memcpy(g_receipts, g_next_receipts, sizeof(g_receipts));
    clib__null_memory(g_next_receipts, sizeof(g_next_receipts));
    g_receipt_count = view->client_count;
    force |= g_changed;
    if (force || view->connected != g_displayed.connected || view->tree_revision != g_displayed.tree_revision || view->policy_song_titles != g_displayed.policy_song_titles || view->channel != g_displayed.channel || view->local_id != g_displayed.local_id || view->client_count != g_displayed.client_count || view->channel_count != g_displayed.channel_count || clib__memcmp(view->clients, g_displayed.clients, sizeof(view->clients)) != 0 || (view->channel_count > 0 && clib__memcmp(view->channels, g_displayed.channels, view->channel_count * sizeof(*view->channels)) != 0))
    {
        _session_view_internal__tree(view);
    }
    if (force || edit_state_changed || view->connected != g_displayed.connected || view->channel != g_displayed.channel || view->chat_count != g_displayed.chat_count || view->attachments.revision != g_displayed.attachments.revision || clib__memcmp(view->chats, g_displayed.chats, sizeof(view->chats)) != 0)
    {
        _session_view_internal__messages(view);
        _session_view_internal__tabs(view);
    }
    if (g_selected)
    {
        for (index = 0; index < view->client_count; index++)
        {
            online |= view->connected && view->clients[index].id == g_conversations[g_selected - 1].peer && view->clients[index].serial == g_conversations[g_selected - 1].serial;
        }
    }
    if (caption)
    {
        scene__set_text(caption, g_selected ? (online ? "Private conversation" : "This client has disconnected.") : view->idle ? "You are idle. Choose Return to rejoin a channel." : view->keys_ready ? "Channel text chat" : "Waiting for a connection and channel keys", clib__strlen(g_selected ? (online ? "Private conversation" : "This client has disconnected.") : view->idle ? "You are idle. Choose Return to rejoin a channel." : view->keys_ready ? "Channel text chat" : "Waiting for a connection and channel keys"));
    }
    if (send)
    {
        send->state = view->connected && !view->idle && (g_selected ? online : view->keys_ready) ? GUI_NODE_STATE__DEFAULT : GUI_NODE_STATE__DISABLED;
    }
    if (picture)
    {
        picture->state = view->connected && !view->idle && view->policy_pictures && (g_selected ? online && view->policy_private : view->keys_ready) ? GUI_NODE_STATE__DEFAULT : GUI_NODE_STATE__DISABLED;
    }
    if (file)
    {
        file->state = view->connected && !view->idle && view->policy_files && (g_selected ? online && view->policy_private : view->keys_ready) ? GUI_NODE_STATE__DEFAULT : GUI_NODE_STATE__DISABLED;
    }
    g_changed = !desktop_session__view_copy(&g_displayed, view);
    desktop_ui_chat__update();
}
