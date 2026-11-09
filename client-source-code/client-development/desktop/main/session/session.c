#include "clib.h"
#include "attachments/attachments_internal.h"
// A worker owns authentication, channel keys and protocol state; the GUI receives value snapshots.
#include <stdlib.h>
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "crypto/crypto_session.h"
#include "network/transport.h"
#include "network/protocol.h"
#include "platform/thread.h"
#include "platform/system.h"
#include "platform/hotkey.h"
#include "network/endpoint.h"
#include "cJSON.h"
#include "mbedtls/platform_util.h"

// declarations
static void _session_internal__worker(void *context);

/**
 * @brief finds a worker-owned client slot by its server ID.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param int64 id -> server-assigned ID to look up
 * @return int64 -> matching client slot, or -1 when absent
 */
int64 desktop_session__client(desktop_session_t *session, int64 id)
{
    int64 index = 0;
    for (index = 0; index < session->model.client_count; index++)
    {
        if (session->model.clients[index].id == id)
        {
            return (int)index;
        }
    }
    return -1;
}

/**
 * @brief finds a worker-owned channel slot by its server ID.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param int64 id -> server-assigned ID to look up
 * @return int64 -> matching channel slot, or -1 when absent
 */
int64 desktop_session__channel(desktop_session_t *session, int64 id)
{
    int64 index = 0;
    for (index = 0; index < session->model.channel_count; index++)
    {
        if (session->model.channels[index].id == id)
        {
            return (int)index;
        }
    }
    return -1;
}

/**
 * @brief publishes a complete value snapshot without exposing worker pointers to the GUI.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return void
 */
void desktop_session__publish(desktop_session_t *session)
{
    desktop_mutex__lock(session->mutex);
    session->model.revision = session->published.revision;
    if (!desktop_session__view_equal(&session->published, &session->model))
    {
        session->model.revision++;
        if (!desktop_session__view_copy(&session->published, &session->model))
        {
            session->failed = TRUE;
            clib__string_copy(session->model.detail, sizeof(session->model.detail), "Not enough memory to publish the channel list.");
            clib__string_copy(session->published.detail, sizeof(session->published.detail), session->model.detail);
            session->published.revision++;
        }
    }
    desktop_mutex__unlock(session->mutex);
}

/**
 * @brief records a visible status and optionally ends the session.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const char* text -> borrowed NUL-terminated text; copied before returning when retained
 * @param boole fatal -> nonzero to stop the session after publishing this error
 * @return void
 */
void desktop_session__detail(desktop_session_t *session, const char *text, boole fatal)
{
    clib__string_copy(session->model.detail, sizeof(session->model.detail), text);
    session->failed |= fatal;
    desktop_session__publish(session);
}

/**
 * @brief checks cancellation under the same lock used by the GUI.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return boole -> nonzero when cancellation or failure requires stopping
 */
boole desktop_session__stopping(desktop_session_t *session)
{
    boole stop = 0;
    desktop_mutex__lock(session->mutex);
    stop = session->stop;
    desktop_mutex__unlock(session->mutex);
    return stop || session->failed;
}

/**
 * @brief forgets channel keys and authority when the local user changes channel.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return void
 */
void desktop_session__keys_reset(desktop_session_t *session)
{
    desktop_voice__channel_changed(session->voice);
    mbedtls_platform_zeroize(&session->keys, sizeof(session->keys));
    mbedtls_platform_zeroize(&session->previous_keys, sizeof(session->previous_keys));
    session->have_previous = 0;
    session->model.keys_ready = 0;
    session->maintainer = -1;
    session->generation = -1;
    session->key_channel = session->model.channel;
    session->rotate = 0;
    session->key_recipient_count = 0;
    session->waiting_since = desktop_system__milliseconds();
}

/**
 * @brief performs identity login and processes authenticated state without running crypto on the GUI thread.
 * @param void* context -> session retained by its owner until the worker has joined
 * @return void
 */
static void _session_internal__worker(void *context)
{
    desktop_session_t *session = context;
    desktop_transport_status_t status = { 0 };
    desktop_message_t frame = { 0 };
    cJSON *root = NULL_POINTER;
    const cJSON *incoming = NULL_POINTER;
    const char *type = NULL_POINTER;
    char *plaintext = NULL_POINTER;
    char mix[DESKTOP_DH_TEXT_CAPACITY] = { 0 };
    uint64 deadline = 0;
    uint64 keepalive = 0;
    uint64 published = 0;
    int64 phase = 0;
    int64 batch = 0;
    boole fallback = session->connection.automatic_transport && clib__strncmp(session->connection.address, "wss://", 6) == 0;
    char fallback_address[sizeof(session->connection.address)] = { 0 };

    desktop_session__view_release(&session->model);
    clib__null_memory(&session->model, sizeof(session->model));
    session->model.connection_serial = ++session->next_connection_serial;
    desktop_mutex__lock(session->mutex);
    clib__null_memory(session->tree_icons, sizeof(session->tree_icons));
    clib__null_memory(session->tree_icon_present, sizeof(session->tree_icon_present));
    session->model.tree_revision = ++session->next_tree_revision;
    session->tree_icons_revision = session->model.tree_revision;
    desktop_mutex__unlock(session->mutex);
    session->model.local_id = -1;
    session->model.channel = -1;
    session->model.policy_idle = 1;
    session->model.policy_rename = 1;
    session->model.policy_pictures = TRUE;
    session->model.policy_files = TRUE;
    session->model.policy_private = TRUE;
    session->model.picture_limit = DESKTOP_ATTACHMENT_IMAGE_LIMIT;
    session->model.file_limit = 10ULL * 1024 * 1024;
    desktop_session__keys_reset(session);
    desktop_session__detail(session, "Calculating your identity...", 0);
    session->crypto = crypto_session__create();
    if (!session->crypto)
    {
        desktop_session__detail(session, "Could not initialize the secure random source.", 1);
        goto label_finished;
    }
    if (!crypto_session__identity(session->crypto, session->connection.identity, session->connection.key_bits, session->public_key, sizeof(session->public_key)))
    {
        desktop_session__detail(session, "Could not calculate and validate the identity key.", 1);
        goto label_finished;
    }
    if (!crypto_session__server_keys(session->crypto, session->connection.server_keys))
    {
        desktop_session__detail(session, "Invalid server keys: use up to eight nonempty key rows.", 1);
        goto label_finished;
    }
    if (!crypto_session__begin_dh(session->crypto, mix, sizeof(mix)))
    {
        desktop_session__detail(session, "Could not prepare the ephemeral session key exchange.", 1);
        goto label_finished;
    }
    if (desktop_session__stopping(session))
    {
        goto label_finished;
    }
    if (!desktop_transport__connect(session->transport, session->connection.address))
    {
        desktop_session__detail(session, "Could not start the WebSocket connection.", 1);
        goto label_finished;
    }
    desktop_session__detail(session, fallback ? "Connecting securely..." : "Connecting...", 0);
    deadline = desktop_system__milliseconds() + 30000;
    while (!desktop_session__stopping(session))
    {
        desktop_transport__status(session->transport, &status);
        if (status.state == DESKTOP_TRANSPORT_FAILED || status.state == DESKTOP_TRANSPORT_DISCONNECTED)
        {
            if (phase == 0 && fallback)
            {
                if (!status.finished)
                {
                    desktop_system__sleep(5);
                    continue;
                }
                fallback = FALSE;
                session->model.tls_certificate_rejected = status.certificate_rejected;
                if (clib__string_format(fallback_address, sizeof(fallback_address), "ws://%s", session->connection.address + 6) && !desktop_session__stopping(session) && desktop_transport__connect(session->transport, fallback_address))
                {
                    session->model.ws_fallback = TRUE;
                    clib__string_copy(session->connection.address, sizeof(session->connection.address), fallback_address);
                    desktop_session__detail(session, status.certificate_rejected ? "TLS certificate verification failed; trying WS without TLS server verification..." : "Secure connection failed; trying WS without TLS protection...", 0);
                    deadline = desktop_system__milliseconds() + 30000;
                    clib__null_memory(fallback_address, sizeof(fallback_address));
                    continue;
                }
                clib__null_memory(fallback_address, sizeof(fallback_address));
            }
            if (status.close_code == DESKTOP_SERVER_CLOSE_KICKED)
            {
                desktop_session__detail(session, "You were kicked from the server.", 1);
            }
            else if (status.close_code == DESKTOP_SERVER_CLOSE_BANNED)
            {
                desktop_session__detail(session, "You were banned from the server.", 1);
            }
            else
            {
                desktop_session__detail(session, status.detail, 1);
            }
            break;
        }
        if (status.state == DESKTOP_TRANSPORT_CONNECTED && phase == 0)
        {
            if (!client_msg__connection__send_public_key(session, mix))
            {
                break;
            }
            phase = 1;
            desktop_session__detail(session, "Verifying identity...", 0);
        }
        for (batch = 0; batch < 16 && desktop_transport__receive(session->transport, &frame); batch++)
        {
            if (frame.binary)
            {
                desktop_message__release(&frame);
                continue;
            }
            plaintext = crypto_session__decrypt(session->crypto, frame.data, frame.size);
            root = plaintext ? desktop_protocol__parse_json(plaintext, clib__strlen(plaintext), DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT) : NULL;
            incoming = cJSON_GetObjectItemCaseSensitive(root, "message");
            type = server_msg__read_string(incoming, "type");
            if (phase == 1 && clib__strcmp(type, "crypto_policy") == 0)
            {
                cJSON *request = crypto_session__negotiate(session->crypto, incoming, session->public_key);
                if (!request || !client_msg__delivery__send(session, request, 1))
                {
                    desktop_session__detail(session, "The server encryption policy is invalid or unsupported.", 1);
                }
            }
            else if (phase == 1 && clib__strcmp(type, "public_key_challenge") == 0)
            {
                server_msg__connection__process_challenge(session, incoming);
                phase = 2;
            }
            else if (phase == 2 && clib__strcmp(type, "authentication_status") == 0)
            {
                if (clib__strcmp(server_msg__read_string(incoming, "value"), "success") == 0)
                {
                    phase = 3;
                    session->model.connected = 1;
                    desktop_voice__sound(session->voice, DESKTOP_SOUND_CONNECTED);
                    if (!desktop_attachments__start(session->attachments, session->crypto))
                    {
                        desktop_session__detail(session, "Could not initialize attachment encryption.", TRUE);
                    }
                    desktop_voice__message(session, incoming);
                    server_msg__actions__receive(session, incoming);
                    server_msg__channel__read_key_policy(session, incoming);
                    server_msg__tree__receive(session, incoming);
                    client_msg__delivery__send(session, client_msg__delivery__new_message("load_server_info"), 1);
                    keepalive = desktop_system__milliseconds();
                    desktop_session__detail(session, "Connected. Waiting for channel keys...", 0);
                }
                else
                {
                    desktop_session__detail(session, "The server refused this identity.", 1);
                }
            }
            else if (phase == 3 && incoming)
            {
                server_msg__dispatch(session, incoming);
            }
            else if (!root)
            {
                desktop_session__detail(session, "The server reply could not be verified. Check its encryption keys and required identity size.", 1);
            }
            crypto_session__free_json(root);
            root = NULL;
            crypto_session__free_text(plaintext);
            plaintext = NULL;
            desktop_message__release(&frame);
            if (desktop_session__stopping(session))
            {
                break;
            }
        }
        if (desktop_session__stopping(session))
        {
            break;
        }
        if (phase < 3 && desktop_system__milliseconds() >= deadline)
        {
            desktop_session__detail(session, "Login timed out. Check the address and server encryption keys.", 1);
        }
        if (phase == 3)
        {
            desktop_voice__tick(session);
            if (desktop_system__milliseconds() - keepalive >= 5000)
            {
                client_msg__connection__send_keepalive(session);
                keepalive = desktop_system__milliseconds();
            }
            if (!client_msg__channel__distribute_keys(session))
            {
                desktop_session__detail(session, "Could not distribute channel keys safely. Disconnected.", 1);
            }
            client_msg__dispatch_command(session);
            client_msg__attachments__tick(session);
            desktop_attachments__view(session->attachments, &session->model.attachments);
            if (!session->model.idle && !session->model.keys_ready && session->model.channel >= 0 && desktop_system__milliseconds() - session->waiting_since >= 15000)
            {
                client_msg__channel__send_reset_maintainer(session);
                session->waiting_since = desktop_system__milliseconds();
            }
        }
        if (batch || (phase == 3 && desktop_system__milliseconds() - published >= 25))
        {
            desktop_session__publish(session);
            published = desktop_system__milliseconds();
        }
        desktop_system__sleep(5);
    }
label_finished:
    desktop_mutex__lock(session->mutex);
    if (session->model.connected)
    {
        if (status.close_code == DESKTOP_SERVER_CLOSE_KICKED)
        {
            desktop_voice__sound(session->voice, DESKTOP_SOUND_YOU_WERE_KICKED_FROM_THE_SERVER);
        }
        else if (status.close_code == DESKTOP_SERVER_CLOSE_BANNED)
        {
            desktop_voice__sound(session->voice, DESKTOP_SOUND_YOU_WERE_BANNED_FROM_THE_SERVER);
        }
        else
        {
            desktop_voice__sound(session->voice, session->stop ? DESKTOP_SOUND_DISCONNECTED : DESKTOP_SOUND_CONNECTION_LOST);
        }
    }
    else if (session->failed && !session->stop)
    {
        desktop_voice__sound(session->voice, DESKTOP_SOUND_ERROR);
    }
    desktop_mutex__unlock(session->mutex);
    desktop_attachments__disconnect(session->attachments);
    desktop_attachments__view(session->attachments, &session->model.attachments);
    desktop_voice__stop(session->voice);
    desktop_transport__disconnect(session->transport);
    crypto_session__destroy(session->crypto);
    session->crypto = NULL;
    session->model.connected = 0;
    session->model.client_count = 0;
    desktop_session__view_release(&session->model);
    session->model.local_id = -1;
    session->model.channel = -1;
    session->model.audio_ready = 0;
    session->model.microphone_ready = 0;
    session->model.talking = 0;
    session->model.voice_detected = FALSE;
    session->model.input_level = -90;
    session->model.idle = 0;
    clib__string_copy(session->model.audio_detail, sizeof(session->model.audio_detail), "Disconnected");
    desktop_session__keys_reset(session);
    mbedtls_platform_zeroize(&session->connection, sizeof(session->connection));
    clib__null_memory(session->client_keys, sizeof(session->client_keys));
    if (!session->failed)
    {
        desktop_session__detail(session, "Disconnected", 0);
    }
    desktop_session__publish(session);
    desktop_mutex__lock(session->mutex);
    session->running = 0;
    desktop_mutex__unlock(session->mutex);
}

/**
 * @brief creates the GUI-owned session and its stable transport; no connection starts here.
 * @return desktop_session_t* -> owned session, or NULL on allocation failure
 */
desktop_session_t *desktop_session__create(void)
{
    desktop_session_t *session = calloc(1, sizeof(*session));
    if (!session)
    {
        return NULL;
    }
    session->mutex = desktop_mutex__create();
    session->transport = desktop_transport__create();
    session->voice = desktop_voice__create(1);
    session->attachments = desktop_attachments__create();
    session->push_to_talk_key = DESKTOP_HOTKEY_DEFAULT;
    session->audio_options.volume = 100;
    session->audio_options.gain = 100;
    if (!session->mutex || !session->transport || !session->voice || !session->attachments)
    {
        desktop_session__destroy(session);
        return NULL;
    }
    session->published.local_id = -1;
    session->published.channel = -1;
    session->published.revision = 1;
    clib__string_copy(session->published.detail, sizeof(session->published.detail), "Ready to connect");
    return session;
}

/**
 * @brief requests cancellation; the worker releases protocol and key storage after exiting.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return void
 */
void desktop_session__disconnect(desktop_session_t *session)
{
    if (session && session->mutex)
    {
        desktop_mutex__lock(session->mutex);
        session->stop = 1;
        desktop_mutex__unlock(session->mutex);
    }
}

/**
 * @brief joins the worker before destroying anything it owns.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return void
 */
void desktop_session__destroy(desktop_session_t *session)
{
    if (session)
    {
        desktop_session__disconnect(session);
        desktop_thread__join(session->thread);
        desktop_voice__destroy(session->voice);
        desktop_attachments__destroy(session->attachments);
        desktop_transport__destroy(session->transport);
        desktop_mutex__destroy(session->mutex);
        desktop_session__view_release(&session->model);
        desktop_session__view_release(&session->published);
        mbedtls_platform_zeroize(session, sizeof(*session));
        free(session);
    }
}

/**
 * @brief copies local voice preferences without opening devices on the GUI thread.
 * @param desktop_session_t* session -> GUI-owned session
 * @param const desktop_audio_options_t* options -> requested local preferences
 * @param int64 key -> persistent push-to-talk key, as defined by the platform key adapter
 * @return void
 */
void desktop_session__audio(desktop_session_t *session, const desktop_audio_options_t *options, int64 key)
{
    desktop_mutex__lock(session->mutex);
    session->audio_options = *options;
    session->push_to_talk_key = desktop_hotkey__index(key) >= 0 ? key : DESKTOP_HOTKEY_DEFAULT;
    desktop_mutex__unlock(session->mutex);
}

/**
 * @brief accesses the lifetime-stable voice engine from the GUI even while disconnected.
 * @param desktop_session_t* session -> GUI-owned live session
 * @param desktop_devices_view_t* view -> caller-owned device snapshot
 * @param boole refresh -> TRUE to request asynchronous enumeration
 * @return boole -> TRUE when the snapshot changed
 */
boole desktop_session__devices(desktop_session_t *session, desktop_devices_view_t *view, boole refresh)
{
    return desktop_voice__devices(session->voice, view, refresh);
}

/**
 * @brief starts one session with copied, validated settings; called only by the GUI owner.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const desktop_connection_t* connection -> validated connection options copied into session-owned storage
 * @return boole -> 1 when the worker started; 0 when invalid, busy or unavailable
 */
boole desktop_session__connect(desktop_session_t *session, const desktop_connection_t *connection)
{
    desktop_endpoint_t endpoint = { 0 };
    boole running = 0;
    if (!session || !connection || !clib__memchr(connection->address, 0, sizeof(connection->address)) || !clib__memchr(connection->identity, 0, sizeof(connection->identity)) || !connection->identity[0] || !clib__memchr(connection->username, 0, sizeof(connection->username)) || !clib__memchr(connection->server_keys, 0, sizeof(connection->server_keys)) || connection->key_bits < 2048 || connection->key_bits > 8192 || connection->key_bits % 256 || !desktop_endpoint__parse(connection->address, &endpoint))
    {
        return 0;
    }
    desktop_mutex__lock(session->mutex);
    running = session->running;
    desktop_mutex__unlock(session->mutex);
    if (running)
    {
        return 0;
    }
    desktop_thread__join(session->thread);
    session->thread = NULL;
    session->connection = *connection;
    session->failed = 0;
    session->stop = 0;
    session->command_count = 0;
    session->running = 1;
    session->thread = desktop_thread__start(_session_internal__worker, session);
    if (!session->thread)
    {
        session->running = 0;
        mbedtls_platform_zeroize(&session->connection, sizeof(session->connection));
        return 0;
    }
    return 1;
}

/**
 * @brief checks whether a connection is still active or finishing before local settings are replaced.
 * @param desktop_session_t* session -> GUI-owned session
 * @return boole -> TRUE while the worker owns the current connection
 */
boole desktop_session__busy(desktop_session_t *session)
{
    boole busy = FALSE;
    desktop_mutex__lock(session->mutex);
    busy = session->running;
    desktop_mutex__unlock(session->mutex);
    return busy;
}

/**
 * @brief copies a new immutable snapshot into GUI-owned storage only when its revision changed.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param desktop_session_view_t* view -> GUI-owned session snapshot
 * @return boole -> 1 when a newer snapshot was copied
 */
boole desktop_session__view(desktop_session_t *session, desktop_session_view_t *view)
{
    boole changed = 0;
    desktop_mutex__lock(session->mutex);
    changed = view->revision != session->published.revision;
    if (changed)
    {
        changed = desktop_session__view_copy(view, &session->published);
    }
    desktop_mutex__unlock(session->mutex);
    return changed;
}

/**
 * @brief queues a channel request without blocking the GUI on network writes.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param int64 channel -> server-assigned channel ID
 * @param const char* password -> borrowed channel password, copied into the bounded command queue
 * @return boole -> 1 when the bounded command queue accepted the request
 */
boole desktop_session__join(desktop_session_t *session, int64 channel, const char *password)
{
    boole queued = 0;
    desktop_session_command_t command = { 0 };
    command.join = 1;
    command.channel = channel;
    if (channel < 0 || !clib__string_copy(command.text, sizeof(command.text), password))
    {
        return 0;
    }
    desktop_mutex__lock(session->mutex);
    if (session->running && !session->stop && session->published.connected && session->command_count < 8)
    {
        session->commands[session->command_count++] = command;
        queued = 1;
    }
    desktop_mutex__unlock(session->mutex);
    mbedtls_platform_zeroize(&command, sizeof(command));
    return queued;
}

/**
 * @brief queues plain channel text, preserving the selected channel at submission time.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const char* text -> borrowed NUL-terminated text; copied before returning when retained
 * @return boole -> 1 when channel keys are ready and text was queued
 */
boole desktop_session__chat(desktop_session_t *session, const char *text)
{
    boole queued = 0;
    desktop_session_command_t command = { 0 };
    if (!text || !text[0] || !clib__string_copy(command.text, sizeof(command.text), text))
    {
        return 0;
    }
    desktop_mutex__lock(session->mutex);
    if (session->running && !session->stop && session->published.keys_ready && session->command_count < 8)
    {
        command.channel = session->published.channel;
        command.format = session->chat_format;
        session->commands[session->command_count++] = command;
        queued = 1;
    }
    desktop_mutex__unlock(session->mutex);
    mbedtls_platform_zeroize(&command, sizeof(command));
    return queued;
}

/**
 * @brief queues private text only for the exact connected peer selected by the GUI.
 * @param desktop_session_t* session -> GUI-owned session handle
 * @param int64 peer -> selected client ID
 * @param uint64 serial -> selected client's connection lifetime
 * @param const char* text -> bounded plain text copied before returning
 * @return boole -> 1 when queued, zero when unavailable or full
 */
boole desktop_session__direct(desktop_session_t *session, int64 peer, uint64 serial, const char *text)
{
    desktop_session_command_t command = { 0 };
    int64 index = 0;
    boole queued = 0;
    if (!text || !text[0] || !clib__string_copy(command.text, sizeof(command.text), text))
    {
        return 0;
    }
    command.direct = 1;
    command.peer = peer;
    command.peer_serial = serial;
    desktop_mutex__lock(session->mutex);
    for (index = 0; index < session->published.client_count; index++)
    {
        if (session->published.clients[index].id == peer && session->published.clients[index].serial == serial)
        {
            break;
        }
    }
    if (session->running && !session->stop && session->published.connected && !session->published.idle && session->command_count < 8 && peer != session->published.local_id && index < session->published.client_count && !session->published.clients[index].music)
    {
        command.format = session->chat_format;
        session->commands[session->command_count++] = command;
        queued = 1;
    }
    desktop_mutex__unlock(session->mutex);
    mbedtls_platform_zeroize(&command, sizeof(command));
    return queued;
}

/** @brief Changes outgoing formatting; already queued messages retain their captured selection. */
void desktop_session__set_chat_format(desktop_session_t *session, const desktop_chat_format_t *format)
{
    desktop_chat_format_t selected;
    desktop_chat_format__default(&selected);
    if (desktop_chat_format__valid(format))
    {
        selected = *format;
    }
    desktop_mutex__lock(session->mutex);
    session->chat_format = selected;
    desktop_mutex__unlock(session->mutex);
}

/**
 * @brief queues a local listening change for one connected client, leaving server permissions untouched.
 * @param desktop_session_t* session -> GUI-owned session handle
 * @param int64 peer -> selected client ID
 * @param uint64 serial -> selected client's connection lifetime
 * @param boole muted -> nonzero to silence this peer locally
 * @return boole -> 1 when queued
 */
boole desktop_session__mute(desktop_session_t *session, int64 peer, uint64 serial, boole muted)
{
    desktop_session_command_t command = { 0 };
    boole queued = 0;
    command.mute = 1;
    command.muted = muted != 0;
    command.peer = peer;
    command.peer_serial = serial;
    desktop_mutex__lock(session->mutex);
    if (session->running && !session->stop && session->published.connected && session->command_count < 8 && peer >= 0 && peer != session->published.local_id)
    {
        session->commands[session->command_count++] = command;
        queued = 1;
    }
    desktop_mutex__unlock(session->mutex);
    return queued;
}

/**
 * @brief extracts current-channel display data only; private keys and chat never enter game IPC.
 * @param const desktop_session_view_t* view -> GUI-owned session snapshot
 * @param desktop_overlay_snapshot_t* overlay -> destination for current-channel display data only
 * @return void
 */
void desktop_session__overlay(const desktop_session_view_t *view, desktop_overlay_snapshot_t *overlay)
{
    int64 index = 0;
    boole speaking = 0;
    const desktop_client_t *client = NULL_POINTER;
    desktop_overlay_member_t *member = NULL_POINTER;
    desktop_overlay_data__clear(overlay);
    if (!view->connected || view->idle || view->channel < 0)
    {
        return;
    }
    for (index = 0; index < view->channel_count; index++)
    {
        if (view->channels[index].id == view->channel)
        {
            clib__string_copy(overlay->channel, sizeof(overlay->channel), view->channels[index].name);
        }
    }
    for (index = 0; index < view->client_count; index++)
    {
        if (view->clients[index].channel == view->channel && !view->clients[index].idle)
        {
            overlay->total_member_count++;
        }
    }
    for (speaking = 1; speaking >= 0; speaking--)
    {
        for (index = 0; index < view->client_count && overlay->member_count < DESKTOP_OVERLAY_MEMBERS; index++)
        {
            client = &view->clients[index];
            if (client->channel != view->channel || client->idle || ((client->audio == 1) != speaking))
            {
                continue;
            }
            member = &overlay->members[overlay->member_count++];
            member->id = (uint)index;
            member->flags = (client->audio == 1 ? DESKTOP_OVERLAY_SPEAKING : 0) | (client->audio >= 3 ? DESKTOP_OVERLAY_MUTED : 0) | (client->id == view->local_id ? DESKTOP_OVERLAY_LOCAL : 0) | (client->audio == 4 ? DESKTOP_OVERLAY_DEAFENED : 0) | (client->muted ? DESKTOP_OVERLAY_LOCAL_MUTE : 0);
            if (!clib__string_copy(member->name, sizeof(member->name), client->name))
            {
                clib__string_copy(member->name, sizeof(member->name), "Long username");
            }
        }
    }
    overlay->enabled = 1;
}

void desktop_session__sound(desktop_session_t *session, desktop_sound_t id)
{
    desktop_voice__sound(session->voice, id);
}

void desktop_session__notifications(desktop_session_t *session, boole enabled, int64 volume, const char *output)
{
    desktop_voice__notifications(session->voice, enabled, volume, output);
}
