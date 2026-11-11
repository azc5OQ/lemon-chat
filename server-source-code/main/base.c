// This file reads incoming message types and calls the matching request handlers.
// It also holds shared server state and checks clients, permissions and server limits.
#include "base.h"
#include "core/channel.h"
#include "crypto/auth_rsa.h"
#include "msg/client/client_msg.h"
#include "core/attachments.h"
#include "httpclient/url_resolver.h"
#include "core/administration.h"
#include "msg/server/server_msg.h"
#include "core/server_logs.h"
#include "audio_websocket.h"
#include "video_websocket.h"
#include "memory_manager.h"
#include "clib/clib.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "util.h"
#include "../third-party/rxi-log/log.h"
#include <stdatomic.h>
#include <string.h>

// When several locks are needed, take them in this order:
// clients, WebRTC peers, channels, icons, groups, then bans.
custom_rwlock_t g_clients_global_rwlock_guard;
custom_rwlock_t g_channels_global_rwlock_guard;
custom_rwlock_t g_icons_global_rwlock_guard;
custom_rwlock_t g_groups_global_rwlock_guard;
custom_rwlock_t g_webrtc_peers_rwlock_guard;
custom_rwlock_t g_bans_global_rwlock_guard;

pthread_mutex_t g_chat_message_id_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64 g_chat_message_id;
client_t *g_clients_array;
channel_t *g_channel_array;
icon_t *g_icons_array;
group_t *g_groups_array;
ban_entry_t *g_ban_array;
server_settings_t g_server_settings;

static _Thread_local client_request_context_t g_client_request_context;

// Begin while the clients lock protects the socket/authentication snapshot. End
// restores the prior thread-local scope, including when dispatch is nested.
void base__begin_client_request(ws_cli_conn_t *socket, uint64 client_id, boole authenticated, client_request_context_t *previous)
{
    // save the previous context
    *previous = g_client_request_context;

    // set the new context
    g_client_request_context.socket = socket;
    g_client_request_context.client_id = client_id;
    g_client_request_context.authenticated = authenticated;
    g_client_request_context.active = TRUE;
}

void base__end_client_request(const client_request_context_t *previous)
{
    g_client_request_context = *previous;
}

// Caller holds the clients lock. Internal calls without a request scope remain
// allowed for a valid existing slot; WebSocket dispatch must establish a scope.
boole base__is_current_client_request(uint64 client_id)
{
    client_t *client = NULL_POINTER;

    // check the client id
    if (g_clients_array == NULL_POINTER || client_id >= (uint64)g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // check the client exists
    client = &g_clients_array[client_id];
    if (client->is_existing == FALSE)
    {
        return FALSE;
    }

    // allow internal calls
    if (g_client_request_context.active == FALSE)
    {
        return TRUE;
    }

    // compare with the context
    return (boole)(g_client_request_context.client_id == client_id && g_client_request_context.socket == client->p_ws_connection && g_client_request_context.authenticated == client->is_authenticated);
}

static boole g_server_uptime_started = FALSE;
static uint64 g_server_started_monotonic_ms = 0;

static _Atomic(uint64) webrtc_received_bytes;
static _Atomic(uint64) webrtc_sent_bytes;

#if SIZE_MAX <= UINT32_MAX
// Leave room for libraries and thread stacks in a 32-bit process.
#ifndef LEMON_32BIT_RAM_BUDGET_MB
#define LEMON_32BIT_RAM_BUDGET_MB 1536U
#endif
#endif

// declarations
static boole _base_internal__is_message_correct_at_first_sight_and_get_message_type(cJSON *json_root, uint64 client_id, char **out_message_type);
static void _base_internal__process_music_bot_key_request(cJSON *json_root, ws_cli_conn_t *websocket, uint64 client_index);

/**
 * @brief enforces per-client spam protection: rejects an action made inside the cooldown window,
 *        otherwise records its time and allows it
 *
 * @param uint64 client_index -> id of the client
 *
 * @attention uses write lock on clients_global_rwlock_guard
 *
 * @return boole
 */
boole base__is_request_allowed_based_on_spam_protection(uint64 client_index)
{
    uint64 timestamp_now = 0;
    boole skip = FALSE;

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    // check the request context
    if (base__is_current_client_request(client_index) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return FALSE;
    }

    // check the cooldown time
    timestamp_now = util__get_timestamp_ms();
    if (timestamp_now < (g_clients_array[client_index].timestamp_last_action + TIMESTAMP_LAST_ACTION_COOLDOWN_MS))
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_change_client_username TIMESTAMP_LAST_ACTION_COOLDOWN_MS \n");
        skip = TRUE;
    }

    // store the action time
    if (skip == FALSE)
    {
        g_clients_array[client_index].timestamp_last_action = timestamp_now;
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);

    // return the result
    if (skip == TRUE)
    {
        return FALSE;
    }
    else
    {
        return TRUE;
    }
}

/**
 * @brief returns the current global chat message id (thread-safe)
 *
 * @return uint64 the current chat message id
 */
uint64 base__get_chat_message_id(void)
{
    uint64 to_return = 0;

    // read the message id
    pthread_mutex_lock(&g_chat_message_id_mutex);
    to_return = g_chat_message_id;
    pthread_mutex_unlock(&g_chat_message_id_mutex);

    return to_return;
}

/**
 * @brief increments the global chat message id (thread-safe)
 *
 * @return void
 */
void base__increment_chat_message_id(void)
{
    // increase the message id
    pthread_mutex_lock(&g_chat_message_id_mutex);

    g_chat_message_id++;

    pthread_mutex_unlock(&g_chat_message_id_mutex);
}

/**
 * @brief Checks that a client slot exists, is authenticated and has a WebSocket connection.
 * @param uint64 client_id -> index into g_clients_array; client id and client index are the same
 * @return boole -> TRUE when all checks pass; FALSE for an invalid or unused slot
 * @attention The caller holds the clients lock.
 */
boole base__is_client_valid(uint64 client_id)
{
    // check the index before reading the client slot
    if (client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // check the client slot
    return g_clients_array[client_id].is_existing == TRUE &&
           g_clients_array[client_id].is_authenticated == TRUE &&
           g_clients_array[client_id].p_ws_connection != NULL_POINTER;
}

/**
 * @brief Checks for an authenticated client with a WebSocket connection, excluding music bots.
 * @param uint64 client_id -> index into the clients array
 * @return boole -> TRUE when the client passes these checks
 * @attention The caller holds the clients lock.
 */
boole base__is_client_valid_and_not_music_bot(uint64 client_id)
{
    // check the index before reading the client slot
    if (client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // check slot, exclude bots
    return g_clients_array[client_id].is_existing == TRUE &&
           g_clients_array[client_id].is_authenticated == TRUE &&
           g_clients_array[client_id].p_ws_connection != NULL_POINTER &&
           g_clients_array[client_id].is_music_bot == FALSE;
}

/**
 * @brief Checks for an authenticated client with a WebSocket connection and admin rights.
 * @param uint64 client_id -> index into the clients array
 * @return boole -> TRUE when the client passes these checks
 * @attention The caller holds the clients lock.
 */
boole base__is_client_valid_admin(uint64 client_id)
{
    // check the index before reading the client slot
    if (client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // check slot and admin
    return g_clients_array[client_id].is_existing == TRUE &&
           g_clients_array[client_id].is_authenticated == TRUE &&
           g_clients_array[client_id].p_ws_connection != NULL_POINTER &&
           g_clients_array[client_id].is_admin == TRUE;
}

/**
 * @brief Checks whether the client slot holds an existing music bot.
 * @param uint64 client_id -> index into the clients array
 * @return boole -> TRUE when the slot contains a music bot
 * @attention The caller holds the clients lock.
 */
boole base__is_client_valid_musicbot(uint64 client_id)
{
    // check the index before reading the client slot
    if (client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // check the bot flag
    return g_clients_array[client_id].is_existing == TRUE &&
           g_clients_array[client_id].is_music_bot == TRUE;
}

/**
 * @brief Checks whether an existing music bot has room for another song.
 * @param uint64 client_id -> index into the clients array
 * @return boole -> TRUE when the music bot has a free song slot
 * @attention The caller holds the clients lock.
 */
boole base__is_music_bot_and_song_slot_valid(uint64 client_id)
{
    // check the index before reading the client slot
    if (client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // check the song count
    return g_clients_array[client_id].is_existing == TRUE &&
           g_clients_array[client_id].is_music_bot == TRUE &&
           g_clients_array[client_id].music_bot_client_extension.music_bot_songs_count != MUSIC_BOT_MAX_FILE_COUNT;
}

/**
 * @brief Checks whether this user chose to block this kind of picture or file and the server allows
 *        it.
 * @param client_t* client -> recipient, protected by the clients lock
 * @param cstring receive_type -> direct_chat_picture, direct_chat_file, channel_chat_picture or channel_chat_file
 * @return boole -> TRUE when a new attachment should skip this recipient
 */
boole base__is_attachment_blocked(client_t *client, cstring receive_type)
{
    // choose setting by type
    if (clib__strcmp(receive_type, "direct_chat_picture") == 0)
    {
        return g_server_settings.is_blocking_direct_chat_pictures_active && client->block_direct_chat_pictures;
    }
    if (clib__strcmp(receive_type, "direct_chat_file") == 0)
    {
        return g_server_settings.is_blocking_direct_chat_files_active && client->block_direct_chat_files;
    }
    if (clib__strcmp(receive_type, "channel_chat_picture") == 0)
    {
        return g_server_settings.is_blocking_channel_chat_pictures_active && client->block_channel_chat_pictures;
    }
    if (clib__strcmp(receive_type, "channel_chat_file") == 0)
    {
        return g_server_settings.is_blocking_channel_chat_files_active && client->block_channel_chat_files;
    }

    // unknown types are allowed
    return FALSE;
}

/**
 * @brief Checks whether the 32-bit memory budget prevents a new login or upload. The 64-bit build
 *        always returns FALSE.
 * @return boole -> TRUE when the 32-bit budget is reached or usage cannot be read
 */
boole base__is_server_ram_budget_reached(void)
{
#if SIZE_MAX <= UINT32_MAX
    uint64 bytes = 0;

    // read the RAM usage
    if (util__get_process_ram_usage(&bytes) == FALSE)
    {
        return TRUE;
    }

    // compare with the limit
    return bytes / (1024U * 1024U) >= LEMON_32BIT_RAM_BUDGET_MB;
#else
    return FALSE;
#endif
}

/**
 * @brief Counts WebRTC data-channel bytes received or accepted for sending since startup.
 * @param uint64 received -> bytes received in this call
 * @param uint64 sent -> bytes accepted for sending in this call
 * @return void
 */
void base__count_webrtc_bytes(uint64 received, uint64 sent)
{
    // add received bytes
    if (received != 0)
    {
        atomic_fetch_add_explicit(&webrtc_received_bytes, received, memory_order_relaxed);
    }

    // add sent bytes
    if (sent != 0)
    {
        atomic_fetch_add_explicit(&webrtc_sent_bytes, sent, memory_order_relaxed);
    }
}

/**
 * @brief Reads WebRTC traffic totals without blocking the threads that relay audio and video.
 * @param uint64* received -> receives the incoming byte total
 * @param uint64* sent -> receives the outgoing byte total
 * @return void
 */
void base__get_webrtc_bytes(uint64 *received, uint64 *sent)
{
    *received = atomic_load_explicit(&webrtc_received_bytes, memory_order_relaxed);
    *sent = atomic_load_explicit(&webrtc_sent_bytes, memory_order_relaxed);
}

/**
 * @brief Records when this server run begins, before any worker threads start.
 * @return void
 */
void base__start_server_uptime(void)
{
    g_server_uptime_started = util__get_monotonic_ms(&g_server_started_monotonic_ms);
}

/**
 * @brief Returns how many whole seconds this server run has been active.
 * @return uint64 -> elapsed seconds, or zero if the clock is unavailable
 */
uint64 base__get_server_uptime_seconds(void)
{
    uint64 now = 0;

    // check the clock
    if (g_server_uptime_started == FALSE || util__get_monotonic_ms(&now) == FALSE || now < g_server_started_monotonic_ms)
    {
        return 0;
    }

    // convert to seconds
    return (now - g_server_started_monotonic_ms) / 1000;
}

/**
 * @brief Checks the message object and reads its type before choosing a request handler.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 * @param char** out_message_type -> receives a pointer to the message type string parsed from the json
 *
 * @return boole -> TRUE when the message contains a nonempty type string
 */
static boole _base_internal__is_message_correct_at_first_sight_and_get_message_type(cJSON *json_root, uint64 client_id, char **out_message_type)
{
    cJSON *json_message_type = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    int64 objectCount = 0;

    // check the root size
    objectCount = cJSON_GetArraySize(json_root);

    if (objectCount != 1)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " objectCount = cJSON_GetArraySize(json_root) is not 1");
        return FALSE;
    }

    // find the message object
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    // check the object type
    if (cJSON_IsObject(json_message_object) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_message_object) == false\n");
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
    if (cJSON_IsString(json_message_type) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_message_type) \n");
        return FALSE;
    }

    // check the type text
    if (json_message_type->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type->valuestring == NULL \n");
        return FALSE;
    }

    // refuse an empty type
    if (clib__utf8_string_length(json_message_type->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_message_type->valuestring) \n");
        return FALSE;
    }

    // return the type
    *out_message_type = json_message_type->valuestring;
    return TRUE;
}

/**
 * @brief Sends the requested bot key after checking the session, bot ID and channel.
 * @attention Acquires the clients write lock and preserves the request cooldown.
 */
static void _base_internal__process_music_bot_key_request(cJSON *json_root, ws_cli_conn_t *websocket, uint64 client_index)
{
    cJSON *message = NULL_POINTER;
    cJSON *bot_id = NULL_POINTER;
    client_t *client = NULL_POINTER;
    client_t *bot = NULL_POINTER;
    uint64 now = 0;

    // read the request fields
    message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    bot_id = cJSON_GetObjectItemCaseSensitive(message, "client_id");
    now = util__get_timestamp_ms();

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    // check the request context
    if (base__is_current_client_request(client_index) == FALSE)
    {
        goto end;
    }

    // check the client socket
    client = &g_clients_array[client_index];
    if (client->is_existing != TRUE || client->is_authenticated != TRUE || client->p_ws_connection != websocket)
    {
        goto end;
    }

    // check the id type
    if ((boole)cJSON_IsNumber(bot_id) == FALSE)
    {
        goto end;
    }

    // check the id range
    if (!(bot_id->valuedouble >= 0) ||
        !(bot_id->valuedouble < g_server_settings.max_client_count) ||
        bot_id->valuedouble != bot_id->valueint)
    {
        goto end;
    }

    // limit the request rate
    if (!(now - client->timestamp_last_music_bot_key_request >= 250))
    {
        goto end;
    }
    client->timestamp_last_music_bot_key_request = now;

    // the key goes only to a non-idle client in the bot's channel
    bot = &g_clients_array[bot_id->valueint];
    if (bot->is_existing == TRUE && bot->is_music_bot == TRUE && client->is_idle == FALSE && client->channel_id == bot->channel_id)
    {
        server_msg__musicbot__send_key(client, bot);
    }

end:
    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/** @brief Bounds command tree allocation before parsing and rejects ambiguous JSON text. */
static cJSON *_base_internal__parse_client_message(const char *text)
{
    const char *cursor = NULL_POINTER;
    ubyte value = 0;

    // existing requests are shallow objects; large chat/file bodies are strings.
    // this conservative structural count bounds tree nodes even for unused fields
    const uint64 maximum_depth = 32;
    const uint64 maximum_complexity = 4096;
    uint64 depth = 0;
    uint64 complexity = 0;
    boole quoted = FALSE;
    boole escaped = FALSE;

    // check the text
    if (text == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // check every character
    for (cursor = text; *cursor != 0; cursor++)
    {
        // refuse control characters
        value = (ubyte)*cursor;
        if (value < 32 && (quoted || (value != '\t' && value != '\r' && value != '\n')))
        {
            return NULL_POINTER;
        }

        // update flags and counters
        if (quoted)
        {
            // check escape and quote
            if (escaped)
            {
                if (strncmp(cursor, "u0000", 5) == 0)
                {
                    return NULL_POINTER;
                }
                escaped = FALSE;
            }
            else if (value == '\\')
            {
                escaped = TRUE;
            }
            else if (value == '"')
            {
                quoted = FALSE;
            }
        }
        else if (value == '"')
        {
            quoted = TRUE;
        }
        else if (value == '{' || value == '[')
        {
            // count depth and complexity
            if (++depth > maximum_depth)
            {
                return NULL_POINTER;
            }
            complexity += value == '[' ? 2 : 1;
        }
        else if (value == '}' || value == ']')
        {
            // decrease the depth
            if (depth == 0)
            {
                return NULL_POINTER;
            }
            depth--;
        }
        else if (value == ':' || value == ',')
        {
            complexity++;
        }

        // check the complexity limit
        if (complexity > maximum_complexity)
        {
            return NULL_POINTER;
        }
    }

    // check everything is closed
    if (quoted || depth != 0)
    {
        return NULL_POINTER;
    }

    // parse the JSON
    return cJSON_ParseWithOpts(text, NULL_POINTER, TRUE);
}

/**
 * @brief routes an authenticated client's decrypted message to the matching handler
 *
 * @param ws_cli_conn_t* websocket -> the client's websocket connection
 * @param uint64 client_index -> id of the client
 * @param char* decrypted_metadata_cstring -> the decrypted message json
 *
 * @return void
 */
void base__process_authenticated_client_message(ws_cli_conn_t *websocket, uint64 client_index, char *decrypted_metadata_cstring)
{
    cJSON *json_root = NULL_POINTER;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    char *message_type = NULL_POINTER;
    client_t *client = NULL_POINTER;
    boole is_sender_idle = FALSE;

    // DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %s %s", "base__process_authenticated_client_message message : ", decrypted_metadata_cstring, "\n");
    json_root = _base_internal__parse_client_message(decrypted_metadata_cstring);

    // refuse unreadable JSON
    if (json_root == NULL_POINTER)
    {
        // record the rejection without copying decrypted passwords or keys into logs
        log_info("%s %llu %s %llu", "client : ", client_index, " unparseable frame, decrypted length ", (uint64)clib__strlen(decrypted_metadata_cstring));
        ws_close_client(websocket);

        // there is no json object to call cJSON_Delete on so just disconnect the client
        return;
    }

    // read the message type
    if (_base_internal__is_message_correct_at_first_sight_and_get_message_type(json_root, client_index, &message_type) == FALSE)
    {
        // the validator identifies the malformed field; keep the payload private
        log_info("%s %llu %s %llu", "client : ", client_index, " rejected frame, decrypted length ", (uint64)clib__strlen(decrypted_metadata_cstring));
        ws_close_client(websocket);
        util__json_delete_secure(json_root);
        return;
    }

    // log the message type
    DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %llu %s %s %s", "client : ", client_index, " detected message type is ", message_type, "\n");

    // check the client socket
    clib__write_lock(&g_clients_global_rwlock_guard);
    status0 = base__is_current_client_request(client_index);
    if (status0 == TRUE)
    {
        status1 = (boole)(g_clients_array[client_index].p_ws_connection == websocket);
        status2 = g_clients_array[client_index].is_authenticated;
    }
    if (status0 == FALSE || status1 == FALSE || status2 == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        util__json_delete_secure(json_root);
        return;
    }

    // a decrypted, well-formed message from this authenticated socket proves
    // liveness even when its action is later refused by permissions or policy.
    // never refresh a replacement session or accept malformed JSON as activity
    g_clients_array[client_index].timestamp_last_maintain_connection_message_received = util__get_timestamp_ms();
    is_sender_idle = g_clients_array[client_index].is_idle;
    clib__unlock(&g_clients_global_rwlock_guard);

    // todo, ignore audio related messages if audio is completely disabled by server

    // idle-exempt messages: the heartbeat plus the whole webrtc handshake - the server offers
    // to idle clients (create is allowed while idle), so their answers and candidates must pass too
    if (clib__is_string_equal(message_type, "client_connection_check"))
    {
        client_msg__connection__process_connection_check(json_root, client_index);
    }
    else if (clib__is_string_equal(message_type, "load_server_info"))
    {
        client_msg__server_settings__process_info_request(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "load_server_emojis"))
    {
        client_msg__emojis__process_list(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "load_server_emoji"))
    {
        client_msg__emojis__process_image(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "upload_server_emoji"))
    {
        client_msg__emojis__process_upload(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "delete_server_emoji"))
    {
        client_msg__emojis__process_delete(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "set_attachment_receive_preferences"))
    {
        client_msg__attachments__process_attachment_receive_preferences(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "set_server_banner"))
    {
        client_msg__server_settings__process_set_banner_request(json_root, client_index, websocket);
    }
    else if (clib__is_string_equal(message_type, "create_new_webrtc_datachannel_connection"))
    {
        client_msg__audio__process_create_new_webrtc_datachannel_connection(json_root, client_index);
    }
    else if (clib__is_string_equal(message_type, "request_audio_websocket"))
    {
        // check client and socket
        clib__write_lock(&g_clients_global_rwlock_guard);
        status0 = base__is_current_client_request(client_index);
        status1 = FALSE;
        status2 = FALSE;
        if (status0 == TRUE)
        {
            client = &g_clients_array[client_index];
            status1 = client->is_existing;
            status2 = (boole)(client->p_ws_connection == websocket);
        }

        // create the audio session
        if (status0 == TRUE && status1 == TRUE && status2 == TRUE)
        {
            audio_websocket__request(client);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
    }
    else if (clib__is_string_equal(message_type, "request_video_websocket"))
    {
        // check client and socket
        clib__write_lock(&g_clients_global_rwlock_guard);
        status0 = base__is_current_client_request(client_index);
        status1 = FALSE;
        status2 = FALSE;
        if (status0 == TRUE)
        {
            client = &g_clients_array[client_index];
            status1 = client->is_existing;
            status2 = (boole)(client->p_ws_connection == websocket);
        }

        // create the video session
        if (status0 == TRUE && status1 == TRUE && status2 == TRUE)
        {
            video_websocket__request(client);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
    }
    else if (clib__is_string_equal(message_type, "request_music_bot_key"))
    {
        _base_internal__process_music_bot_key_request(json_root, websocket, client_index);
    }
    else if (clib__is_string_equal(message_type, "sdp_answer"))
    {
        client_msg__audio__process_sdp_answer(json_root, client_index);
    }
    else if (clib__is_string_equal(message_type, "ice_candidate"))
    {
        client_msg__audio__process_ice_candidate(json_root, client_index);
    }
    else
    {
        // else block checks messages where it matters if client is in idle state or not
        if (is_sender_idle == FALSE)
        {
            // choose by message type
            if (clib__is_string_equal(message_type, "change_client_username"))
            {
                client_msg__profile__process_change_client_username(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "resolve_url"))
            {
                client_msg__chat__process_resolve_url_request(json_root, client_index, websocket);
            }
            else if (clib__is_string_equal(message_type, "create_channel_request"))
            {
                client_msg__channel__process_create_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "edit_channel_group"))
            {
                client_msg__moderation__process_channel_group(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "channel_access_request"))
            {
                client_msg__moderation__process_channel_access(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "moderate_client"))
            {
                client_msg__moderation__process_client_action(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "musicbot_playback"))
            {
                client_msg__moderation__process_music_control(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "save_server_emojis"))
            {
                client_msg__moderation__process_save_emojis(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "manage_bans"))
            {
                client_msg__moderation__process_bans(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "edit_channel_request"))
            {
                client_msg__channel__process_edit_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "direct_chat_message"))
            {
                client_msg__chat__process_direct_chat_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "offline_chat_message"))
            {
                client_msg__chat__process_offline_chat_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "channel_chat_message"))
            {
                client_msg__chat__process_channel_chat_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_chat_message_request"))
            {
                client_msg__chat__process_delete_chat_message_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "edit_chat_message_request"))
            {
                client_msg__chat__process_edit_chat_message_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "join_channel_request"))
            {
                client_msg__channel__process_join_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "reset_channel_maintainer"))
            {
                client_msg__channel__process_reset_maintainer_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_channel_request"))
            {
                client_msg__channel__process_delete_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "typing_indicator_request"))
            {
                client_msg__chat__process_typing_indicator_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "poke_client"))
            {
                client_msg__chat__process_poke_client_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "microphone_usage"))
            {
                client_msg__audio__process_microphone_usage(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "start_song_stream"))
            {
                client_msg__musicbot__process_start_song_stream_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "stop_song_stream"))
            {
                client_msg__musicbot__process_stop_song_stream_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "video_stream_start"))
            {
                client_msg__video__process_start(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "video_stream_stop"))
            {
                client_msg__video__process_stop(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "video_stream_watch"))
            {
                client_msg__video__process_watch(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "video_stream_allow_viewer"))
            {
                client_msg__video__process_allow_viewer(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "video_stream_keyframe_request"))
            {
                client_msg__video__process_keyframe_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "admin_password"))
            {
                client_msg__administration__process_admin_password_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "change_admin_password"))
            {
                client_msg__administration__process_change_admin_password_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "add_group_to_client"))
            {
                client_msg__administration__process_add_group_to_client_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "remove_group_from_client"))
            {
                client_msg__administration__process_remove_group_from_client_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_stored_clients"))
            {
                client_msg__profile__process_request_stored_clients(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_icon_upload"))
            {
                client_msg__administration__process_icon_upload(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_add_new_group"))
            {
                client_msg__administration__process_add_group(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_delete_group"))
            {
                client_msg__administration__process_delete_group(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_identity_list"))
            {
                client_msg__administration__process_request_identity_list(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_identity"))
            {
                client_msg__administration__process_delete_identity(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "modify_identity_group"))
            {
                client_msg__administration__process_modify_identity_group(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_delete_icon"))
            {
                client_msg__administration__process_delete_icon(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "rename_group"))
            {
                client_msg__administration__process_rename_group(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "save_groups"))
            {
                client_msg__administration__process_save_groups(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "set_group_permissions"))
            {
                client_msg__administration__process_set_group_permissions(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_set_group_icon"))
            {
                client_msg__administration__process_set_group_icon(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "set_channel_icon"))
            {
                client_msg__channel__process_set_icon(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "avatar_upload"))
            {
                client_msg__profile__process_avatar_upload(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_avatar"))
            {
                client_msg__profile__process_delete_avatar(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_avatar_for_client"))
            {
                client_msg__profile__process_request_avatar_for_client(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_avatars"))
            {
                client_msg__profile__process_request_avatars_batch(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "save_server_settings"))
            {
                client_msg__server_settings__process_save_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "load_server_settings"))
            {
                client_msg__server_settings__process_load_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "admin_log_request"))
            {
                client_msg__administration__process_admin_log_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "admin_log_clear"))
            {
                client_msg__administration__process_admin_log_clear(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "call_idle_client_request"))
            {
                if (g_server_settings.is_idle_mode_allowed == TRUE)
                {
                    client_msg__chat__process_call_idle_client_message(json_root, client_index);
                }
            }
            else if (clib__is_string_equal(message_type, "go_to_idle_mode_request"))
            {
                if (g_server_settings.is_idle_mode_allowed == TRUE)
                {
                    client_msg__channel__process_go_to_idle_mode_request(json_root, client_index);
                }
            }
            else if (clib__is_string_equal(message_type, "kick"))
            {
                client_msg__administration__process_kick_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "ban"))
            {
                client_msg__administration__process_ban_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "get_client_info"))
            {
                client_msg__administration__process_get_client_info_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "remove_ban"))
            {
                client_msg__administration__process_remove_ban_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "create_music_bot"))
            {
                client_msg__musicbot__process_create_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_music_bot"))
            {
                client_msg__musicbot__process_delete_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "musicbot_get_song_list"))
            {
                client_msg__musicbot__process_get_song_list_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "remove_song_from_music_bot"))
            {
                client_msg__musicbot__process_remove_song_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "file_send"))
            {
                client_msg__attachments__process_file_send_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "file_send_completed"))
            {
                // file_transfer__process_direct_chat_picture
                // and
                // file_transfer__process_channel_chat_picture
                // gets called by client_msg__attachments__process_file_send_completed_request if needed

                client_msg__attachments__process_file_send_completed_request(json_root, client_index);
            }
        }

        // allow return from idle
        if (is_sender_idle == TRUE)
        {
            if (clib__is_string_equal(message_type, "come_back_from_idle_mode_request"))
            {
                if (g_server_settings.is_idle_mode_allowed == TRUE)
                {
                    client_msg__channel__process_come_back_from_idle_mode_request(json_root, client_index);
                }
            }
        }
    }

    // print the allocation count
    DBG_DBG_MEMORY_ALLOCATIONS memorymanager__print_allocations_count();

    // delete the JSON
    util__json_delete_secure(json_root);
    json_root = 0;
}

/**
 * @brief routes a not-yet-authenticated client's message (key exchange / challenge) to the matching
 *        handler
 *
 * @param ws_cli_conn_t* websocket -> the client's websocket connection
 * @param uint64 index -> id of the client
 * @param char* decrypted_metadata_cstring -> the decrypted message json
 *
 * @return void
 */
void base__process_not_authenticated_client_message(ws_cli_conn_t *websocket, uint64 index, char *decrypted_metadata_cstring)
{
    cJSON *json_root = NULL_POINTER;
    int64 checked_message_length = 0;
    char *message_type = NULL_POINTER;

    // log the client socket
    DBG_AUTHENTICATION log_info("%s %p %s", "[i] authenticating client ", websocket, "\n");

    // drop oversized unauth messages. the cap must hold public_key_info: the 8192-bit DH public mix
    // (< SHARED_SECRET_LENGTH digits) + the RSA public key (< MAX_PUBLIC_KEY_LENGTH) + JSON scaffolding,
    // so it is derived from those constants rather than hardcoded, to survive future modulus/key bumps
    checked_message_length = clib__utf8_string_length_check_max_length(decrypted_metadata_cstring, UNAUTH_HANDSHAKE_MAX_LENGTH);

    // refuse a long message
    if (checked_message_length == -1)
    {
        DBG_AUTHENTICATION log_info("%s %llu", "oversized unauthenticated message from client ", index);
        server_logs__join_refused("wrong key or malformed handshake", ws_getaddress(websocket));
        ws_close_client(websocket);
        return;
    }

    // parse the JSON
    json_root = _base_internal__parse_client_message(decrypted_metadata_cstring);

    DBG_AUTHENTICATION log_info("%s", "base__process_not_authenticated_client_message decrypted_metadata_cstring \n");

    // refuse unreadable JSON
    if (json_root == 0)
    {
        DBG_AUTHENTICATION log_info("%s %llu %s", "client : ", index, " json_root is null \n");

        // a wrong key decrypts the handshake into garbage, which is exactly what arrives here
        server_logs__join_refused("wrong key or malformed handshake", ws_getaddress(websocket));
        ws_close_client(websocket);

        // there is no json object to cJSON_Delete, since it's 0
        return;
    }

    // read the message type
    if (_base_internal__is_message_correct_at_first_sight_and_get_message_type(json_root, index, &message_type) == FALSE)
    {
        DBG_AUTHENTICATION log_info("%s%llu%s", "client : ", index, "_base_internal__is_message_correct_at_first_sight_and_get_message_type failed \n");
        server_logs__join_refused("wrong key or malformed handshake", ws_getaddress(websocket));
        ws_close_client(websocket);
        util__json_delete_secure(json_root);
        return;
    }

    // log the message type
    DBG_AUTHENTICATION log_info("%s %llu %s %s %s", "client : ", index, " detected message type is ", message_type, "\n");

    // choose by message type
    if (clib__is_string_equal(message_type, "public_key_info"))
    {
        client_msg__connection__process_public_key_info(json_root, index, websocket);
    }
    else if (clib__is_string_equal(message_type, "public_key_challenge_response"))
    {
        client_msg__connection__process_public_key_challenge_response(json_root, index);
    }

    // delete the JSON
    util__json_delete_secure(json_root);
    json_root = 0;
}
