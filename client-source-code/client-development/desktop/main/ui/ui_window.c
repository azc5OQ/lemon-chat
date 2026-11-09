// Main-window controls: connection form, chat tabs, channel/client selection and overlay preferences.
#include "ui/ui_window.h"
#include "ui/ui_theme.h"
#include "ui/ui_effects.h"
#include "platform/system.h"
#include "platform/memory.h"
#include "ui/ui_headers.h"
#include "ui/ui_input.h"
#include "ui/ui_connection.h"
#include "ui/ui_actions.h"
#include "ui/ui_channels.h"
#include "ui/ui_bookmarks.h"
#include "ui/ui_tree.h"
#include "ui/ui_context.h"
#include "ui/ui_pointer.h"
#include "ui/ui_server.h"
#include "ui/ui_layout.h"
#include "ui/ui_audio.h"
#include "ui/ui_chat.h"
#include "ui/ui_chat_format.h"
#include "ui/ui_voice.h"
#include "ui/ui_attachments.h"
#include "session/session.h"
#include "ui/session_view.h"
#include "crypto/crypto_session.h"
#include "settings/settings.h"
#include "overlay_controller.h"
#include "ui/member_view.h"
#include "clib.h"

// These pointers belong to main.c and remain valid until shutdown. Only the window
// thread reads this copied session view or modifies these controls and preferences.
static desktop_session_t *g_session;
static desktop_session_view_t *g_view;
static desktop_overlay_controller_t *g_overlay;
static desktop_settings_t *g_settings;
static desktop_overlay_snapshot_t g_displayed;
static boole g_preview;
static boole g_refresh = TRUE;
static int64 g_join_channel = -1;
static int64 g_member = -1;
static uint64 g_member_serial;

// declarations
static void _ui_window_internal__panel(const char *id, boole visible);
static void _ui_window_internal__set_text(const char *id, const char *text);
static void _ui_window_internal__request_render(void);
static void _ui_window_internal__submit(void);
static const desktop_client_t *_ui_window_internal__member(void);
static void _ui_window_internal__member_controls(void);
static void _ui_window_internal__show_member(void);
static void _ui_window_internal__join_channel(int64 id);

// Selected clients and private conversations

/**
 * @brief finds the selected client only if its ID and connection serial still match.
 * A new connection can reuse an old ID; actions must not target that new client.
 * @return const desktop_client_t* -> GUI-owned member snapshot, or NULL after disconnect
 */
static const desktop_client_t *_ui_window_internal__member(void)
{
    int64 index = 0;
    for (index = 0; index < g_view->client_count; index++)
    {
        if (g_view->connected && g_view->clients[index].id == g_member && g_view->clients[index].serial == g_member_serial)
        {
            return &g_view->clients[index];
        }
    }
    return NULL;
}

/**
 * @brief updates the selected client's name and channel; disables Message and Mute after disconnect.
 * @return void
 */
static void _ui_window_internal__member_controls(void)
{
    const desktop_client_t *member = _ui_window_internal__member();
    gui_node_t *message = scene__find_by_id("member-message");
    gui_node_t *mute = scene__find_by_id("member-mute");
    int64 index = 0;
    char text[192] = { 0 };
    if (message)
    {
        message->state = !member || member->music || member->id == g_view->local_id ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
    }
    if (mute)
    {
        mute->state = !member || member->id == g_view->local_id ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;
    }
    _ui_window_internal__set_text("member-mute", member && member->muted ? "Unmute locally" : "Mute locally");
    if (!member)
    {
        _ui_window_internal__set_text("member-detail", "This client has disconnected.");
        return;
    }
    clib__string_format(text, sizeof(text), "%s%s", member->name, member->id == g_view->local_id ? " (you)" : "");
    _ui_window_internal__set_text("member-title", text);
    for (index = 0; index < g_view->channel_count; index++)
    {
        if (g_view->channels[index].id == member->channel)
        {
            clib__string_format(text, sizeof(text), "%s%s", member->idle ? "Idle in " : "In ", g_view->channels[index].name);
            _ui_window_internal__set_text("member-detail", text);
            break;
        }
    }
}

/**
 * @brief opens a member's private conversation on primary click or their context menu on secondary click.
 * @param gui_event_t* event -> row click carrying an index into the displayed snapshot
 * @return void
 */
static void _ui_window_internal__on_member(gui_event_t *event)
{
    nuint index = event && event->sender ? (nuint)event->sender->user_data : 0;
    if (!index || index > g_view->client_count)
    {
        return;
    }
    desktop_session_view__select_member(&g_view->clients[index - 1]);
    g_refresh = TRUE;
    if (event->mouse.button == 1)
    {
        g_member = g_view->clients[index - 1].id;
        g_member_serial = g_view->clients[index - 1].serial;
        _ui_window_internal__member_controls();
        desktop_ui_context__open(&g_view->clients[index - 1], event->mouse.x, event->mouse.y);
    }
    else if (event->mouse.button == 0 && !g_view->clients[index - 1].music && g_view->clients[index - 1].id != g_view->local_id)
    {
        if (!desktop_session_view__open(&g_view->clients[index - 1]))
        {
            _ui_window_internal__set_text("connection-status", "Close a private conversation before opening another one.");
        }
    }
}

/**
 * @brief opens information for the client selected in the channel tree.
 * @return void
 */
static void _ui_window_internal__show_member(void)
{
    _ui_window_internal__member_controls();
    _ui_window_internal__panel("member-panel", TRUE);
}

/**
 * @brief closes member actions without changing the selected conversation.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_window_internal__on_close_member(gui_event_t *event)
{
    (void)event;
    _ui_window_internal__panel("member-panel", 0);
}

/**
 * @brief opens a private text tab while leaving the current voice channel unchanged.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_window_internal__on_private_message(gui_event_t *event)
{
    const desktop_client_t *member = _ui_window_internal__member();
    (void)event;
    if (member && !member->music && member->id != g_view->local_id)
    {
        if (!desktop_session_view__open(member))
        {
            _ui_window_internal__set_text("connection-status", "Close a private conversation before opening another one.");
            return;
        }
        g_refresh = 1;
        _ui_window_internal__on_close_member(NULL);
    }
}

/**
 * @brief mutes or unmutes the selected client locally without affecting other listeners.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_window_internal__on_mute_member(gui_event_t *event)
{
    const desktop_client_t *member = _ui_window_internal__member();
    (void)event;
    if (member && member->id != g_view->local_id)
    {
        desktop_session__mute(g_session, member->id, member->serial, !member->muted);
    }
}

/**
 * @brief selects a channel/private text tab without moving the user between voice channels.
 * @param gui_event_t* event -> tab click carrying its one-based UI index
 * @return void
 */
static void _ui_window_internal__on_choose_conversation(gui_event_t *event)
{
    nuint index = event && event->sender ? (nuint)event->sender->user_data : 0;
    if (index)
    {
        desktop_session_view__select((int64)index - 1);
        g_refresh = 1;
        _ui_window_internal__request_render();
    }
}

/**
 * @brief closes the selected private tab and returns to channel text.
 * @param gui_event_t* event -> unused click event
 * @return void
 */
static void _ui_window_internal__on_close_conversation(gui_event_t *event)
{
    if (event && event->sender)
    {
        desktop_session_view__close_tab((int64)(nuint)event->sender->user_data);
    }
    else
    {
        desktop_session_view__close();
    }
    g_refresh = 1;
    _ui_window_internal__request_render();
}

// Floating windows and connection details

/**
 * @brief shows or hides one panel without covering the chat with an input-blocking background.
 *
 * @param const char* id -> control ID in the current scene
 * @param boole visible -> nonzero to show the panel, zero to hide it
 *
 * @return void
 */
static void _ui_window_internal__panel(const char *id, boole visible)
{
    gui_node_t *node = NULL_POINTER;

    node = scene__find_by_id((char *)id);
    if (node)
    {
        clib__string_copy(node->klass, sizeof(node->klass), visible ? "column panel" : "column panel hidden");
        if (visible)
        {
            desktop_ui_layout__raise(node);
        }
        _ui_window_internal__request_render();
    }
}

/**
 * @brief opens the server connection panel.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_show_connection(gui_event_t *event)
{
    (void)event;
    desktop_ui_connection__state(g_view->connected, desktop_session__busy(g_session));
    _ui_window_internal__panel("connection-panel", 1);
}

/**
 * @brief closes the server connection panel.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_close_connection(gui_event_t *event)
{
    (void)event;
    _ui_window_internal__panel("connection-panel", 0);
}

/**
 * @brief opens the local game overlay settings panel.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_show_settings(gui_event_t *event)
{
    (void)event;
    _ui_window_internal__panel("settings-panel", 1);
}

/**
 * @brief closes settings without applying unsaved values.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_close_settings(gui_event_t *event)
{
    (void)event;
    _ui_window_internal__panel("settings-panel", 0);
}

/**
 * @brief copies text into a bounded control buffer and requests a redraw only when it changes.
 *
 * @param const char* id -> control ID in the current scene
 * @param const char* text -> text buffer
 *
 * @return void
 */
static void _ui_window_internal__set_text(const char *id, const char *text)
{
    gui_node_t *node = NULL_POINTER;

    node = scene__find_by_id((char *)id);
    if (node && clib__strcmp(node->text, text) != 0)
    {
        scene__set_text(node, text, clib__strlen(text));
        _ui_window_internal__request_render();
    }
}

/**
 * @brief saves connection details and starts a connection unless the Save button was used.
 *
 * @param gui_event_t* event -> Connect or Save button event; NULL connects
 *
 * @return void
 */
static void _ui_window_internal__on_connect(gui_event_t *event)
{
    gui_node_t *username = NULL_POINTER;
    desktop_connection_t connection = { 0 };
    desktop_settings_t settings = { 0 };
    boole connect_now = !event || !event->sender || clib__strcmp(event->sender->id, "connection-save") != 0;

    (void)event;
    username = scene__find_by_id("username");
    if (!username)
    {
        return;
    }
    if (connect_now && desktop_session__busy(g_session))
    {
        _ui_window_internal__set_text("login-result", "Disconnect and wait for the current connection to finish first.");
        return;
    }
    if (!clib__string_copy(connection.identity, sizeof(connection.identity), g_settings->identity) || !clib__string_copy(connection.username, sizeof(connection.username), username->text))
    {
        _ui_window_internal__set_text("login-result", "The connection details are too long.");
        goto label_finished;
    }
    if (!desktop_ui_connection__keys(connection.server_keys, sizeof(connection.server_keys)))
    {
        goto label_finished;
    }
    if (!desktop_ui_connection__address(connection.address, sizeof(connection.address)))
    {
        _ui_window_internal__set_text("login-result", "Enter a domain name or IP address and a port from 1 to 65535.");
        goto label_finished;
    }
    if (connection.identity[0] && !desktop_ui_connection__identity_valid(connection.identity))
    {
        _ui_window_internal__set_text("login-result", "An identity must contain 199 to 512 printable characters. Leave it empty to create one.");
        goto label_finished;
    }
    connection.automatic_transport = TRUE;
    connection.key_bits = g_settings->key_bits ? g_settings->key_bits : 2048;
    if (!connection.identity[0] && !crypto_session__new_identity(connection.identity, sizeof(connection.identity)))
    {
        _ui_window_internal__set_text("login-result", "Could not create a random identity.");
        goto label_finished;
    }
    settings = *g_settings;
    clib__string_copy(settings.address, sizeof(settings.address), connection.address);
    clib__string_copy(settings.username, sizeof(settings.username), connection.username);
    clib__string_copy(settings.identity, sizeof(settings.identity), connection.identity);
    clib__string_copy(settings.server_keys, sizeof(settings.server_keys), connection.server_keys);
    settings.key_bits = connection.key_bits;
    if (!desktop_settings__save(&settings))
    {
        _ui_window_internal__set_text("login-result", "Could not save the connection details. Check the settings directory permissions.");
    }
    else
    {
        *g_settings = settings;
        _ui_window_internal__set_text("identity-string", settings.identity);
        if (!connect_now)
        {
            _ui_window_internal__set_text("login-result", "Connection details saved.");
            goto label_finished;
        }
        if (!desktop_session__connect(g_session, &connection))
        {
            _ui_window_internal__set_text("login-result", "Could not start the connection. Your saved identity is retained.");
            goto label_finished;
        }
        g_preview = 0;
        g_refresh = 1;
        _ui_window_internal__set_text("login-result", "Connecting...");
        _ui_window_internal__set_text("preview-button", "Preview channel and speakers");
        _ui_window_internal__panel("connection-panel", 0);
    }
label_finished:
    desktop_memory__erase(&connection, sizeof(connection));
    desktop_memory__erase(&settings, sizeof(settings));
}

/**
 * @brief cancels the active WebSocket connection.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_disconnect(gui_event_t *event)
{
    (void)event;
    desktop_session__disconnect(g_session);
}

// Sending messages and joining channels

/**
 * @brief queues a channel or private message and clears the input only when it is accepted.
 * @param gui_event_t* event -> GUI event, or NULL for handlers that do not use its sender
 * @return void
 */
static void _ui_window_internal__on_send_chat(gui_event_t *event)
{
    gui_node_t *input = scene__find_by_id("message-input");
    uint64 serial = 0;
    int64 peer = desktop_session_view__peer(&serial);
    (void)event;
    if (input && (peer >= 0 ? desktop_session__direct(g_session, peer, serial, input->text) : desktop_session__chat(g_session, input->text)))
    {
        _ui_window_internal__set_text("message-input", "");
    }
    else
    {
        _ui_window_internal__set_text("connection-status", "Message not queued. Check the connection, recipient and channel keys.");
    }
}

/**
 * @brief submits the composer through the same handler used by its Send button.
 * @return void
 */
static void _ui_window_internal__submit(void)
{
    _ui_window_internal__on_send_chat(NULL);
}

/**
 * @brief selects a channel row; joining and editing are explicit context-menu actions.
 * @param gui_event_t* event -> GUI event, or NULL for handlers that do not use its sender
 * @return void
 */
static void _ui_window_internal__on_join_channel(gui_event_t *event)
{
    nuint index = event && event->sender ? (nuint)event->sender->user_data : 0;
    if (!index || index > g_view->channel_count)
    {
        return;
    }
    desktop_session_view__select_channel(&g_view->channels[index - 1]);
    g_refresh = TRUE;
    if (event->mouse.button == 1)
    {
        desktop_ui_context__channel(&g_view->channels[index - 1], event->mouse.x, event->mouse.y);
        return;
    }
}

/**
 * @brief shares password-aware joining between the tree and channel context menu.
 * @param int64 id -> selected channel ID
 * @return void
 */
static void _ui_window_internal__join_channel(int64 id)
{
    const desktop_channel_t *channel = NULL_POINTER;
    desktop_action_t action = { 0 };
    int64 index = 0;
    for (index = 0; index < g_view->channel_count; index++)
    {
        if (g_view->channels[index].id == id)
        {
            channel = &g_view->channels[index];
            break;
        }
    }
    if (!channel || (!g_view->idle && channel->id == g_view->channel))
    {
        return;
    }
    if (g_view->idle)
    {
        action.type = DESKTOP_ACTION_RETURN;
        action.value = channel->id;
        desktop_session__action(g_session, &action);
        return;
    }
    if (channel->password)
    {
        g_join_channel = channel->id;
        _ui_window_internal__set_text("join-channel-name", channel->name);
        _ui_window_internal__set_text("channel-password", "");
        _ui_window_internal__panel("join-panel", 1);
    }
    else if (!desktop_session__join(g_session, channel->id, ""))
    {
        _ui_window_internal__set_text("connection-status", "Channel request could not be queued.");
    }
}

/**
 * @brief submits the password without retaining it in local settings.
 * @param gui_event_t* event -> GUI event, or NULL for handlers that do not use its sender
 * @return void
 */
static void _ui_window_internal__on_join_password(gui_event_t *event)
{
    gui_node_t *password = scene__find_by_id("channel-password");
    (void)event;
    if (password && desktop_session__join(g_session, g_join_channel, password->text))
    {
        _ui_window_internal__set_text("channel-password", "");
        _ui_window_internal__panel("join-panel", 0);
    }
}

/**
 * @brief closes the channel password panel without sending a request.
 * @param gui_event_t* event -> GUI event, or NULL for handlers that do not use its sender
 * @return void
 */
static void _ui_window_internal__on_close_join(gui_event_t *event)
{
    (void)event;
    _ui_window_internal__set_text("channel-password", "");
    _ui_window_internal__panel("join-panel", 0);
}

// Game-overlay preferences

/**
 * @brief toggles the explicitly marked sample channel and speaking members.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_overlay_preview(gui_event_t *event)
{
    (void)event;
    g_preview = !g_preview;
    g_refresh = 1;
    _ui_window_internal__set_text("preview-button", g_preview ? "Stop overlay preview" : "Preview channel and speakers");
}

/**
 * @brief saves the chosen game executable and whether to display the overlay inside that game.
 *
 * @param gui_event_t* event -> UI event; unused by this handler
 *
 * @return void
 */
static void _ui_window_internal__on_save_overlay(gui_event_t *event)
{
    gui_node_t *enabled = NULL_POINTER;
    gui_node_t *game = NULL_POINTER;
    desktop_settings_t settings = { 0 };

    (void)event;
    enabled = scene__find_by_id("overlay-enabled");
    game = scene__find_by_id("game-executable");
    if (!enabled || !game)
    {
        return;
    }
    settings = *g_settings;
    settings.overlay_enabled = enabled->value != 0;
    if (!clib__string_copy(settings.game_executable, sizeof(settings.game_executable), game->text) || (settings.overlay_enabled && !desktop_settings__valid_game(settings.game_executable)))
    {
        _ui_window_internal__set_text("settings-result", "Enter an executable name, for example game.exe.");
        return;
    }
    if (!desktop_settings__save(&settings))
    {
        _ui_window_internal__set_text("settings-result", "Could not save settings. The previous settings remain active.");
        return;
    }
    *g_settings = settings;
    desktop_overlay_controller__configure(g_overlay, g_settings);
    _ui_window_internal__set_text("settings-result", "Settings saved");
}

/**
 * @brief requests a frame on the event-driven Windows host; POSIX hosts draw each tick.
 * @return void
 */
static void _ui_window_internal__request_render(void)
{
#ifdef _WIN32
    platform_win32__request_render();
#endif
}

// Window initialization, updates and cleanup

/**
 * @brief registers window actions and loads the controls from assets/main.ui.
 * The caller owns all four objects. Keep session, view and settings until shutdown,
 * even if initialization fails. Stop actions and updates before destroying overlay;
 * UI cleanup does not use it, so main can stop game detection first.
 * @param desktop_session_t* session -> connection and command queue owned by main
 * @param desktop_session_view_t* view -> main's copy of clients, channels and messages
 * @param desktop_settings_t* settings -> preferences kept alive for the whole application
 * @param desktop_overlay_controller_t* overlay -> object that finds the chosen game and supplies its overlay data
 * @param boole preview -> display example members until a connection is requested
 * @param boole native_devices -> allow access to the computer's audio devices
 * @return boole -> TRUE when controls and their input handlers are ready
 */
// Call these functions on the window thread. The caller owns all four objects.
// Keep session, view and settings alive through shutdown. The overlay must remain
// alive while actions or updates run; main stops it before freeing UI resources.
boole desktop_ui_window__initialize(desktop_session_t *session, desktop_session_view_t *view, desktop_settings_t *settings, desktop_overlay_controller_t *overlay, boole preview, boole native_devices)
{
    g_session = session;
    g_view = view;
    g_settings = settings;
    g_overlay = overlay;
    g_preview = preview;
    g_refresh = TRUE;

    desktop_system__window_icon();
    desktop_ui_theme__fonts();
    scene__register_handler("on_show_connection", _ui_window_internal__on_show_connection);
    scene__register_handler("on_close_connection", _ui_window_internal__on_close_connection);
    scene__register_handler("on_show_settings", _ui_window_internal__on_show_settings);
    scene__register_handler("on_close_settings", _ui_window_internal__on_close_settings);
    scene__register_handler("on_connect", _ui_window_internal__on_connect);
    scene__register_handler("on_disconnect", _ui_window_internal__on_disconnect);
    scene__register_handler("on_overlay_preview", _ui_window_internal__on_overlay_preview);
    scene__register_handler("on_save_overlay", _ui_window_internal__on_save_overlay);
    scene__register_handler("on_send_chat", _ui_window_internal__on_send_chat);
    scene__register_handler("on_join_channel", _ui_window_internal__on_join_channel);
    scene__register_handler("on_join_password", _ui_window_internal__on_join_password);
    scene__register_handler("on_close_join", _ui_window_internal__on_close_join);
    scene__register_handler("on_member", _ui_window_internal__on_member);
    scene__register_handler("on_close_member", _ui_window_internal__on_close_member);
    scene__register_handler("on_private_message", _ui_window_internal__on_private_message);
    scene__register_handler("on_mute_member", _ui_window_internal__on_mute_member);
    scene__register_handler("on_choose_conversation", _ui_window_internal__on_choose_conversation);
    scene__register_handler("on_close_conversation", _ui_window_internal__on_close_conversation);
    desktop_ui_actions__initialize(g_session, g_view, g_settings);
    desktop_ui_bookmarks__initialize(g_settings);
    desktop_ui_channels__initialize(g_session, g_view);
    desktop_ui_server__initialize(g_session, g_view);
    desktop_ui_audio__initialize(g_session, g_settings, native_devices);
    desktop_ui_chat__initialize(g_session, g_view);
    desktop_ui_chat_format__initialize(g_session, g_settings);
    desktop_ui_connection__initialize(g_settings);
    desktop_ui_input__register(_ui_window_internal__submit);
    desktop_ui_layout__register();
    desktop_ui_voice__register();
    desktop_ui_attachments__initialize(g_session, g_view, g_settings);
    desktop_ui_tree__initialize(g_session, g_view);
    desktop_ui_effects__register();
    desktop_ui_context__initialize(g_session, g_view, _ui_window_internal__show_member, _ui_window_internal__join_channel);

    // Each widget extension calls the implementation registered before it. Voice
    // indicators must precede images and tree rows; pointer handling comes last
    // so right-clicks retain all of those drawing and input functions.
    if (!desktop_ui_pointer__initialize())
    {
        return FALSE;
    }

    // Register actions and widget extensions before parsing the layout that uses them.
    // Keeping the stylesheet separate allows a theme change without replacing controls.
    return desktop_ui_theme__load("") && hot_reload__watch_ui("assets/main.ui") && desktop_ui_input__limits();
}

/**
 * @brief restores connection fields and applies audio, chat and game-overlay preferences.
 * Call after loading settings and after the controls have been created. This fills
 * the connection form; it does not connect to the saved server automatically.
 * @return void
 */
void desktop_ui_window__apply_settings(void)
{
    gui_node_t *enabled = NULL_POINTER;

    if (g_settings->theme[0] && !desktop_ui_theme__load(g_settings->theme))
    {
        _ui_window_internal__set_text("chat-settings-result", "Saved theme could not be loaded; using Classic.");
    }
    desktop_ui_audio__apply();
    desktop_ui_attachments__apply();
    desktop_overlay_controller__configure(g_overlay, g_settings);
    desktop_ui_chat_format__apply();
#ifndef LEMON_GAME_OVERLAY_AVAILABLE
    _ui_window_internal__set_text("overlay-support", "Game injection is unavailable in this build. Preview channel and speakers works in this window.");
#endif
    _ui_window_internal__set_text("game-executable", g_settings->game_executable);
    desktop_ui_connection__load_address(g_settings->address[0] ? g_settings->address : "wss://127.0.0.1:3000/");
    _ui_window_internal__set_text("username", g_settings->username);
    desktop_ui_connection__apply_identity();
    if (!desktop_ui_connection__load_keys(g_settings->server_keys))
    {
        _ui_window_internal__set_text("login-result", "Saved server keys are invalid. Enter them again before connecting.");
    }
    _ui_window_internal__set_text("preview-button", g_preview ? "Stop overlay preview" : "Preview channel and speakers");
    enabled = scene__find_by_id("overlay-enabled");
    if (enabled)
    {
        enabled->value = g_settings->overlay_enabled;
    }
}

/**
 * @brief reapplies theme, picture sizes and text limits after the layout file changes.
 * @return void
 */
void desktop_ui_window__reload(void)
{
    desktop_ui_theme__apply();
    desktop_ui_attachments__apply();
    desktop_ui_chat_format__apply();
    if (!desktop_ui_input__limits())
    {
        _ui_window_internal__set_text("connection-status", "Reloaded fields exceed their text limits; drafts have been retained.");
    }
    g_refresh = TRUE;
}

/**
 * @brief updates chat, client rows, open forms and microphone controls on the window thread.
 * @param boole changed -> the copied session or attachment data changed this iteration
 * @return void
 */
void desktop_ui_window__update(boole changed)
{
    char detail[128] = { 0 };
    g_refresh |= desktop_ui_chat_format__tick();

    // Discard outdated picture textures before rebuilding rows that may use them.
    // A local click can request a refresh even when no server message has arrived.
    if (changed || g_refresh)
    {
        desktop_ui_images__refresh(&g_view->attachments);
        _ui_window_internal__set_text("connection-status", g_view->detail);
        _ui_window_internal__set_text("login-result", g_view->detail);
        desktop_ui_audio__update(g_view);
        _ui_window_internal__member_controls();
        clib__string_format(detail, sizeof(detail), "%lld members / %lld channels", g_view->client_count, g_view->channel_count);
        _ui_window_internal__set_text("received-status", detail);
        if (!g_preview)
        {
            desktop_session_view__update(g_view, g_refresh);
            _ui_window_internal__set_text("tree-caption", g_view->connected ? "Right-click a channel or client for actions." : "Connect through the Connections menu.");
            _ui_window_internal__request_render();
        }
        g_refresh = 0;
    }
    desktop_ui_connection__state(g_view->connected, desktop_session__busy(g_session));
    desktop_ui_actions__update();
    desktop_ui_channels__update();
    desktop_ui_server__update();
    desktop_ui_audio__tick();
    if (desktop_ui_context__tick())
    {
        _ui_window_internal__request_render();
    }
    if (desktop_ui_tree__tick(!g_preview))
    {
        _ui_window_internal__request_render();
    }
}

/**
 * @brief displays the current channel and overlay status, or example members in preview mode.
 * @param const desktop_overlay_snapshot_t* snapshot -> copied channel name and speaking states
 * @param const char* status -> game detection or overlay status to show in settings
 * @return void
 */
void desktop_ui_window__overlay(const desktop_overlay_snapshot_t *snapshot, const char *status)
{
    _ui_window_internal__set_text("overlay-status", status);
    if (clib__memcmp(&g_displayed, snapshot, sizeof(*snapshot)) != 0)
    {
        _ui_window_internal__set_text("channel-title", snapshot->channel);
        _ui_window_internal__set_text("channel-path", snapshot->channel);
        if (g_preview)
        {
            _ui_window_internal__set_text("view-caption", "Overlay preview - sample channel and members");
            _ui_window_internal__set_text("tree-caption", "Sample members for the overlay preview");
            desktop_member_view__update(scene__find_by_id("member-list"), snapshot);
        }
        g_displayed = *snapshot;
        _ui_window_internal__request_render();
    }
}

/**
 * @brief reports whether the user is displaying example channel members for the overlay.
 * @return boole -> TRUE for preview data, FALSE for the current server connection
 */
boole desktop_ui_window__preview(void)
{
    return g_preview;
}

/**
 * @brief releases window-owned images, conversation data and editable connection secrets.
 * The renderer must still exist while textures are released. The caller stops the
 * connection and frees the settings after these controls no longer use them.
 * @return void
 */
void desktop_ui_window__shutdown(void)
{
    desktop_ui_context__close();
    desktop_ui_actions__shutdown();
    desktop_ui_channels__shutdown();
    desktop_ui_tree__shutdown();
    desktop_ui_voice__shutdown();
    desktop_ui_attachments__shutdown();
    desktop_ui_connection__shutdown();
    desktop_session_view__shutdown();
    desktop_memory__erase(&g_displayed, sizeof(g_displayed));
    g_session = NULL;
    g_view = NULL;
    g_settings = NULL;
    g_overlay = NULL;
}
