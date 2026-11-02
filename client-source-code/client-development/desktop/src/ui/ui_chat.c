#include "ui/ui_chat.h"
#include "ui/ui_headers.h"
#include "ui/ui_layout.h"
#include "platform/memory.h"
#include "widget_registry.h"
#include "renderer.h"

static desktop_session_t* g_chat_session;
static const desktop_session_view_t* g_chat_view;
static uint64 g_chat_sequence;
static uint64 g_chat_connection;
static widget_vtable_t g_chat_button_original;
static widget_vtable_t g_chat_button;
static boole g_chat_button_registered;

// declarations
static const desktop_chat_t* _ui_chat_internal__message(uint64 sequence, uint64 connection);
static void _ui_chat_internal__open(gui_event_t* event);
static void _ui_chat_internal__close(gui_event_t* event);
static void _ui_chat_internal__save(gui_event_t* event);
static void _ui_chat_internal__result(const char* text);
static void _ui_chat_internal__draw(gui_node_t* node, float scale);
static void _ui_chat_internal__layout(gui_node_t* node, float x, float y, float width, float height, float scale);

/**
 * @brief resolves pencil visibility from the message's actual hover state before painting and hit testing.
 * @param gui_node_t* node -> scene-owned button
 * @param float x -> allocated x
 * @param float y -> allocated y
 * @param float width -> available width
 * @param float height -> available height
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_chat_internal__layout(gui_node_t* node, float x, float y, float width, float height, float scale)
{
    gui_node_t* message = node->parent ? node->parent->parent : NULL;
    if (g_chat_button_original.layout)
    {
        g_chat_button_original.layout(node, x, y, width, height, scale);
    }
    if (clib__strcmp(node->klass, "chat-edit-button") == 0)
    {
        // Meek currently treats visibility:visible as an unspecified style value.
        // Resolve this control explicitly while retaining its stable layout space.
        node->resolved.visibility = message && (message->state == GUI_NODE_STATE__HOVER || message->state == GUI_NODE_STATE__PRESSED)
            ? GUI_VISIBILITY__VISIBLE : GUI_VISIBILITY__HIDDEN;
    }
}

/**
 * @brief paints a compact pencil instead of a permanent text button, using the selected theme's ink.
 * @param gui_node_t* node -> scene-owned button
 * @param float scale -> physical pixels per logical unit
 * @return void
 */
static void _ui_chat_internal__draw(gui_node_t* node, float scale)
{
    gui_color_t ink = node->resolved.font_color;
    gui_color_t hover = ink;
    float x = node->bounds.x + (node->bounds.w - 26 * scale) * 0.5f;
    float y = node->bounds.y + (node->bounds.h - 26 * scale) * 0.5f;
    if (clib__strcmp(node->klass, "chat-edit-button") != 0)
    {
        if (g_chat_button_original.emit_draws)
        {
            g_chat_button_original.emit_draws(node, scale);
        }
        return;
    }
    if (node->resolved.visibility == GUI_VISIBILITY__HIDDEN)
    {
        return;
    }
    if (node->state == GUI_NODE_STATE__HOVER || node->state == GUI_NODE_STATE__PRESSED)
    {
        hover.a *= node->state == GUI_NODE_STATE__PRESSED ? 0.22f : 0.12f;
        renderer__submit_rect(node->bounds, hover, 4 * scale);
    }
    renderer__submit_line((gui_vec2_t){x + 8.5f * scale, y + 17.5f * scale},
        (gui_vec2_t){x + 17.5f * scale, y + 8.5f * scale}, 3 * scale, ink);
    renderer__submit_line((gui_vec2_t){x + 18.5f * scale, y + 7.5f * scale},
        (gui_vec2_t){x + 20 * scale, y + 6 * scale}, 3 * scale, ink);
    renderer__submit_triangle((gui_vec2_t){x + 6 * scale, y + 20 * scale},
        (gui_vec2_t){x + 7 * scale, y + 16.5f * scale}, (gui_vec2_t){x + 9.5f * scale, y + 19 * scale}, ink);
}

/**
 * @brief resolves the editor's retained message without relying on a mutable history index.
 * @param uint64 sequence -> original message sequence
 * @param uint64 connection -> original connection lifetime
 * @return const desktop_chat_t* -> borrowed snapshot entry, or NULL after eviction/reconnection
 */
static const desktop_chat_t* _ui_chat_internal__message(uint64 sequence, uint64 connection)
{
    int64 index = 0;
    if (!g_chat_view || g_chat_view->connection_serial != connection)
    {
        return NULL;
    }
    for (index = 0; index < g_chat_view->chat_count; index++)
    {
        if (g_chat_view->chats[index].sequence == sequence && g_chat_view->chats[index].connection_serial == connection)
        {
            return &g_chat_view->chats[index];
        }
    }
    return NULL;
}

/**
 * @brief displays an editor result as plain text.
 * @param const char* text -> borrowed status text
 * @return void
 */
static void _ui_chat_internal__result(const char* text)
{
    gui_node_t* node = scene__find_by_id("message-edit-result");
    if (node)
    {
        scene__set_text(node, text, clib__strlen(text));
    }
}

/**
 * @brief opens a bounded multiline editor while leaving the chat usable behind it.
 * @param gui_event_t* event -> button carrying the retained message sequence
 * @return void
 */
static void _ui_chat_internal__open(gui_event_t* event)
{
    gui_node_t* panel = scene__find_by_id("message-edit-panel");
    gui_node_t* input = scene__find_by_id("message-edit-text");
    const desktop_chat_t* chat = NULL;
    uint64 sequence = event && event->sender ? (uint64)(nint)event->sender->user_data : 0;
    if (!g_chat_view || !panel || !input || !event || !event->sender || event->mouse.button != 0
        || event->sender->resolved.visibility == GUI_VISIBILITY__HIDDEN)
    {
        return;
    }
    chat = _ui_chat_internal__message(sequence, g_chat_view->connection_serial);
    if (!desktop_session__can_edit(g_chat_view, chat) || !scene__set_text(input, chat->text, clib__strlen(chat->text)))
    {
        return;
    }
    g_chat_sequence = sequence;
    g_chat_connection = chat->connection_serial;
    _ui_chat_internal__result("Choose Save changes to send the edit. Cancel keeps the original message.");
    clib__string_copy(panel->klass, sizeof(panel->klass), "column panel");
    desktop_ui_layout__raise(panel);
    scene__set_focus(input);
    desktop_ui_chat__update();
}

/**
 * @brief discards the editor draft and clears its owned text buffer before hiding it.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_chat_internal__close(gui_event_t* event)
{
    gui_node_t* panel = scene__find_by_id("message-edit-panel");
    gui_node_t* input = scene__find_by_id("message-edit-text");
    (void)event;
    if (input)
    {
        desktop_memory__erase(input->text, input->text_capacity);
        scene__set_text(input, "", 0);
    }
    if (panel)
    {
        clib__string_copy(panel->klass, sizeof(panel->klass), "column panel hidden");
    }
    g_chat_sequence = 0;
    g_chat_connection = 0;
    scene__set_focus(scene__find_by_id("message-input"));
}

/**
 * @brief queues an explicit edit without sending a new message or consuming the main composer draft.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_chat_internal__save(gui_event_t* event)
{
    gui_node_t* input = scene__find_by_id("message-edit-text");
    (void)event;
    if (input && desktop_session__edit(g_chat_session, g_chat_sequence, g_chat_connection, input->text))
    {
        _ui_chat_internal__close(NULL);
    }
    else
    {
        _ui_chat_internal__result("Edit not queued. Enter nonempty text and check the connection, recipient and encryption keys.");
    }
}

/**
 * @brief disables Save if its original message or connection is no longer available, preserving the draft.
 * @return void
 */
void desktop_ui_chat__update(void)
{
    gui_node_t* save = scene__find_by_id("message-edit-save");
    const desktop_chat_t* chat = _ui_chat_internal__message(g_chat_sequence, g_chat_connection);
    if (save)
    {
        save->state = desktop_session__can_edit(g_chat_view, chat) ? GUI_NODE_STATE__DEFAULT : GUI_NODE_STATE__DISABLED;
    }
}

/**
 * @brief registers the message editor before scene loading.
 * @param desktop_session_t* session -> GUI-owned command target
 * @param const desktop_session_view_t* view -> lifetime-stable GUI snapshot
 * @return void
 */
void desktop_ui_chat__initialize(desktop_session_t* session, const desktop_session_view_t* view)
{
    const widget_vtable_t* original = widget_registry__get(GUI_NODE_TYPE__BUTTON);
    g_chat_session = session;
    g_chat_view = view;
    g_chat_sequence = 0;
    g_chat_connection = 0;
    if (!g_chat_button_registered && original)
    {
        g_chat_button_original = *original;
        g_chat_button = *original;
        g_chat_button.layout = _ui_chat_internal__layout;
        g_chat_button.emit_draws = _ui_chat_internal__draw;
        widget_registry__register(GUI_NODE_TYPE__BUTTON, &g_chat_button);
        g_chat_button_registered = TRUE;
    }
    scene__register_handler("on_edit_message", _ui_chat_internal__open);
    scene__register_handler("on_save_message_edit", _ui_chat_internal__save);
    scene__register_handler("on_cancel_message_edit", _ui_chat_internal__close);
}
