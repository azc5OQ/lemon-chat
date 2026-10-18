#include "definitions.h"
#include "audio_channel.h"
#include <limits.h>

#ifdef WIN32
#include <Windows.h>
#endif

#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "metadata_crypto.h"
#include "server_message.h"
#include "server_logs.h"
#include "memory_manager.h"
#include "../third-party/eteran-cvector/cvector.h"
#include "../third-party/rxi-log/log.h"
#include "clib/clib_string.h"  // clib__is_string_equal (identity online check)
#include "clib/clib_memory.h"  // clib__null_memory (clearing a delivered offline message slot)
#include "../third-party/zhicheng/base64.h"  // BASE64_ENCODE_OUT_SIZE (identity hash buffer)

#include "util.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"

typedef struct outgoing_metadata
{
    crypto_session_keys_t session;
    size_t text_size;
    char text[];
} outgoing_metadata_t;

typedef struct listed_client_t
{
    uint64 client_id;
    int64 channel_id;
    int64 audio_state;
    boole is_music_bot;
    boole is_idle;
    boole is_streaming_song;
    boole is_streaming_video;
    boole has_music_keys;
    boole include_music_keys;
    music_bot_audio_keys_t music_keys;
    int* tag_ids;
    int tag_count;
    char username[USERNAME_MAX_LENGTH];
    char alias[USERNAME_MAX_LENGTH];
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    char song_name[SONG_NAME_MAX_LENGTH];
    char country_iso_code[COUNTRY_ISO_CODE_LENGTH];
} listed_client_t;

typedef struct outgoing_client_list_t
{
    crypto_session_keys_t session;
    size_t allocation_size;
    uint64 count;
    char local_username[USERNAME_MAX_LENGTH];
    listed_client_t clients[];
} outgoing_client_list_t;

// declarations
static char* _server_msg_internal__encode_client_list(void* context, size_t* size);
static void _server_msg_internal__release_client_list(void* context);
static char* _server_msg_internal__encode_queued_text(void* context, size_t* size);
static void _server_msg_internal__release_queued_text(void* context);
static void _server_msg_internal__release_encoded_text(char* text);
static int _server_msg_internal__queue_text(ws_cli_conn_t* socket,
    const crypto_session_keys_t* session, const char* plaintext, boole bulk, boole keepalive);
static cJSON* _server_msg_internal__new_message(const char* type, cJSON** out_message);
static void _server_msg_internal__send_text(ws_cli_conn_t* socket, char* dh_shared_secret, char* plaintext);
static void _server_msg_internal__send_json(ws_cli_conn_t* socket, char* dh_shared_secret, cJSON* json_root);
static void _server_msg_internal__send_text_to_client(client_t* client, char* plaintext);
static void _server_msg_internal__send_json_to_client(client_t* client, cJSON* json_root);
static void _server_msg_internal__send_text_to_all_clients(char* plaintext, client_t* except);
static void _server_msg_internal__send_text_to_channel(char* plaintext, uint64 channel_id, client_t* except, cstring receive_type);
static void _server_msg_internal__send_attachment_to_channel(cJSON* root, uint64 channel_id, client_t* except, cstring receive_type);
static void _server_msg_internal__add_attachment_receive_policy(cJSON* message);
static void _server_msg_internal__send_json_to_all_clients(cJSON* json_root, client_t* except);
static void _server_msg_internal__send_json_to_channel(cJSON* json_root, uint64 channel_id, client_t* except);
static cstring _server_msg_internal__country_code_to_send(client_t* client);
static void _server_msg_internal__add_server_info(cJSON* message, boole include_banner, boole banner_changed);

/**
 * @brief Encrypts a queued message using the private copy of its text and keys.
 * This lets a worker prepare it after the caller releases the clients lock,
 * even if the client disconnects or reconnects through another socket meanwhile.
 *
 * @param void* context -> outgoing_metadata_t owned by this queued message
 * @param size_t* size -> receives the encrypted message length, excluding the terminating zero
 *
 * @return char* -> allocated encrypted text for the queue to release, or NULL_POINTER on failure
 */
static char* _server_msg_internal__encode_queued_text(void* context, size_t* size)
{
    outgoing_metadata_t* outgoing = context;
    int64 encoded_size = 0;
    char* encoded = metadata_crypto__encrypt(outgoing->text, &encoded_size,
        outgoing->session.is_ready ? &outgoing->session : NULL_POINTER);

    *size = encoded_size > 0 ? (size_t)encoded_size - 1 : 0;
    return encoded;
}

/**
 * @brief Erases and frees a queued message's private copy of its text and keys.
 *
 * @param void* context -> outgoing_metadata_t to release, or NULL_POINTER
 *
 * @return void
 */
static void _server_msg_internal__release_queued_text(void* context)
{
    outgoing_metadata_t* outgoing = context;

    if (outgoing != NULL_POINTER)
    {
        mbedtls_platform_zeroize(outgoing, sizeof(*outgoing) + outgoing->text_size + 1);
        memorymanager__free((nuint)outgoing);
    }
}

/**
 * @brief Frees the encrypted text after the queue has sent it or cancelled delivery.
 *
 * @param char* text -> encrypted message allocated by metadata_crypto__encrypt
 *
 * @return void
 */
static void _server_msg_internal__release_encoded_text(char* text)
{
    memorymanager__free((nuint)text);
}

/**
 * @brief Copies a message and its connection keys into the socket's outgoing
 * queue so a worker can encrypt it and the socket writer can send it later.
 *
 * @param ws_cli_conn_t* socket -> the chat connection that will receive the message
 * @param const crypto_session_keys_t* session -> connection keys to copy, or NULL_POINTER before the key exchange
 * @param const char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 * @param boole bulk -> TRUE lets file delivery retry when the queue is busy
 * @param boole keepalive -> TRUE sends a connection-check reply before waiting messages
 *
 * @attention The caller holds the clients lock while choosing the socket and
 * copying its keys. The queue takes ownership of the copy, including on failure.
 * Server-wide keys and the message-size limit cannot change after startup, so
 * workers can safely read them without the clients lock.
 *
 * @return int -> 0 queued, 1 busy for file delivery, -1 failed. Being queued
 * does not mean the message has already reached the client.
 */
static int _server_msg_internal__queue_text(ws_cli_conn_t* socket,
    const crypto_session_keys_t* session, const char* plaintext, boole bulk, boole keepalive)
{
    size_t text_size = 0;
    size_t charged_bytes = 0;
    outgoing_metadata_t* outgoing = NULL_POINTER;

    if (socket == NULL_POINTER || plaintext == NULL_POINTER)
    {
        return -1;
    }
    text_size = strlen(plaintext);
    if (text_size > (size_t)g_server_settings.websocket_message_max_length
        || text_size > (SIZE_MAX - sizeof(*outgoing) - 8192) / 5)
    {
        ws_abort_client(socket);
        return -1;
    }
    outgoing = (outgoing_metadata_t*)memorymanager__allocate(
        sizeof(*outgoing) + text_size + 1, MEMALLOC_TYPE_ENCRYPT);
    if (outgoing == NULL_POINTER)
    {
        ws_abort_client(socket);
        return -1;
    }
    outgoing->text_size = text_size;
    memcpy(outgoing->text, plaintext, text_size + 1);
    if (session != NULL_POINTER)
    {
        outgoing->session = *session;
    }
    // Reserve room for the text, encryption buffers, base64 and WebSocket frame.
    // The fixed allowance also covers padding when the message itself is short.
    charged_bytes = sizeof(*outgoing) + text_size * 5 + 8192;
    if (keepalive == TRUE)
    {
        return ws_queue_keepalive(socket, outgoing, charged_bytes,
            _server_msg_internal__encode_queued_text,
            _server_msg_internal__release_queued_text,
            _server_msg_internal__release_encoded_text);
    }
    if (bulk == TRUE)
    {
        return ws_try_queue_text(socket, outgoing, charged_bytes,
            _server_msg_internal__encode_queued_text,
            _server_msg_internal__release_queued_text,
            _server_msg_internal__release_encoded_text);
    }
    return ws_queue_text(socket, outgoing, charged_bytes,
        _server_msg_internal__encode_queued_text,
        _server_msg_internal__release_queued_text,
        _server_msg_internal__release_encoded_text);
}

/**
 * @brief Queues a message for a client's current socket using a private copy of
 * its connection keys and message text for later encryption and delivery.
 *
 * @param client_t* client -> client whose current connection will receive the message
 * @param const char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 *
 * @attention The caller must hold the clients lock until this function returns.
 * The queued message stores its own data and does not keep a client_t pointer.
 *
 * @return boole -> TRUE when the message was queued; FALSE on failure
 */
boole server_msg__queue_text_to_client(client_t* client, const char* plaintext)
{
    crypto_session_keys_t session = {0};
    boole result = FALSE;

    if (client == NULL_POINTER || client->p_ws_connection == NULL_POINTER || plaintext == NULL_POINTER)
    {
        return FALSE;
    }
    if (client->session_keys.is_ready == TRUE)
    {
        return _server_msg_internal__queue_text(client->p_ws_connection, &client->session_keys, plaintext, FALSE, FALSE) == 0;
    }
    if (client->dh_shared_secret[0] != 0)
    {
        if (!metadata_crypto__derive_session_keys(client->dh_shared_secret, &session))
        {
            ws_abort_client(client->p_ws_connection);
            return FALSE;
        }
    }
    result = _server_msg_internal__queue_text(client->p_ws_connection,
        session.is_ready ? &session : NULL_POINTER, plaintext, FALSE, FALSE) == 0;
    metadata_crypto__clear_session_keys(&session);
    return result;
}

/**
 * @brief starts a message: the root object with a "message" object inside that already carries the type
 *
 * @param const char* type -> the message type the client dispatches on
 * @param cJSON** out_message -> receives the "message" object the fields go into
 *
 * @return cJSON* -> the root, to send and then delete
 */
static cJSON* _server_msg_internal__new_message(const char* type, cJSON** out_message)
{
    cJSON* root = cJSON_CreateObject();

    *out_message = cJSON_AddObjectToObject(root, "message");
    cJSON_AddStringToObject(*out_message, "type", type);
    return root;
}

/**
 * @brief Queues a message for a particular chat socket. The message and keys are
 * copied now, then encrypted and sent after this function returns.
 *
 * @param ws_cli_conn_t* socket -> the chat connection that will receive the message
 * @param char* dh_shared_secret -> connection's shared secret, or NULL_POINTER before the key exchange;
 * without a shared secret, only the server's configured keys protect the message
 * @param char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 *
 * @attention The caller holds the clients lock while choosing the socket and secret.
 *
 * @return void
 */
static void _server_msg_internal__send_text(ws_cli_conn_t* socket, char* dh_shared_secret, char* plaintext)
{
    crypto_session_keys_t session = {0};

    if (socket == NULL_POINTER || plaintext == NULL_POINTER)
    {
        return;
    }
    if (dh_shared_secret != NULL_POINTER && dh_shared_secret[0] != 0
        && !metadata_crypto__derive_session_keys(dh_shared_secret, &session))
    {
        ws_abort_client(socket);
        return;
    }
    _server_msg_internal__queue_text(socket, session.is_ready ? &session : NULL_POINTER, plaintext, FALSE, FALSE);
    metadata_crypto__clear_session_keys(&session);
}

/**
 * @brief Queues a JSON message for encrypted delivery to one socket.
 *
 * @param ws_cli_conn_t* socket -> receiving chat connection
 * @param char* dh_shared_secret -> connection secret, or NULL_POINTER before key exchange
 * @param cJSON* json_root -> message owned by the caller
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
static void _server_msg_internal__send_json(ws_cli_conn_t* socket, char* dh_shared_secret, cJSON* json_root)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    _server_msg_internal__send_text(socket, dh_shared_secret, plaintext);
    cJSON_free(plaintext);
}

/**
 * @brief Queues a printed message for encrypted delivery to one client's chat socket.
 *
 * @param client_t* client -> client whose current connection will receive the message
 * @param char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 *
 * @attention The caller must hold the clients lock until the message and keys are copied.
 *
 * @return void
 */
static void _server_msg_internal__send_text_to_client(client_t* client, char* plaintext)
{
    server_msg__queue_text_to_client(client, plaintext);
}

/**
 * @brief Queues a JSON message for encrypted delivery to one client.
 *
 * @param client_t* client -> receiving client
 * @param cJSON* json_root -> message owned by the caller
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
static void _server_msg_internal__send_json_to_client(client_t* client, cJSON* json_root)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    _server_msg_internal__send_text_to_client(client, plaintext);
    cJSON_free(plaintext);
}

/**
 * @brief sends a printed message to every authenticated client that is not a music bot
 *
 * @param char* plaintext -> the message text, printed once by the caller
 * @param client_t* except -> one client that does not get it, or NULL_POINTER
 *
 * @attention clients read lock held by the caller
 *
 * @return void
 */
static void _server_msg_internal__send_text_to_all_clients(char* plaintext, client_t* except)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client == except || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
        {
            continue;
        }
        _server_msg_internal__send_text_to_client(client, plaintext);
    }
}

/**
 * @brief Queues text for authenticated clients in one channel, excluding music bots.
 *
 * @param char* plaintext -> message text to copy
 * @param uint64 channel_id -> receiving channel
 * @param client_t* except -> client to skip, or NULL_POINTER
 * @param cstring receive_type -> attachment type to check, or NULL_POINTER for ordinary messages
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
static void _server_msg_internal__send_text_to_channel(char* plaintext, uint64 channel_id, client_t* except, cstring receive_type)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client == except || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE
            || client->channel_id != channel_id
            || (receive_type != NULL_POINTER && util__is_attachment_blocked(client, receive_type)))
        {
            continue;
        }
        _server_msg_internal__send_text_to_client(client, plaintext);
    }
}

/**
 * @brief Queues a JSON message for all authenticated clients except music bots.
 *
 * @param cJSON* json_root -> message owned by the caller
 * @param client_t* except -> client to skip, or NULL_POINTER
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
static void _server_msg_internal__send_json_to_all_clients(cJSON* json_root, client_t* except)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    _server_msg_internal__send_text_to_all_clients(plaintext, except);
    cJSON_free(plaintext);
}

/**
 * @brief Queues a JSON message for authenticated clients in one channel, excluding music bots.
 *
 * @param cJSON* json_root -> message owned by the caller
 * @param uint64 channel_id -> receiving channel
 * @param client_t* except -> client to skip, or NULL_POINTER
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
static void _server_msg_internal__send_json_to_channel(cJSON* json_root, uint64 channel_id, client_t* except)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    _server_msg_internal__send_text_to_channel(plaintext, channel_id, except, NULL_POINTER);
    cJSON_free(plaintext);
}

/**
 * @brief Announces a channel attachment only to clients who will receive its data.
 * @param cJSON* root -> attachment details owned by the caller
 * @param uint64 channel_id -> receiving channel
 * @param client_t* except -> sender to skip
 * @param cstring receive_type -> channel_chat_picture or channel_chat_file
 * @return void
 * @attention The caller holds the clients lock.
 */
static void _server_msg_internal__send_attachment_to_channel(cJSON* root, uint64 channel_id, client_t* except, cstring receive_type)
{
    char* plaintext = cJSON_PrintUnformatted(root);

    _server_msg_internal__send_text_to_channel(plaintext, channel_id, except, receive_type);
    cJSON_free(plaintext);
}

/**
 * @brief Tells clients which attachment types they are allowed to block.
 * @param cJSON* message -> login, settings or policy message being built
 * @return void
 */
static void _server_msg_internal__add_attachment_receive_policy(cJSON* message)
{
    cJSON_AddBoolToObject(message, "allow_block_direct_chat_pictures", g_server_settings.allow_block_direct_chat_pictures);
    cJSON_AddBoolToObject(message, "allow_block_direct_chat_files", g_server_settings.allow_block_direct_chat_files);
    cJSON_AddBoolToObject(message, "allow_block_channel_chat_pictures", g_server_settings.allow_block_channel_chat_pictures);
    cJSON_AddBoolToObject(message, "allow_block_channel_chat_files", g_server_settings.allow_block_channel_chat_files);
}

/**
 * @brief Adds a music bot's audio keys when the receiver is active in its channel.
 *
 * @param cJSON* object -> message to receive the encryption flag and eligible keys
 * @param client_t* receiver -> client that will receive the message
 * @param client_t* bot -> music bot whose keys are being shared
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void server_msg__add_music_bot_keys(cJSON* object, client_t* receiver, client_t* bot)
{
    cJSON* keys = NULL_POINTER;
    music_bot_audio_keys_t* audio_keys = bot->music_bot_client_extension.audio_keys;
    char key_hex[65] = {0};
    uint64 i = 0;
    uint64 j = 0;

    if (bot->is_music_bot == FALSE || audio_keys == NULL_POINTER)
    {
        return;
    }
    cJSON_AddNumberToObject(object, "music_bot_encryption", 1);
    if (receiver->is_idle == TRUE || receiver->channel_id != bot->channel_id)
    {
        return;
    }
    keys = cJSON_AddArrayToObject(object, "music_bot_keys");
    if (keys == NULL_POINTER)
    {
        return;
    }
    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 32; j++)
        {
            snprintf(key_hex + j * 2, 3, "%02x", audio_keys->keys[i][j]);
        }
        cJSON_AddItemToArray(keys, cJSON_CreateString(key_hex));
    }
}

/**
 * @brief Queues a music bot's audio keys for a client joining its channel.
 *
 * @param client_t* receiver -> active client in the bot's channel
 * @param client_t* bot -> music bot whose keys are being shared
 *
 * @attention The caller holds the clients lock and checks the receiver's channel and idle state.
 *
 * @return void
 */
void server_msg__send_music_bot_key(client_t* receiver, client_t* bot)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    char* plaintext = NULL_POINTER;

    root = _server_msg_internal__new_message("music_bot_key", &message);
    if (message != NULL_POINTER)
    {
        cJSON_AddNumberToObject(message, "client_id", (double)bot->client_id);
        cJSON_AddNumberToObject(message, "channel_id", (double)bot->channel_id);
        server_msg__add_music_bot_keys(message, receiver, bot);
        plaintext = cJSON_PrintUnformatted(root);
    }
    if (plaintext != NULL_POINTER)
    {
        _server_msg_internal__send_text_to_client(receiver, plaintext);
        cJSON_free(plaintext);
    }
    cJSON_Delete(root);
}

/**
 * @brief the country code users may learn about a client: nothing unless flags are displayed, and
 *        nothing for an admin while admin flags are kept private. country blocking resolves the code
 *        for its own check, which must never leak it to users
 *
 * @param client_t* client -> the client being described
 *
 * @return cstring -> the code to send, possibly ""
 */
static cstring _server_msg_internal__country_code_to_send(client_t* client)
{
    if (g_server_settings.is_display_country_flags_active == FALSE)
    {
        return "";
    }

    if (g_server_settings.hide_admin_country_flag == TRUE && client->is_admin == TRUE)
    {
        return "";
    }

    return client->country_iso_code;
}

/**
 * @brief tells a client its rsa key is too weak, naming the required size, so it can offer to
 *        regenerate. only sent when the admin enabled announce_minimum_rsa_key_bits; otherwise
 *        the client is dropped without a word and never learns the requirement.
 *
 * @attention sent in PLAINTEXT on purpose: the key is rejected before the diffie-hellman
 *            exchange completes, so there is no shared secret to encrypt with yet. the message
 *            carries nothing secret - only the already-public policy the admin chose to publish
 *
 * @param client_t* receiver -> the rejected client
 *
 * @return void
 */
void server_msg__send_rsa_key_too_weak_to_single_client(client_t* receiver)
{
    char* plaintext = NULL_POINTER;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("rsa_key_too_weak", &message);
    cJSON_AddNumberToObject(message, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);

    // in plaintext on purpose, see above: no shared secret exists for this socket yet
    plaintext = cJSON_PrintUnformatted(root);
    if (plaintext != NULL_POINTER)
    {
        ws_sendframe_txt(receiver->p_ws_connection, plaintext);
    }
    base__free_json_message(root, plaintext);
}

/**
 * @brief the first message of the key exchange: the challenge the client must sign and the server's diffie hellman mix
 *
 * @param client_t* receiver -> the client that receives it; no shared secret exists for it yet
 * @param char* random_value_challenge_string -> randomly generated string to be sent as challenge
 * @param char* dh_public_mix_for_client -> dh public mix that the client needs to set on its side, result of diffie hellman key exchange
 *
 * @return void
 */
void server_msg__send_public_key_challenge_to_single_client(client_t* receiver, char* random_value_challenge_string, char* dh_public_mix_for_client)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("public_key_challenge", &message);
    cJSON_AddStringToObject(message, "value", random_value_challenge_string);
    cJSON_AddStringToObject(message, "dh_public_mix", dh_public_mix_for_client);

    // the key exchange has not happened yet, so the lowest sender gets no secret
    _server_msg_internal__send_json(receiver->p_ws_connection, NULL_POINTER, root);
    cJSON_Delete(root);
}

/**
 * @brief part of authentication process. Sends authentication status to client along with information whether server is using voice chan and stun port , for UDP voice chat to set on clients side
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @return void
 *
 */
void server_msg__send_authentication_status_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("authentication_status", &message);

    cJSON_AddStringToObject(message, "value", "success");
    cJSON_AddBoolToObject(message, "is_audio_over_websocket_allowed", g_server_settings.is_audio_over_websocket_allowed);
    cJSON_AddBoolToObject(message, "is_audio_over_webrtc_allowed", g_server_settings.is_audio_over_webrtc_allowed);
    cJSON_AddBoolToObject(message, "is_voice_chat_active", g_server_settings.is_voice_chat_active);
    cJSON_AddBoolToObject(message, "is_music_bot_audio_active", g_server_settings.is_music_bot_audio_active);
    // video streaming policy: while off the receiver shows no stream ui at all (icon, popup, channel toggle)
    cJSON_AddBoolToObject(message, "is_video_streaming_allowed", g_server_settings.is_video_streaming_active);
    cJSON_AddBoolToObject(message, "is_idle_mode_allowed", g_server_settings.is_idle_mode_allowed);
    // the receiver attempts a fast reconnect only on a server that allows it
    cJSON_AddBoolToObject(message, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
    cJSON_AddBoolToObject(message, "enforce_connection_check", g_server_settings.enforce_connection_check);
    // the receiver keeps marquees of streaming bots in other channels visible when this is on
    cJSON_AddBoolToObject(message, "show_music_bot_marquee_to_everyone", g_server_settings.show_music_bot_marquee_to_everyone);
    cJSON_AddBoolToObject(message, "is_alias_registration_allowed", g_server_settings.allow_alias_registrations);
    // avatars policy travels in-protocol so clients that were NOT served by this server's http
    // server (the android app loads the page from its assets) still learn it - otherwise they
    // fall back to "avatars off" and hide the set/delete avatar actions
    cJSON_AddBoolToObject(message, "allow_avatars", g_server_settings.allow_avatars);
    cJSON_AddBoolToObject(message, "allow_typing_indicator", g_server_settings.allow_typing_indicator);
    // rename policy travels in-protocol so the receiver can grey its rename input out when users may not rename
    cJSON_AddBoolToObject(message, "allow_client_renames", g_server_settings.allow_client_renames);
    cJSON_AddBoolToObject(message, "allow_link_previews", g_server_settings.allow_link_previews);
    cJSON_AddNumberToObject(message, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
    // the settings tab's icon upload refuses oversize files itself, with the same cap the server enforces
    cJSON_AddNumberToObject(message, "icon_max_size", (double)g_server_settings.icon_max_size_bytes);
    // chat file policy: the receiver refuses oversize/forbidden files itself, with a reason, before uploading
    _server_msg_internal__add_attachment_receive_policy(message);
    cJSON_AddBoolToObject(message, "allow_file_uploads", g_server_settings.allow_file_uploads);
    cJSON_AddNumberToObject(message, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(message, "chat_picture_max_size", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_AddBoolToObject(message, "allow_chat_pictures", g_server_settings.allow_chat_pictures);
    // A masquerading tunnel cannot report the visitor's reflexive address.
    // Clients initiate ICE toward our explicitly advertised public candidates instead.
    cJSON_AddNumberToObject(message, "stun_port", g_server_settings.is_wireguard_mode_active ? 0 : 3478);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
    server_msg__send_server_info_to_single_client(receiver, FALSE,
        (boole)(g_server_settings.server_banner != NULL_POINTER));
}

/**
 * @brief sends one client's info (connected duration, ip, country, identity, last action) to a single
 *        admin client, in reply to a get_client_info request
 *
 * @param client_t* receiver -> the admin the reply is sent to (provides the websocket + shared secret)
 * @param client_t* subject -> the client the info is about
 *
 * @attention caller must hold at least the clients read lock
 *
 * @return void
 */
void server_msg__send_client_info_to_single_client(client_t* receiver, client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    uint64 now_ms = 0;
    uint64 connected_seconds = 0;
    uint64 last_action_seconds_ago = 0;

    now_ms = base__get_timestamp_ms();
    if (now_ms >= subject->timestamp_connected)
    {
        connected_seconds = (now_ms - subject->timestamp_connected) / 1000;
    }
    if (now_ms >= subject->timestamp_last_action)
    {
        last_action_seconds_ago = (now_ms - subject->timestamp_last_action) / 1000;
    }

    root = _server_msg_internal__new_message("client_info", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "connected_seconds", (double)connected_seconds);
    cJSON_AddNumberToObject(message, "last_action_seconds_ago", (double)last_action_seconds_ago);
    cJSON_AddStringToObject(message, "ip_address", &subject->ip_address[0]);
    cJSON_AddStringToObject(message, "country_iso_code", &subject->country_iso_code[0]);
    cJSON_AddStringToObject(message, "identity", &subject->public_key[0]);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells a single client that the channel they tried to join is full (used by the capacity gate)
 *
 * @param client_t* receiver -> the client to notify
 * @param uint64 channel_id -> the full channel
 *
 * @return void
 */
void server_msg__send_channel_full_to_single_client(client_t* receiver, uint64 channel_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("channel_full", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends channel list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @return void
 */
void server_msg__send_channel_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_channel_array = 0;
    uint64 i = 0;
    cJSON* single_channel = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_list", &message);
    json_channel_array = cJSON_CreateArray();

    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        if (g_channel_array[i].is_existing == FALSE)
        {
            continue;
        }

        single_channel = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_channel, "channel_id", g_channel_array[i].channel_id);
        cJSON_AddNumberToObject(single_channel, "parent_channel_id", g_channel_array[i].parent_channel_id);
        cJSON_AddStringToObject(single_channel, "name", g_channel_array[i].name);
        cJSON_AddStringToObject(single_channel, "description", g_channel_array[i].description);
        cJSON_AddBoolToObject(single_channel, "is_using_password", g_channel_array[i].is_using_password);
        cJSON_AddBoolToObject(single_channel, "is_audio_enabled", g_channel_array[i].is_audio_enabled);
        cJSON_AddBoolToObject(single_channel, "is_video_stream_enabled", g_channel_array[i].is_video_stream_enabled);
        cJSON_AddBoolToObject(single_channel, "is_root_channel", g_channel_array[i].is_root_channel);
        cJSON_AddBoolToObject(single_channel, "has_maintainer", g_channel_array[i].is_channel_maintainer_present);
        cJSON_AddBoolToObject(single_channel, "is_temp_channel", g_channel_array[i].is_temp_channel);
        cJSON_AddBoolToObject(single_channel, "is_client_limit_active", g_channel_array[i].is_client_limit_active);
        cJSON_AddNumberToObject(single_channel, "max_client_count", (double)g_channel_array[i].max_client_count);
        cJSON_AddBoolToObject(single_channel, "has_channel_icon", g_channel_array[i].has_channel_icon);
        cJSON_AddNumberToObject(single_channel, "channel_icon_id", (double)g_channel_array[i].icon_id);
        cJSON_AddItemToArray(json_channel_array, single_channel);
    }

    cJSON_AddItemToObject(message, "channels", json_channel_array);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Copies the visible client details so a worker can build the login list later.
 * @param client_t* receiver -> newly authenticated client receiving the list
 * @attention The caller holds the clients and channels locks. The queued copy
 * contains no pointers to live client records, so disconnects cannot change it.
 * @return void
 */
void server_msg__send_client_list_to_single_client(client_t* receiver)
{
    outgoing_client_list_t* outgoing = NULL_POINTER;
    listed_client_t* listed = NULL_POINTER;
    client_t* client = NULL_POINTER;
    size_t allocation_size = sizeof(outgoing_client_list_t);
    size_t tag_count = 0;
    size_t count = 0;
    uint64 i = 0;
    int* tags = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client->is_existing == FALSE || client->is_authenticated == FALSE)
        {
            continue;
        }
        tag_count = cvector_size(client->tag_ids);
        if (tag_count > INT_MAX || allocation_size > SIZE_MAX - sizeof(listed_client_t)
            || tag_count > (SIZE_MAX - allocation_size - sizeof(listed_client_t)) / sizeof(int))
        {
            ws_abort_client(receiver->p_ws_connection);
            return;
        }
        allocation_size += sizeof(listed_client_t) + tag_count * sizeof(int);
        count++;
    }
    if (allocation_size > (SIZE_MAX - 8192) / 6 || !receiver->session_keys.is_ready)
    {
        ws_abort_client(receiver->p_ws_connection);
        return;
    }
    outgoing = (outgoing_client_list_t*)memorymanager__allocate(allocation_size, MEMALLOC_TYPE_ENCRYPT);
    if (outgoing == NULL_POINTER)
    {
        ws_abort_client(receiver->p_ws_connection);
        return;
    }
    outgoing->allocation_size = allocation_size;
    outgoing->count = count;
    outgoing->session = receiver->session_keys;
    memcpy(outgoing->local_username, receiver->username, sizeof(outgoing->local_username));
    tags = (int*)(outgoing->clients + count);
    count = 0;
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client->is_existing == FALSE || client->is_authenticated == FALSE)
        {
            continue;
        }
        listed = &outgoing->clients[count++];
        listed->client_id = client->client_id;
        listed->channel_id = (int64)client->channel_id;
        listed->audio_state = client->audio_state;
        listed->is_music_bot = client->is_music_bot;
        listed->is_idle = client->is_idle;
        listed->is_streaming_song = client->is_streaming_song;
        listed->is_streaming_video = client->is_streaming_video;
        memcpy(listed->username, client->username, sizeof(listed->username));
        memcpy(listed->alias, client->alias, sizeof(listed->alias));
        memcpy(listed->public_key, client->public_key, sizeof(listed->public_key));
        memcpy(listed->song_name, client->song_name, sizeof(listed->song_name));
        snprintf(listed->country_iso_code, sizeof(listed->country_iso_code), "%s",
            _server_msg_internal__country_code_to_send(client));
        listed->tag_count = (int)cvector_size(client->tag_ids);
        if (listed->tag_count != 0)
        {
            listed->tag_ids = tags;
            memcpy(tags, client->tag_ids, (size_t)listed->tag_count * sizeof(int));
            tags += listed->tag_count;
        }
        // Decide what this receiver may see while the channel settings are locked.
        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE
            && receiver->channel_id != client->channel_id
            && client->channel_id < g_server_settings.max_channel_count
            && g_channel_array[client->channel_id].is_existing == TRUE
            && g_channel_array[client->channel_id].is_using_password == TRUE)
        {
            listed->channel_id = -1;
        }
        if (listed->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE && client->channel_id != ROOT_CHANNEL_ID)
        {
            listed->audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        if (client->is_music_bot == TRUE && client->music_bot_client_extension.audio_keys != NULL_POINTER)
        {
            listed->has_music_keys = TRUE;
            if (receiver->is_idle == FALSE && receiver->channel_id == client->channel_id)
            {
                listed->include_music_keys = TRUE;
                listed->music_keys = *client->music_bot_client_extension.audio_keys;
            }
        }
    }
    ws_queue_text(receiver->p_ws_connection, outgoing, allocation_size * 6 + 8192,
        _server_msg_internal__encode_client_list, _server_msg_internal__release_client_list,
        _server_msg_internal__release_encoded_text);
}

/**
 * @brief Erases and frees the client-list copy, including its copied connection and music keys.
 * @param void* context -> client-list copy owned by the outgoing queue
 * @return void
 */
static void _server_msg_internal__release_client_list(void* context)
{
    outgoing_client_list_t* outgoing = context;
    size_t allocation_size = outgoing->allocation_size;

    mbedtls_platform_zeroize(outgoing, allocation_size);
    memorymanager__free((nuint)outgoing);
}

/**
 * @brief Builds and encrypts the copied client list without taking the clients lock.
 * @param void* context -> client details and connection keys copied during login
 * @param size_t* size -> receives the encrypted message length
 * @return char* -> encrypted message owned by the outgoing queue, or NULL_POINTER
 */
static char* _server_msg_internal__encode_client_list(void* context, size_t* size)
{
    outgoing_client_list_t* outgoing = context;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_client_array = 0;
    cJSON* json_tag_ids_array = 0;
    listed_client_t* client_in_loop = 0;
    cJSON* single_client = NULL_POINTER;
    cJSON* music_keys = NULL_POINTER;
    char key_hex[65] = {0};
    char* plaintext = NULL_POINTER;
    char* encoded = NULL_POINTER;
    int64 encoded_size = 0;
    uint key_index = 0;
    uint byte_index = 0;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("client_list", &message);
    json_client_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < outgoing->count; x++)
    {
        client_in_loop = &outgoing->clients[x];
        single_client = cJSON_CreateObject();
        cJSON_AddStringToObject(single_client, "username", client_in_loop->username);
        cJSON_AddStringToObject(single_client, "public_key", client_in_loop->public_key);

        cJSON_AddNumberToObject(single_client, "channel_id", (double)client_in_loop->channel_id);

        cJSON_AddNumberToObject(single_client, "client_id", (double)client_in_loop->client_id);

        cJSON_AddBoolToObject(single_client, "is_music_bot", client_in_loop->is_music_bot);
        if (client_in_loop->has_music_keys == TRUE)
        {
            cJSON_AddNumberToObject(single_client, "music_bot_encryption", 1);
        }
        if (client_in_loop->include_music_keys == TRUE)
        {
            music_keys = cJSON_AddArrayToObject(single_client, "music_bot_keys");
            for (key_index = 0; key_index < 4; key_index++)
            {
                for (byte_index = 0; byte_index < 32; byte_index++)
                {
                    snprintf(key_hex + byte_index * 2, 3, "%02x", client_in_loop->music_keys.keys[key_index][byte_index]);
                }
                cJSON_AddItemToArray(music_keys, cJSON_CreateString(key_hex));
            }
        }
        cJSON_AddNumberToObject(single_client, "audio_state", (double)client_in_loop->audio_state);

        if (client_in_loop->tag_ids != NULL_POINTER)
        {
            json_tag_ids_array = cJSON_CreateIntArray(client_in_loop->tag_ids, client_in_loop->tag_count);
            cJSON_AddItemToObject(single_client, "tag_ids", json_tag_ids_array);
        }
        else
        {
            json_tag_ids_array = cJSON_CreateArray();
            cJSON_AddItemToObject(single_client, "tag_ids", json_tag_ids_array);
        }

        cJSON_AddBoolToObject(single_client, "is_idle", client_in_loop->is_idle);

        // admin-registered alias (display name); empty string when none
        cJSON_AddStringToObject(single_client, "alias", &client_in_loop->alias[0]);

        // property country_iso_code will always be part of response; empty for an unknown country,
        // whenever flags are not displayed, and for an admin when admin flags are kept private
        cJSON_AddStringToObject(single_client, "country_iso_code", client_in_loop->country_iso_code);

        // a stream already running when somebody logs in: the list is the only place a late
        // joiner can learn about it, start_song_stream went out before the joiner was here
        cJSON_AddBoolToObject(single_client, "is_streaming_song", client_in_loop->is_streaming_song);
        cJSON_AddBoolToObject(single_client, "is_streaming_video", client_in_loop->is_streaming_video);
        cJSON_AddStringToObject(single_client, "song_name", client_in_loop->song_name);

        cJSON_AddItemToArray(json_client_array, single_client);
    }

    cJSON_AddItemToObject(message, "clients", json_client_array);
    cJSON_AddStringToObject(message, "local_username", outgoing->local_username);
    plaintext = cJSON_PrintUnformatted(root);
    if (plaintext != NULL_POINTER)
    {
        encoded = metadata_crypto__encrypt(plaintext, &encoded_size, &outgoing->session);
    }
    cJSON_free(plaintext);
    cJSON_Delete(root);
    *size = encoded_size > 0 ? (size_t)encoded_size - 1 : 0;
    return encoded;
}

/**
 * @brief This function sends icon list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_icon_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_icons_array = 0;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("icon_list", &message);
    json_icons_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_ICONS; x++)
    {
        cJSON* single_client = NULL_POINTER;

        if (g_icons_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_client = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_client, "icon_id", (double)g_icons_array[x].id);
        cJSON_AddStringToObject(single_client, "base64_icon", (g_icons_array[x].base64 != NULL_POINTER) ? g_icons_array[x].base64 : "");
        cJSON_AddItemToArray(json_icons_array, single_client);
    }

    cJSON_AddItemToObject(message, "icons", json_icons_array);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends the available tags to a client after login.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_tag_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_tags_array = 0;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("tag_list", &message);
    json_tags_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_TAGS; x++)
    {
        cJSON* single_tag_id_object = NULL_POINTER;

        if (g_tags_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_tag_id_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_tag_id_object, "tag_id", (double)g_tags_array[x].id);
        cJSON_AddStringToObject(single_tag_id_object, "tag_name", g_tags_array[x].name);
        cJSON_AddNumberToObject(single_tag_id_object, "tag_linked_icon_id", (double)g_tags_array[x].icon_id);
        cJSON_AddItemToObject(single_tag_id_object, "has_icon", cJSON_CreateBool(g_tags_array[x].has_icon == TRUE));
        cJSON_AddItemToArray(json_tags_array, single_tag_id_object);
    }

    cJSON_AddItemToObject(message, "tags", json_tags_array);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends the admin identity-management list: every stored identity as { public_key_hash,
 *        tag_ids[], is_online }. is_online is TRUE when a currently-connected authenticated client
 *        hashes to the same identity. admin-only; the caller checks that and holds the clients lock.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @note the caller must hold the clients read lock (g_clients_array is read for online status). the
 *       identity store is guarded by g_client_stored_data_mutex, taken here.
 *
 * @return void
 */
void server_msg__send_identity_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_identities_array = 0;
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 c = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("identity_list", &message);
    json_identities_array = cJSON_CreateArray();

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        cJSON* single_identity_object = NULL_POINTER;
        cJSON* identity_tag_ids_array = NULL_POINTER;
        boole is_online = FALSE;

        // an identity is worth listing when it carries ANYTHING: tags, an alias or an avatar. it used
        // to require tags, which hid every registered user from the admin - registration grants an
        // alias, not a tag, so the very people this list is for were the ones missing from it
        if (g_client_stored_data[i].public_key[0] == 0
            || (g_client_stored_data[i].tag_id_count == 0 && g_client_stored_data[i].alias[0] == 0 && g_client_stored_data[i].base64_avatar[0] == 0))
        {
            continue;
        }

        single_identity_object = cJSON_CreateObject();
        cJSON_AddStringToObject(single_identity_object, "public_key_hash", &g_client_stored_data[i].public_key[0]);
        cJSON_AddStringToObject(single_identity_object, "username", &g_client_stored_data[i].username[0]);
        // the admin-granted display name; empty means this identity is not registered
        cJSON_AddStringToObject(single_identity_object, "alias", &g_client_stored_data[i].alias[0]);

        identity_tag_ids_array = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(identity_tag_ids_array, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(single_identity_object, "tag_ids", identity_tag_ids_array);

        // mark online if any connected authenticated non-bot receiver hashes to this identity
        for (c = 0; c < g_server_settings.max_client_count; c++)
        {
            client_t* connected_client = &g_clients_array[c];

            if (connected_client->is_existing == FALSE || connected_client->is_authenticated == FALSE || connected_client->is_music_bot == TRUE)
            {
                continue;
            }
            if (connected_client->public_key[0] == 0)
            {
                continue;
            }

            base__hash_password_to_base64(connected_client->public_key, client_hash, sizeof(client_hash));
            if (clib__is_string_equal(client_hash, &g_client_stored_data[i].public_key[0]) == TRUE)
            {
                is_online = TRUE;
                break;
            }
        }
        cJSON_AddItemToObject(single_identity_object, "is_online", cJSON_CreateBool(is_online == TRUE));

        cJSON_AddItemToArray(json_identities_array, single_identity_object);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    cJSON_AddItemToObject(message, "identities", json_identities_array);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends the microphone states of clients in the selected channel.
 *
 * @param client_t* receiver -> the client that receives it
 * @param uint64 channel_id -> channel whose microphone states are reported
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_t* receiver, uint64 channel_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_clients_array = 0;
    uint64 x = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("current_channel_active_microphone_usage", &message);
    json_clients_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        cJSON* single_object = NULL_POINTER;

        client_in_loop = &g_clients_array[x];
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->channel_id != channel_id)
        {
            continue;
        }

        // have to send even to ourselves
        // if receiver.client_id == current_client_id {
        // continue;
        // }

        // only active mics are relevant since this is "active microphone usage"
        // though
        if (client_in_loop->audio_state != AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            continue;
        }

        single_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_object, "client_id", client_in_loop->client_id);
        cJSON_AddNumberToObject(single_object, "audio_state", client_in_loop->audio_state);
        cJSON_AddBoolToObject(single_object, "is_streaming_song", client_in_loop->is_streaming_song);
        cJSON_AddBoolToObject(single_object, "is_streaming_video", client_in_loop->is_streaming_video);
        cJSON_AddStringToObject(single_object, "song_name", client_in_loop->song_name);

        cJSON_AddItemToArray(json_clients_array, single_object);
    }

    cJSON_AddItemToObject(message, "clients", json_clients_array);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Announces a connected client, building each version of the message only once.
 *
 * @param client_t* subject -> the client that connected
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * Recipients in other channels must not receive active microphone state or
 * music bot keys. Those recipients receive a separate version of the message.
 *
 * @return void
 */
void server_msg__send_client_connect_message_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_tag_ids_array = 0;
    uint64 x = 0;
    client_t* client_in_loop = 0;
    char* texts[4] = {0};
    uint variant = 0;
    int64 audio_state_to_send = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop == subject)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        variant = 0;
        if (subject->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE && client_in_loop->channel_id != subject->channel_id)
        {
            variant |= 1;
        }
        if (subject->is_music_bot == TRUE && client_in_loop->is_idle == FALSE
            && client_in_loop->channel_id == subject->channel_id)
        {
            variant |= 2;
        }
        if (texts[variant] != NULL_POINTER)
        {
            _server_msg_internal__send_text_to_client(client_in_loop, texts[variant]);
            continue;
        }

        root = _server_msg_internal__new_message("client_connect", &message);
        // carry the connecting client's tags so identity-restored tags show up on join without a page reload
        json_tag_ids_array = cJSON_CreateIntArray(subject->tag_ids, (int)cvector_size(subject->tag_ids));

        server_msg__add_music_bot_keys(message, client_in_loop, subject);
        cJSON_AddStringToObject(message, "username", subject->username);
        cJSON_AddStringToObject(message, "alias", subject->alias);
        cJSON_AddStringToObject(message, "public_key", subject->public_key);
        cJSON_AddNumberToObject(message, "channel_id", (double)subject->channel_id);
        cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
        cJSON_AddBoolToObject(message, "is_music_bot", subject->is_music_bot);

        audio_state_to_send = subject->audio_state;

        // only send the "PUSH_TO_TALK_ACTIVE" state to the connected client for other clients in the same channel as the client (the root channel in this case)
        // privacy reasons
        if (subject->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE && client_in_loop->channel_id != subject->channel_id)
        {
            audio_state_to_send = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }

        cJSON_AddNumberToObject(message, "audio_state", audio_state_to_send);
        cJSON_AddStringToObject(message, "country_iso_code", _server_msg_internal__country_code_to_send(subject));
        cJSON_AddItemToObject(message, "tag_ids", json_tag_ids_array);

        texts[variant] = cJSON_PrintUnformatted(root);
        if (texts[variant] != NULL_POINTER)
        {
            _server_msg_internal__send_text_to_client(client_in_loop, texts[variant]);
        }
        cJSON_Delete(root);
    }
    for (variant = 0; variant < 4; variant++)
    {
        cJSON_free(texts[variant]);
    }
}

/**
 * @brief This function sends the maintainer of the current channel the client just joined. Used when the client joins the server or when it switches channel
 *
 * @param client_t* receiver -> the client to send the maintainer id to
 * @param channel_t* channel -> the channel
 * @param uint64 maintainer_id -> the maintainer client id to announce, as a payload value
 *
 * @attention sometimes parameter id_of_client_that_joined_the_channel can have the value as id_of_client_that_is_maintainer_of_channel
 *
 * @return void
 */
void server_msg__send_maintainer_id_to_single_client(client_t* receiver, channel_t* channel, uint64 maintainer_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_maintainer_id", &message);

    cJSON_AddNumberToObject(message, "maintainer_id", (double)maintainer_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Queues a keepalive reply ahead of waiting uploads and announcements.
 * @param client_t* receiver -> authenticated connection, kept alive by the clients lock
 * @return void
 */
void server_msg__send_connection_check_response_to_single_client(client_t* receiver)
{
    _server_msg_internal__queue_text(receiver->p_ws_connection, &receiver->session_keys,
        "{\"message\":{\"type\":\"connection_check_response\"}}", FALSE, TRUE);
}

/**
 * @brief tells a returning client that its session was adopted (fast reconnect), so it keeps its
 *        state and treats the lists that follow as a refresh. sent before authentication_status
 *
 * @param client_t* receiver -> the resumed session, already carrying the new socket
 *
 * @return void
 */
void server_msg__send_fast_reconnect_ok_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("fast_reconnect_ok", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)receiver->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)receiver->channel_id);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells a client the server will build no datachannel for it for a while: its attempts kept
 *        failing (blocked udp, usually). the client sleeps that long instead of asking every 10 s
 *
 * @param client_t* receiver -> the client whose request was refused
 * @param uint64 seconds_left -> how long the refusal lasts
 *
 * @return void
 */
void server_msg__send_datachannel_cooldown_to_single_client(client_t* receiver, uint64 seconds_left)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("datachannel_cooldown", &message);

    cJSON_AddNumberToObject(message, "seconds_left", (double)seconds_left);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends client_rename_message to all authenticated clients
 *
 * @param client_t* subject -> the client that changed its username
 * @param char* new_username -> self explanatory
 *
 * @attention this function is used within acquired readlock within client_message.c client_msg__process_change_client_username function
 *
 * @return void
 */
void server_msg__send_client_rename_message_to_all_clients(client_t* subject, char* new_username)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("client_rename", &message);

    cJSON_AddStringToObject(message, "new_username", new_username);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief tells a single client it should change the admin password (sent once after the first admin login,
 *        because the initial password was typed in cleartext at setup)
 *
 * @param client_t* receiver -> the client to notify
 *
 * @return void
 */
void server_msg__send_force_admin_password_change_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("force_admin_password_change", &message);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends an access-denied message to a single client
 *
 * @param client_t* receiver -> self explanatory
 * @param char* reason -> optional text the client shows instead of its generic denial line; NULL omits it
 *
 * @attention this function is used within acquired readlock within client_message.c , multiple functions
 *
 * @return void
 */
void server_msg__send_access_denied_to_single_client(client_t* receiver, char* reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("access_denied", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    if (reason != NULL_POINTER && reason[0] != 0)
    {
        cJSON_AddStringToObject(message, "reason", reason);
    }

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief replies to a single client with the current general-settings toggle values, so the client can
 *        reflect the live server state (used when an admin opens the general-settings tab)
 *
 * @param client_t* receiver -> authenticated administrator receiving the reply
 * @param boole memory_only -> TRUE refreshes RAM, slots and uptime without replacing edited settings
 *
 * @return void
 */
void server_msg__send_server_settings_to_single_client(client_t* receiver, boole memory_only)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_bans = 0;
    cJSON* json_ban = 0;
    cJSON* json_blocked_countries = 0;
    ban_entry_t* ban_in_loop = 0;
    uint64 i = 0;
    uint64 used_slots = 0;
    uint64 ram_bytes = 0;
    uint64 webrtc_received = 0;
    uint64 webrtc_sent = 0;

    root = _server_msg_internal__new_message("server_settings_values", &message);
    util__get_webrtc_bytes(&webrtc_received, &webrtc_sent);
    cJSON_AddNumberToObject(message, "websocket_received_bytes", (double)ws_get_received_bytes());
    cJSON_AddNumberToObject(message, "websocket_sent_bytes", (double)ws_get_sent_bytes());
    cJSON_AddNumberToObject(message, "webrtc_received_bytes", (double)webrtc_received);
    cJSON_AddNumberToObject(message, "webrtc_sent_bytes", (double)webrtc_sent);
    if (util__get_process_ram_usage(&ram_bytes))
    {
        cJSON_AddNumberToObject(message, "ram_bytes", (double)ram_bytes);
    }
    else
    {
        cJSON_AddNullToObject(message, "ram_bytes");
    }
    cJSON_AddNumberToObject(message, "uptime_seconds", (double)base__get_server_uptime_seconds());
    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == TRUE || g_clients_array[i].timestamp_connected != 0)
        {
            used_slots++;
        }
    }
    cJSON_AddNumberToObject(message, "used_slots", (double)used_slots);
    cJSON_AddNumberToObject(message, "max_slots", (double)g_server_settings.max_client_count);
    if (memory_only)
    {
        cJSON_AddBoolToObject(message, "memory_only", TRUE);
        _server_msg_internal__send_json_to_client(receiver, root);
        cJSON_Delete(root);
        return;
    }
    _server_msg_internal__add_attachment_receive_policy(message);
    cJSON_AddBoolToObject(message, "publish_server_uptime", g_server_settings.publish_server_uptime);
    cJSON_AddBoolToObject(message, "publish_server_slots", g_server_settings.publish_server_slots);
    cJSON_AddBoolToObject(message, "publish_server_banner", g_server_settings.publish_server_banner);
    cJSON_AddBoolToObject(message, "publish_server_features", g_server_settings.publish_server_features);
    cJSON_AddStringToObject(message, "server_banner", g_server_settings.server_banner != NULL_POINTER ? g_server_settings.server_banner : "");
    cJSON_AddItemToObject(message, "display_country_flags", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(message, "hide_admin_country_flag", cJSON_CreateBool(g_server_settings.hide_admin_country_flag == TRUE));
    cJSON_AddItemToObject(message, "enable_audio", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddStringToObject(message, "audio_connections", audio_channel__policy_value());
    cJSON_AddItemToObject(message, "enable_music_bot_audio", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_AddItemToObject(message, "is_video_streaming_active", cJSON_CreateBool(g_server_settings.is_video_streaming_active == TRUE));
    cJSON_AddItemToObject(message, "hide_clients_in_password_channels", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(message, "allow_temp_channels", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_AddItemToObject(message, "allow_typing_indicator", cJSON_CreateBool(g_server_settings.allow_typing_indicator == TRUE));
    cJSON_AddItemToObject(message, "allow_client_renames", cJSON_CreateBool(g_server_settings.allow_client_renames == TRUE));
    cJSON_AddBoolToObject(message, "allow_link_previews", g_server_settings.allow_link_previews);
    cJSON_AddItemToObject(message, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_AddItemToObject(message, "allow_private_messages", cJSON_CreateBool(g_server_settings.allow_private_messages == TRUE));
    cJSON_AddItemToObject(message, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_AddBoolToObject(message, "enforce_connection_check", g_server_settings.enforce_connection_check);
    cJSON_AddNumberToObject(message, "max_concurrent_logins", (double)g_server_settings.max_concurrent_logins);
    cJSON_AddNumberToObject(message, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds);
    cJSON_AddNumberToObject(message, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes);
    cJSON_AddItemToObject(message, "show_music_bot_marquee_to_everyone", cJSON_CreateBool(g_server_settings.show_music_bot_marquee_to_everyone == TRUE));
    cJSON_AddNumberToObject(message, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_AddItemToObject(message, "announce_minimum_rsa_key_bits", cJSON_CreateBool(g_server_settings.announce_minimum_rsa_key_bits == TRUE));
    cJSON_AddItemToObject(message, "allow_file_uploads", cJSON_CreateBool(g_server_settings.allow_file_uploads == TRUE));
    cJSON_AddNumberToObject(message, "file_upload_max_size_mb", (double)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)));
    cJSON_AddNumberToObject(message, "chat_picture_max_size_mb", (double)(g_server_settings.chat_picture_max_size_bytes / (1024 * 1024)));
    cJSON_AddItemToObject(message, "allow_chat_pictures", cJSON_CreateBool(g_server_settings.allow_chat_pictures == TRUE));
    cJSON_AddItemToObject(message, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    json_blocked_countries = cJSON_CreateArray();
    cJSON_AddItemToObject(message, "blocked_countries", json_blocked_countries);
    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        cJSON_AddItemToArray(json_blocked_countries, cJSON_CreateString(&g_server_settings.blocked_countries[i][0]));
    }
    cJSON_AddItemToObject(message, "log_client_joins", cJSON_CreateBool(g_server_settings.log_client_joins == TRUE));
    cJSON_AddItemToObject(message, "log_username_changes", cJSON_CreateBool(g_server_settings.log_username_changes == TRUE));
    cJSON_AddItemToObject(message, "log_tag_changes", cJSON_CreateBool(g_server_settings.log_tag_changes == TRUE));
    cJSON_AddItemToObject(message, "log_server_settings_updates", cJSON_CreateBool(g_server_settings.log_server_settings_updates == TRUE));
    cJSON_AddItemToObject(message, "log_kicks_and_bans", cJSON_CreateBool(g_server_settings.log_kicks_and_bans == TRUE));
    cJSON_AddItemToObject(message, "log_client_disconnects", cJSON_CreateBool(g_server_settings.log_client_disconnects == TRUE));
    cJSON_AddItemToObject(message, "log_socket_opens_and_closes", cJSON_CreateBool(g_server_settings.log_socket_opens_and_closes == TRUE));
    cJSON_AddItemToObject(message, "log_failed_attempts", cJSON_CreateBool(g_server_settings.log_failed_attempts == TRUE));
    cJSON_AddNumberToObject(message, "admin_log_max_size_mb", (double)(g_server_settings.admin_log_max_size_bytes / (1024 * 1024)));
    cJSON_AddNumberToObject(message, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);

    // include the current ban list so the admin's bans section can render it
    json_bans = cJSON_CreateArray();
    cJSON_AddItemToObject(message, "bans", json_bans);
    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        ban_in_loop = &g_ban_array[i];
        if (ban_in_loop->is_existing == FALSE)
        {
            continue;
        }
        json_ban = cJSON_CreateObject();
        cJSON_AddStringToObject(json_ban, "ip_address", &ban_in_loop->ip_address[0]);
        cJSON_AddStringToObject(json_ban, "country_iso_code", &ban_in_loop->country_iso_code[0]);
        cJSON_AddStringToObject(json_ban, "identity", &ban_in_loop->identity[0]);
        cJSON_AddStringToObject(json_ban, "extra_data", &ban_in_loop->extra_data[0]);
        cJSON_AddNumberToObject(json_ban, "timestamp_banned", (double)ban_in_loop->timestamp_banned);
        cJSON_AddItemToArray(json_bans, json_ban);
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Adds only the server information the administrator has chosen to share.
 * @param cJSON* message -> message object to fill
 * @param boole include_banner -> include the saved banner when TRUE
 * @param boole banner_changed -> ask visible clients to refresh their banner when TRUE
 * @return void
 * @attention The caller holds the clients lock while reading settings and slots.
 */
static void _server_msg_internal__add_server_info(cJSON* message, boole include_banner, boole banner_changed)
{
    cJSON* visibility = NULL_POINTER;
    cJSON* features = NULL_POINTER;
    uint64 used_slots = 0;
    uint64 i = 0;

    visibility = cJSON_AddObjectToObject(message, "visibility");
    cJSON_AddBoolToObject(visibility, "uptime", g_server_settings.publish_server_uptime);
    cJSON_AddBoolToObject(visibility, "slots", g_server_settings.publish_server_slots);
    cJSON_AddBoolToObject(visibility, "banner", g_server_settings.publish_server_banner);
    cJSON_AddBoolToObject(visibility, "features", g_server_settings.publish_server_features);
    if (g_server_settings.publish_server_uptime == TRUE)
    {
        cJSON_AddNumberToObject(message, "uptime_seconds", (double)base__get_server_uptime_seconds());
    }
    if (g_server_settings.publish_server_slots == TRUE)
    {
        for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
        {
            if (g_clients_array[i].is_existing == TRUE || g_clients_array[i].timestamp_connected != 0)
            {
                used_slots++;
            }
        }
        cJSON_AddNumberToObject(message, "used_slots", (double)used_slots);
        cJSON_AddNumberToObject(message, "max_slots", (double)g_server_settings.max_client_count);
    }
    if (g_server_settings.publish_server_banner == TRUE && include_banner == TRUE)
    {
        cJSON_AddStringToObject(message, "server_banner", g_server_settings.server_banner != NULL_POINTER ? g_server_settings.server_banner : "");
    }
    if (g_server_settings.publish_server_banner == TRUE && banner_changed == TRUE)
    {
        cJSON_AddBoolToObject(message, "banner_changed", TRUE);
    }
    if (g_server_settings.publish_server_features == FALSE)
    {
        return;
    }

    // Only public feature switches belong here. Passwords, keys, bans and
    // administrator-only settings must stay in their existing private replies.
    features = cJSON_AddObjectToObject(message, "features");
    cJSON_AddBoolToObject(features, "is_voice_chat_active", g_server_settings.is_voice_chat_active);
    cJSON_AddBoolToObject(features, "is_music_bot_audio_active", g_server_settings.is_music_bot_audio_active);
    cJSON_AddBoolToObject(features, "is_video_streaming_allowed", g_server_settings.is_video_streaming_active);
    cJSON_AddBoolToObject(features, "is_audio_over_websocket_allowed", g_server_settings.is_audio_over_websocket_allowed);
    cJSON_AddBoolToObject(features, "is_audio_over_webrtc_allowed", g_server_settings.is_audio_over_webrtc_allowed);
    cJSON_AddBoolToObject(features, "is_idle_mode_allowed", g_server_settings.is_idle_mode_allowed);
    cJSON_AddBoolToObject(features, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
    cJSON_AddBoolToObject(features, "enforce_connection_check", g_server_settings.enforce_connection_check);
    cJSON_AddBoolToObject(features, "allow_typing_indicator", g_server_settings.allow_typing_indicator);
    cJSON_AddBoolToObject(features, "allow_client_renames", g_server_settings.allow_client_renames);
    cJSON_AddBoolToObject(features, "allow_link_previews", g_server_settings.allow_link_previews);
    cJSON_AddBoolToObject(features, "allow_avatars", g_server_settings.allow_avatars);
    cJSON_AddBoolToObject(features, "allow_private_messages", g_server_settings.allow_private_messages);
    cJSON_AddBoolToObject(features, "allow_file_uploads", g_server_settings.allow_file_uploads);
    cJSON_AddBoolToObject(features, "allow_chat_pictures", g_server_settings.allow_chat_pictures);
    cJSON_AddBoolToObject(features, "allow_temp_channels", g_server_settings.is_temp_channel_creation_allowed);
    cJSON_AddBoolToObject(features, "are_identities_enabled", g_server_settings.are_identities_enabled);
    cJSON_AddBoolToObject(features, "is_alias_registration_allowed", g_server_settings.are_identities_enabled && g_server_settings.allow_alias_registrations);
    cJSON_AddBoolToObject(features, "allow_stored_clients_list", g_server_settings.are_identities_enabled && g_server_settings.allow_stored_clients_list);
    cJSON_AddBoolToObject(features, "allow_last_seen", g_server_settings.are_identities_enabled && g_server_settings.allow_last_seen);
    cJSON_AddBoolToObject(features, "allow_offline_messages", g_server_settings.are_identities_enabled && g_server_settings.allow_stored_clients_list && g_server_settings.allow_offline_messages);
    cJSON_AddBoolToObject(features, "display_country_flags", g_server_settings.is_display_country_flags_active);
    cJSON_AddBoolToObject(features, "hide_clients_in_password_channels", g_server_settings.is_hide_clients_in_password_protected_channels_active);
    cJSON_AddBoolToObject(features, "is_sending_text_to_idle_clients_allowed", g_server_settings.is_sending_text_to_idle_clients_allowed);
    cJSON_AddBoolToObject(features, "show_music_bot_marquee_to_everyone", g_server_settings.show_music_bot_marquee_to_everyone);
}

/**
 * @brief Sends public server information to an authenticated client.
 * @param client_t* receiver -> client that requested the information
 * @param boole include_banner -> include the saved banner when TRUE
 * @param boole banner_changed -> tell the client its cached banner needs refreshing
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__send_server_info_to_single_client(client_t* receiver, boole include_banner, boole banner_changed)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("server_info", &message);
    _server_msg_internal__add_server_info(message, include_banner, banner_changed);
    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Updates every client's public server information, including users in idle mode.
 * @param boole banner_changed -> ask clients to refresh the banner when it is published
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__send_server_info_to_all_clients(boole banner_changed)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("server_info", &message);
    _server_msg_internal__add_server_info(message, FALSE, banner_changed);
    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);
    cJSON_Delete(root);
}

/**
 * @brief Announces a changed banner only when the administrator publishes it.
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__send_server_banner_changed_to_all_clients(void)
{
    if (g_server_settings.publish_server_banner == TRUE)
    {
        server_msg__send_server_info_to_all_clients(TRUE);
    }
}

/**
 * @brief Tells an administrator whether saving the banner succeeded.
 * @param client_t* receiver -> administrator that sent the change
 * @param boole success -> TRUE when the banner was saved
 * @param cstring error -> short failure code, or an empty string on success
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__send_server_banner_result_to_single_client(client_t* receiver, boole success, cstring error)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("server_banner_result", &message);
    cJSON_AddBoolToObject(message, "success", success);
    cJSON_AddStringToObject(message, "error", error);
    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief replies to a single client with every admin log line, oldest first (used when an admin
 *        opens or refreshes the log tab). the caller must have checked the client is an admin
 *
 * @param client_t* receiver -> the client to reply to
 *
 * @return void
 */
void server_msg__send_admin_log_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_lines = 0;

    json_lines = server_logs__build_json_array();
    if (json_lines == NULL_POINTER)
    {
        return;
    }

    root = _server_msg_internal__new_message("admin_log", &message);

    cJSON_AddItemToObject(message, "lines", json_lines);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts the client-relevant policy values (file/picture limits, avatars, typing,
 *        aliases) to every connected client, so a settings change applies without a reconnect
 *
 * @note the caller must hold the clients read lock
 *
 * @return void
 */
void server_msg__send_policy_update_to_all_clients(void)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("server_policy", &message);
    _server_msg_internal__add_attachment_receive_policy(message);

    cJSON_AddBoolToObject(message, "allow_file_uploads", g_server_settings.allow_file_uploads);
    cJSON_AddNumberToObject(message, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(message, "chat_picture_max_size", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_AddBoolToObject(message, "allow_chat_pictures", g_server_settings.allow_chat_pictures);
    cJSON_AddBoolToObject(message, "allow_typing_indicator", g_server_settings.allow_typing_indicator);
    cJSON_AddBoolToObject(message, "allow_client_renames", g_server_settings.allow_client_renames);
    cJSON_AddBoolToObject(message, "allow_link_previews", g_server_settings.allow_link_previews);
    cJSON_AddBoolToObject(message, "allow_avatars", g_server_settings.allow_avatars);
    cJSON_AddNumberToObject(message, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
    cJSON_AddNumberToObject(message, "icon_max_size", (double)g_server_settings.icon_max_size_bytes);
    cJSON_AddBoolToObject(message, "is_alias_registration_allowed", g_server_settings.allow_alias_registrations);
    cJSON_AddBoolToObject(message, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
    cJSON_AddBoolToObject(message, "enforce_connection_check", g_server_settings.enforce_connection_check);
    cJSON_AddBoolToObject(message, "show_music_bot_marquee_to_everyone", g_server_settings.show_music_bot_marquee_to_everyone);
    cJSON_AddBoolToObject(message, "is_video_streaming_allowed", g_server_settings.is_video_streaming_active);
    cJSON_AddBoolToObject(message, "is_audio_over_websocket_allowed", g_server_settings.is_audio_over_websocket_allowed);
    cJSON_AddBoolToObject(message, "is_audio_over_webrtc_allowed", g_server_settings.is_audio_over_webrtc_allowed);
    // the audio switches too: a client whose audio was switched off stops asking for connections
    cJSON_AddBoolToObject(message, "is_voice_chat_active", g_server_settings.is_voice_chat_active);
    cJSON_AddBoolToObject(message, "is_music_bot_audio_active", g_server_settings.is_music_bot_audio_active);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts a channel-created event to all clients
 *
 * @param channel_t* channel -> the new channel
 * @param client_t* creator -> the client that created it
 *
 * @attention this function is called within two acquired read locks, clients_global_rwlock_guard and channels_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_channel_create_message_to_all_clients(channel_t* channel, client_t* creator)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("channel_create", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(message, "parent_channel_id", channel->parent_channel_id);
    cJSON_AddStringToObject(message, "name", channel->name);
    cJSON_AddStringToObject(message, "description", channel->description);
    cJSON_AddNumberToObject(message, "maintainer_id", channel->maintainer_id);
    cJSON_AddBoolToObject(message, "is_using_password", channel->is_using_password);
    cJSON_AddBoolToObject(message, "is_audio_enabled", channel->is_audio_enabled);
    cJSON_AddBoolToObject(message, "is_video_stream_enabled", channel->is_video_stream_enabled);
    cJSON_AddBoolToObject(message, "is_root_channel", channel->is_root_channel);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);
    cJSON_AddBoolToObject(message, "is_temp_channel", channel->is_temp_channel);
    cJSON_AddBoolToObject(message, "is_client_limit_active", channel->is_client_limit_active);
    cJSON_AddNumberToObject(message, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(message, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)channel->icon_id);
    cJSON_AddNumberToObject(message, "channel_creator_id", (double)creator->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a channel-edited event to all clients
 *
 * @param channel_t* channel -> the edited channel
 * @param client_t* editor -> the client that performed the edit, sent along so receivers know who changed the channel
 *
 * @attention this function is called within two acquired read locks, clients_global_rwlock_guard and channels_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_channel_edit_message_to_all_clients(channel_t* channel, client_t* editor)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("channel_edit", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "channel_id", channel->channel_id);
    cJSON_AddStringToObject(message, "channel_name", channel->name);
    cJSON_AddStringToObject(message, "channel_description", channel->description);
    cJSON_AddBoolToObject(message, "is_using_password", (cJSON_bool)channel->is_using_password);
    cJSON_AddBoolToObject(message, "is_audio_enabled", (cJSON_bool)channel->is_audio_enabled);
    cJSON_AddBoolToObject(message, "is_video_stream_enabled", (cJSON_bool)channel->is_video_stream_enabled);
    cJSON_AddBoolToObject(message, "is_client_limit_active", (cJSON_bool)channel->is_client_limit_active);
    cJSON_AddNumberToObject(message, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(message, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)channel->icon_id);
    cJSON_AddNumberToObject(message, "channel_editor_id", (double)editor->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief metadata are sent, so that the client knows an image is being sent to it.
 *
 * @param client_t* receiver -> the client that sent the message
 * @param uint64 chat_message_id -> id of server message
 * @param uint64 local_message_id -> id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(client_t* receiver, uint64 chat_message_id, uint64 local_message_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("server_chat_message_id_for_local_message_id", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "server_chat_message_id", chat_message_id);
    cJSON_AddNumberToObject(message, "local_message_id", local_message_id);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief send chat message itself to client
 *
 * @param client_t* receiver -> the client that receives the message
 * @param client_t* sender -> the client that sent it
 * @param uint64 server_chat_message_id -> server assigned id of the chat message
 * @param char* chat_message_value -> text content of the chat message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_message_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, char* chat_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("direct_chat_message", &message);

    cJSON_AddStringToObject(message, "value", chat_message_value);
    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", sender->client_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", server_chat_message_id);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param client_t* sender -> client that sent the message
 * @param uint64 channel_id -> channel whose other members receive the message
 * @param uint64 server_chat_message_id -> server assigned id of the chat message
 * @param char* chat_message_value -> message content to forward
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_message_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* chat_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_chat_message", &message);

    cJSON_AddStringToObject(message, "value", chat_message_value);
    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", server_chat_message_id);

    _server_msg_internal__send_json_to_channel(root, channel_id, sender);

    cJSON_Delete(root);
}

/**
 * @brief sends a delete/edit action for a chat message to a SINGLE client (private/direct case). the action
 *        carries the target server chat id and the requester's public key + admin flag, so the receiver can
 *        decide for itself whether to honour it (author or admin). for an edit it also carries the new value.
 *
 * @param client_t* receiver -> the client to send the action to
 * @param char* action_type -> "chat_message_delete" or "chat_message_edit"
 * @param uint64 target_chat_message_id -> server chat id of the message to act on
 * @param char* requester_public_key -> public key of the client that requested the action
 * @param boole requester_is_admin -> whether the requester is an admin
 * @param char* new_message_value -> new text for an edit, or NULL_POINTER for a delete
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_chat_message_action_to_single_client(client_t* receiver, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message(action_type, &message);

    cJSON_AddNumberToObject(message, "chat_message_id", target_chat_message_id);
    cJSON_AddStringToObject(message, "requester_public_key", requester_public_key);
    cJSON_AddBoolToObject(message, "requester_is_admin", requester_is_admin == TRUE);
    if (new_message_value != NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "new_message_value", new_message_value);
    }

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends a delete/edit action for a chat message to every client in a channel (channel case). unlike a
 *        normal chat broadcast this does NOT skip the requester, so the message updates in their own view too.
 *
 * @param uint64 receiving_channel_id -> the channel whose clients receive the action
 * @param char* action_type -> "chat_message_delete" or "chat_message_edit"
 * @param uint64 target_chat_message_id -> server chat id of the message to act on
 * @param char* requester_public_key -> public key of the client that requested the action
 * @param boole requester_is_admin -> whether the requester is an admin
 * @param char* new_message_value -> new text for an edit, or NULL_POINTER for a delete
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_chat_message_action_to_clients_in_same_channel(uint64 receiving_channel_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message(action_type, &message);

    cJSON_AddNumberToObject(message, "chat_message_id", target_chat_message_id);
    cJSON_AddStringToObject(message, "requester_public_key", requester_public_key);
    cJSON_AddBoolToObject(message, "requester_is_admin", requester_is_admin == TRUE);
    if (new_message_value != NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "new_message_value", new_message_value);
    }

    _server_msg_internal__send_json_to_channel(root, receiving_channel_id, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief sends chat-picture metadata to all clients in the sender's channel
 *
 * @param client_t* sender -> client that sent the picture
 * @param uint64 channel_id -> channel whose other members receive the picture details
 * @param uint64 server_chat_message_id -> server assigned id of the picture
 * @param uint64 encrypted_size -> encrypted picture length in bytes
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_chat_picture_metadata", &message);

    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "picture_id", server_chat_message_id);
    // the encrypted length the receiver counts its chunks against, for the progress ring
    cJSON_AddNumberToObject(message, "size", (double)encrypted_size);

    _server_msg_internal__send_attachment_to_channel(root, channel_id, sender, "channel_chat_picture");

    cJSON_Delete(root);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param client_t* sender -> the client that sent it
 * @param uint64 channel_id -> the channel whose members receive it
 * @param uint64 server_chat_message_id -> server assigned id of the picture message
 * @param char* chat_message_value -> self explanatory
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_channel_chat_picture_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* chat_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_chat_picture", &message);

    cJSON_AddStringToObject(message, "value", chat_message_value);
    cJSON_AddStringToObject(message, "username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "picture_id", server_chat_message_id);

    _server_msg_internal__send_json_to_channel(root, channel_id, sender);

    cJSON_Delete(root);
}

/**
 * @brief This function sends the image status to the client that SENT the image, so the client knows that the server received and sent its message to the other clients / client
 *
 * @param client_t* receiver -> self explanatory
 * @param char* status -> status string that is put into the "value" field of the image_sent_status message
 *
 * @attention thanks for your attention
 *
 * @return void
 */
void server_msg__send_image_status_to_single_client(client_t* receiver, char* status)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("image_sent_status", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddStringToObject(message, "value", status);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief send chat picture metadata to a single client
 *
 * @param client_t* receiver -> the client that receives the message
 * @param client_t* sender -> the client that sent it
 * @param uint64 server_chat_message_id -> server assigned id of the picture
 * @param uint64 encrypted_size -> encrypted picture length in bytes
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_picture_metadata_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("direct_chat_picture_metadata", &message);

    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", sender->client_id);
    cJSON_AddNumberToObject(message, "picture_id", server_chat_message_id);
    // the encrypted length the receiver counts its chunks against, for the progress ring
    cJSON_AddNumberToObject(message, "size", (double)encrypted_size);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends chat-file metadata to all clients in the sender's channel: the sender's encrypted
 *        name/size/mime header (opaque here) and the encrypted body length, so receivers can draw
 *        the file card and a progress ring before the chunks arrive
 *
 * @param client_t* sender -> client that sent the file
 * @param uint64 channel_id -> channel whose other members receive the file details
 * @param uint64 server_chat_message_id -> server assigned id of the file
 * @param char* file_header -> the sender's encrypted header, forwarded as is
 * @param uint64 encrypted_size -> length of the encrypted body that follows in chunks
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_channel_chat_file_metadata_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_chat_file_metadata", &message);

    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "file_id", server_chat_message_id);
    cJSON_AddStringToObject(message, "file_header", file_header);
    cJSON_AddNumberToObject(message, "encrypted_size", (double)encrypted_size);

    _server_msg_internal__send_attachment_to_channel(root, channel_id, sender, "channel_chat_file");

    cJSON_Delete(root);
}

/**
 * @brief sends chat-file metadata to a single client (direct chat): the sender's encrypted
 *        name/size/mime header (opaque here) and the encrypted body length
 *
 * @param client_t* receiver -> the client that receives the message
 * @param client_t* sender -> the client that sent it
 * @param uint64 server_chat_message_id -> server assigned id of the file
 * @param char* file_header -> the sender's encrypted header, forwarded as is
 * @param uint64 encrypted_size -> length of the encrypted body that follows in chunks
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_chat_file_metadata_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("direct_chat_file_metadata", &message);

    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", sender->client_id);
    cJSON_AddNumberToObject(message, "file_id", server_chat_message_id);
    cJSON_AddStringToObject(message, "file_header", file_header);
    cJSON_AddNumberToObject(message, "encrypted_size", (double)encrypted_size);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells the client that SENT a chat file why the server refused it, so the failure is never
 *        silent on its side. reasons: file_uploads_disabled, file_too_large, receiver_unavailable,
 *        private_messages_disabled
 *
 * @param client_t* receiver -> the sender
 * @param char* reason -> one of the reason strings above
 * @param uint64 local_message_id -> the sender's own id of the refused message, so it can mark that card
 *
 * @attention this function is used within acquired lock on clients array
 *
 * @return void
 */
void server_msg__send_file_send_error_to_single_client(client_t* receiver, char* reason, uint64 local_message_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s %s %s", "server_msg__send_file_send_error_to_single_client", reason, "\n");

    root = _server_msg_internal__new_message("file_send_error", &message);

    cJSON_AddStringToObject(message, "reason", reason);
    cJSON_AddNumberToObject(message, "local_message_id", (double)local_message_id);
    cJSON_AddNumberToObject(message, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends channel join message to all clients
 *
 * @param client_t* subject -> self explanatory
 * @param channel_t* new_channel -> id of server message
 *
 * @attention
 * this function gets called AFTER new channel id is assigned to client struct, not before
 * so this function must assume the client is already in its new channel, if it is going to do any logical operations based on that
 *
 * @return void
 */
void server_msg__send_channel_join_message_to_all_clients(client_t* subject, channel_t* new_channel)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* root_hidden = NULL_POINTER;
    cJSON* message_hidden = NULL_POINTER;
    uint64 x = 0;
    char* plaintext = NULL_POINTER;
    char* plaintext_hidden = NULL_POINTER;

    client_t* client_in_loop = 0;
    char* song_name = 0;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_join", &message);

    root_hidden = _server_msg_internal__new_message("channel_join", &message_hidden);

    // clients not in same channel will not receive real time microphone usage information from the client that switched the channel
    is_streaming_song = subject->is_streaming_song;
    audio_state = subject->audio_state;
    song_name = subject->song_name;

    if (subject->channel_id != new_channel->channel_id)
    {
        if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        is_streaming_song = FALSE;
        song_name = "";
    }

    cJSON_AddNumberToObject(message, "channel_id", new_channel->channel_id);
    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    cJSON_AddNumberToObject(message, "audio_state", audio_state);
    cJSON_AddBoolToObject(message, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(message, "song_name", song_name);

    plaintext = cJSON_PrintUnformatted(root);

    cJSON_AddNumberToObject(message_hidden, "channel_id", (double)-1);
    cJSON_AddNumberToObject(message_hidden, "client_id", subject->client_id);
    cJSON_AddNumberToObject(message_hidden, "audio_state", audio_state);
    cJSON_AddBoolToObject(message_hidden, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(message_hidden, "song_name", song_name);
    plaintext_hidden = cJSON_PrintUnformatted(root_hidden);

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        boole is_hide_client_active = FALSE;

        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        is_hide_client_active = FALSE;

        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)
        {
            if (client_in_loop->channel_id != new_channel->channel_id)
            {
                if (new_channel->is_using_password == TRUE)
                {
                    is_hide_client_active = TRUE;
                }
            }
        }
        // a client that may not see the password protected channel gets the variant without the channel id
        _server_msg_internal__send_text_to_client(client_in_loop, is_hide_client_active == TRUE ? plaintext_hidden : plaintext);
    }

    base__free_json_message(root, plaintext);
    base__free_json_message(root_hidden, plaintext_hidden);
}

/**
 * @brief sends a channel_join message about one client to one single receiving client
 *
 *        when the receiving client is not in the same channel as the client that switched, real time
 *        microphone information is stripped before sending, push to talk active is downgraded to push
 *        to talk enabled and the streamed song name is cleared.
 *
 * @param client_t* subject -> the client whose channel change is being announced
 * @param channel_t* new_channel -> channel that the subject joined
 * @param client_t* receiver -> client that receives the channel change
 *
 * @return void
 */
void server_msg__send_channel_join_message_to_single_client(client_t* receiver, client_t* subject, channel_t* new_channel)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    char* song_name = 0;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    root = _server_msg_internal__new_message("channel_join", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // clients not in same channel will not receive real time microphone usage information from the client that switched the channel
    is_streaming_song = subject->is_streaming_song;
    audio_state = subject->audio_state;
    song_name = subject->song_name;

    if (subject->channel_id != new_channel->channel_id)
    {
        if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        is_streaming_song = FALSE;
        song_name = "";
    }

    cJSON_AddNumberToObject(message, "channel_id", new_channel->channel_id);
    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    cJSON_AddNumberToObject(message, "audio_state", audio_state);
    cJSON_AddBoolToObject(message, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(message, "song_name", song_name);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts the channel's maintainer id to all clients in that channel
 *
 * @param channel_t* channel -> the channel whose members receive it
 * @param uint64 maintainer_id -> the maintainer client id to announce, as a payload value
 *
 * @attention used within write lock for clients and channels
 *
 * @return void
 */
void server_msg__send_maintainer_id_to_clients_in_same_channel(channel_t* channel, uint64 maintainer_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_maintainer_id", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddNumberToObject(message, "maintainer_id", maintainer_id);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);

    _server_msg_internal__send_json_to_channel(root, channel->channel_id, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a channel-deleted event to all clients
 *
 * @param uint64 deleted_channel_id -> id of the deleted channel
 * @param uint64 channel_deletor_id -> id of the client that deleted it
 *
 * @attention used within write lock for clients and channels
 *
 * @return void
 */
void server_msg__send_channel_delete_message_to_all_clients(uint64 deleted_channel_id, uint64 channel_deletor_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_delete", &message);

    cJSON_AddNumberToObject(message, "channel_id", deleted_channel_id);
    if (channel_deletor_id == UINT64_MAX)
    {
        cJSON_AddNullToObject(message, "channel_deletor_id");
    }
    else
    {
        cJSON_AddNumberToObject(message, "channel_deletor_id", channel_deletor_id);
    }

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a client-disconnected event to all clients
 *
 * @param uint64 client_index -> array index of the disconnected client
 *
 * @attention used within acquired write lock for clients_array
 *
 * @return void
 */
void server_msg__send_client_disconnect_message_to_all_clients(uint64 client_index)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("client_disconnect", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "client_id", client_index);

    _server_msg_internal__send_json_to_all_clients(root, &g_clients_array[client_index]);

    cJSON_Delete(root);
}

/**
 * @brief sends a poke message to a single client
 *
 * @param client_t* receiver -> the client to poke
 * @param client_t* sender -> the client sending the poke
 * @param char* poke_message -> the poke text
 *
 * @return void
 */
void server_msg__send_poke_to_single_client(client_t* receiver, client_t* sender, char* poke_message)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("poke", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)sender->client_id);
    cJSON_AddStringToObject(message, "poke_message", poke_message);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells the people a client is writing to that the client is typing. carries no message content at
 *        all - only who is typing and which conversation it belongs to, so the receiver can show it
 *        against the right chat and drop it again when it stops being refreshed.
 *
 * @param client_t* subject -> the client that is typing
 * @param char* receiver_type -> "channel" or "user", the kind of conversation being written to
 * @param uint64 receiver_id -> channel id for "channel", target client id for "user"
 *
 * @attention the caller must already hold a read lock on clients_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_typing_indicator(client_t* subject, char* receiver_type, uint64 receiver_id)
{
    uint64 i = 0;
    boole is_direct_message = FALSE;
    client_t* client_in_loop = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    is_direct_message = clib__is_string_equal(receiver_type, "user");

    root = _server_msg_internal__new_message("typing_indicator", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddStringToObject(message, "receiver_type", receiver_type);
    cJSON_AddNumberToObject(message, "receiver_id", (double)receiver_id);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE || client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop == subject)
        {
            continue;  // he knows he is typing
        }

        // a direct message goes to that one person, a channel one to whoever stands in the channel
        if (is_direct_message == TRUE)
        {
            if (client_in_loop->client_id != receiver_id)
            {
                continue;
            }
        }
        else if (client_in_loop->channel_id != receiver_id)
        {
            continue;
        }

        _server_msg_internal__send_json_to_client(client_in_loop, root);
    }

    cJSON_Delete(root);
}

/**
 * @brief sends a WebRTC SDP offer to a single client
 *
 * @param client_t* receiver -> the client to send the candidate to
 * @param const char* candidate -> the ICE candidate string
 * @param const char* mid -> the sdpMid the candidate belongs to
 *
 * @return void
 */
void server_msg__send_webrtc_sdp_offer_to_single_client(client_t* receiver, const char* candidate, const char* mid)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_message_value = NULL_POINTER;
    cJSON* jmid = NULL_POINTER;
    cJSON* jsoncandidate = NULL_POINTER;
    cJSON* jsonlineindex = NULL_POINTER;

    root = _server_msg_internal__new_message("ice_candidate", &message);
    json_message_value = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    jsoncandidate = cJSON_CreateString(candidate);
    cJSON_AddItemToObject(json_message_value, "candidate", jsoncandidate);

    jmid = cJSON_CreateString(mid);
    cJSON_AddItemToObject(json_message_value, "sdpMid", jmid);

    jsonlineindex = cJSON_CreateNumber(0.0);
    cJSON_AddItemToObject(json_message_value, "sdpMLineIndex", jsonlineindex);

    cJSON_AddItemToObject(message, "value", json_message_value);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts a client's audio state to all clients
 *
 * @param client_t* subject -> the client whose audio state is broadcast
 * @param uint64 state -> the client's new audio state
 *
 * @attention clients read lock held by the caller; the channels array is read for the hidden check
 *
 * @return void
 */
void server_msg__send_audio_state_of_client_to_all_clients(client_t* subject, uint64 state)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* root_other_channel = NULL_POINTER;
    cJSON* message_other_channel = NULL_POINTER;
    char* plaintext = NULL_POINTER;
    char* plaintext_other_channel = NULL_POINTER;
    uint64 i = 0;
    client_t* client = NULL_POINTER;
    boole is_subject_hidden = FALSE;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // two variants, each printed once: the subject's channel hears the real state, the rest of the server
    // only learns that the microphone is on or off, never that it is transmitting right now
    root = _server_msg_internal__new_message("audio_state_of_single_client", &message);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "value", (double)state);
    plaintext = cJSON_PrintUnformatted(root);

    root_other_channel = _server_msg_internal__new_message("audio_state_of_single_client", &message_other_channel);
    cJSON_AddNumberToObject(message_other_channel, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message_other_channel, "value", (double)(state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE ? AUDIO_STATE__PUSH_TO_TALK_ENABLED : state));
    plaintext_other_channel = cJSON_PrintUnformatted(root_other_channel);

    // while clients in password protected channels are hidden, a subject standing in one is not reported outside it
    is_subject_hidden = g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE
        && subject->channel_id < g_server_settings.max_channel_count
        && g_channel_array[subject->channel_id].is_existing == TRUE
        && g_channel_array[subject->channel_id].is_using_password == TRUE;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (util__is_client_valid_and_not_music_bot((int)i) == FALSE)
        {
            continue;
        }
        if (client->channel_id == subject->channel_id)
        {
            _server_msg_internal__send_text_to_client(client, plaintext);
        }
        else if (is_subject_hidden == FALSE)
        {
            _server_msg_internal__send_text_to_client(client, plaintext_other_channel);
        }
    }

    cJSON_free(plaintext);
    cJSON_free(plaintext_other_channel);
    cJSON_Delete(root);
    cJSON_Delete(root_other_channel);
}

/**
 * @brief tells clients in the channel that a song stream has started
 *
 * @param client_t* subject -> the streaming client
 *
 * @return void
 */
void server_msg__send_start_song_stream_message_to_clients_in_same_channel(client_t* subject)
{
    uint64 i = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("start_song_stream", &message);

    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    cJSON_AddStringToObject(message, "song_name", subject->song_name);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        // only the streamer's channel hears about it, unless the marquee is shown to everyone
        if (client->channel_id != subject->channel_id && g_server_settings.show_music_bot_marquee_to_everyone == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        _server_msg_internal__send_json_to_client(client, root);
    }

    cJSON_Delete(root);
}

/**
 * @brief tells clients in the channel that a song stream has stopped
 *
 * @param client_t* subject -> the streaming client
 *
 * @return void
 */
void server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client_t* subject)
{
    uint64 i = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("stop_song_stream", &message);

    cJSON_AddNumberToObject(message, "client_id", subject->client_id);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        // only the streamer's channel hears about it, unless the marquee is shown to everyone
        if (client->channel_id != subject->channel_id && g_server_settings.show_music_bot_marquee_to_everyone == FALSE)
        {
            continue;
        }

        _server_msg_internal__send_json_to_client(client, root);
    }

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a tag-added-to-client event to all clients
 *
 * @param client_t* subject -> the client that received the tag
 * @param uint64 tag_id -> id of the tag added
 *
 * @return void
 */
void server_msg__send_add_tag_to_client_event_to_all_clients(client_t* subject, uint64 tag_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("tag_add_to_client", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts "this client's alias changed" (with the alias text; empty = cleared) to all
 *        authenticated non-bot clients. must be called with the clients lock already held
 *        (like the tag-event broadcasts).
 *
 * @param client_t* subject -> the client whose identity just got the alias
 * @param char* alias -> the new alias text, empty or NULL when cleared
 *
 * @return void
 */
void server_msg__send_client_alias_changed_to_all_clients(client_t* subject, char* alias)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("client_alias_changed", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddStringToObject(message, "alias", (alias == NULL_POINTER) ? "" : alias);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts the country code a client's row shows from now on, to all authenticated non-bot
 *        clients. the clients only ever see the code, never the setting that withholds it, so this
 *        is sent whenever the code the server is willing to show changed: the client became or
 *        stopped being an admin, or the admin toggled hide_admin_country_flag
 *
 * @param client_t* subject -> the client whose shown code changed
 *
 * @note must be called with the clients lock already held (like the tag-event broadcasts)
 *
 * @return void
 */
void server_msg__send_client_country_code_changed_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("client_country_code_changed", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddStringToObject(message, "country_iso_code", _server_msg_internal__country_code_to_send(subject));

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief re-sends every connected admin's shown country code to all clients, after the admin
 *        toggled hide_admin_country_flag: the flags appear or vanish for everyone at once
 *
 * @note must be called with the clients lock already held
 *
 * @return void
 */
void server_msg__send_admin_country_codes_to_all_clients(void)
{
    uint64 i = 0;
    client_t* client = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_admin == FALSE)
        {
            continue;
        }

        server_msg__send_client_country_code_changed_to_all_clients(client);
    }
}

/**
 * @brief broadcasts a lightweight "this client's avatar changed" event (no image payload) to all
 *        authenticated non-bot clients, so they can re-request that client's avatar if they show it.
 *
 * @param client_t* subject -> the client whose avatar was set or deleted
 *
 * @note must be called with the clients lock already held (like the tag-event broadcasts)
 *
 * @return void
 */
void server_msg__send_avatar_changed_event_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = _server_msg_internal__new_message("avatar_changed", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief sends one client's avatar (full base64 data-url, or empty string if none) to a single
 *        requesting client. used for both the profile-pane request and the chunked tree lazy-load,
 *        so avatars are only ever sent to clients that asked for them.
 *
 * @param client_t* receiver -> the client that asked for the avatar
 * @param uint64 avatar_owner_client_id -> whose avatar this is
 * @param char*          base64_avatar    -> the avatar data-url, or "" for none
 *
 * @return void
 */
void server_msg__send_client_avatar_to_single_client(client_t* receiver, uint64 avatar_owner_client_id, char* base64_avatar)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    if (receiver == NULL_POINTER)
    {
        return;
    }

    root = _server_msg_internal__new_message("client_avatar", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)avatar_owner_client_id);
    cJSON_AddStringToObject(message, "base64_avatar", (base64_avatar != NULL_POINTER) ? base64_avatar : "");

    _server_msg_internal__send_json_to_client(receiver, root);

    cJSON_Delete(root);
}

/**
 * @brief sends the stored-identity list to one client, so it can show the people registered on this
 *        server even while they are offline.
 *
 *        every entry carries the alias, the avatar and the tag ids. no identity hash and no username
 *        is sent: the alias is the only handle, and it is what the client pairs against a live client
 *        of the same alias. the raw public key is added on top only while allow_offline_messages is
 *        enabled, since that is what lets a peer encrypt to somebody who is not connected - it is
 *        public by definition, but it still only goes to registered requesters (the caller checked
 *        that). last_seen (unix seconds) is added only while allow_last_seen is enabled, so nothing
 *        leaks while the setting is off. slots with no public key or no alias are skipped - they are
 *        free, or they have no name to show and nothing to pair on.
 *
 * @param client_t* receiver -> the client that receives the list
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock), exactly like
 *       the identity serializer in base.c.
 *
 * @return void
 */
void server_msg__send_stored_clients_to_single_client(client_t* receiver)
{
    uint64 i = 0;
    uint64 t = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_stored_clients = 0;
    cJSON* json_stored_client = 0;
    cJSON* json_tag_ids = 0;

    if (receiver == NULL_POINTER)
    {
        return;
    }

    root = _server_msg_internal__new_message("stored_clients_list", &message);
    json_stored_clients = cJSON_CreateArray();

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue; // free slot, or nothing to name and pair it by
        }

        json_stored_client = cJSON_CreateObject();
        cJSON_AddStringToObject(json_stored_client, "alias", &g_client_stored_data[i].alias[0]);
        cJSON_AddStringToObject(json_stored_client, "base64_avatar", &g_client_stored_data[i].base64_avatar[0]);

        // with offline messages on, registered peers also get this identity's public key: it is what
        // lets them encrypt a message to somebody who is not connected. a public key is public by
        // definition, but it is still only handed to REGISTERED requesters (the caller checked) and
        // only while the feature is enabled
        if (g_server_settings.allow_offline_messages == TRUE && g_client_stored_data[i].raw_public_key[0] != 0)
        {
            cJSON_AddStringToObject(json_stored_client, "public_key", &g_client_stored_data[i].raw_public_key[0]);
        }

        // when the admin enabled last-seen, tell the receiver when this identity was last connected
        // (unix seconds). left out entirely while the setting is off, so nothing leaks
        if (g_server_settings.allow_last_seen == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            cJSON_AddNumberToObject(json_stored_client, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds);
        }

        json_tag_ids = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(json_tag_ids, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(json_stored_client, "tag_ids", json_tag_ids);

        cJSON_AddItemToArray(json_stored_clients, json_stored_client);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    cJSON_AddItemToObject(message, "stored_clients", json_stored_clients);

    _server_msg_internal__send_json_to_client(receiver, root);

    cJSON_Delete(root);
}

/**
 * @brief delivers everything that was said to this client's identity while it was offline, oldest
 *        first, then drops the queue for that identity. each message goes out as its own
 *        offline_chat_message so the client can render them like any other direct message; the
 *        payload is the sender's ciphertext, untouched (the server never could read it).
 *
 * @param client_t* receiver -> the freshly authenticated client
 *
 * @return void
 *
 * @note takes g_offline_messages_mutex (leaf lock). caller holds the clients lock.
 */
void server_msg__send_queued_offline_messages_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    char recipient_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 delivered_count = 0;
    uint64 lowest_sequence_number = 0;
    int64 next_slot = 0;

    if (g_offline_messages == NULL_POINTER || receiver == NULL_POINTER || receiver->public_key[0] == 0)
    {
        return;
    }

    base__hash_password_to_base64(receiver->public_key, recipient_identity_hash, sizeof(recipient_identity_hash));

    // deliver in the order they were sent: repeatedly pick the lowest sequence number still queued
    // for this identity. the queue is small and this runs once per connect
    while (TRUE)
    {
        next_slot = -1;
        lowest_sequence_number = 0;

        pthread_mutex_lock(&g_offline_messages_mutex);

        for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
        {
            if (g_offline_messages[i].is_used == FALSE)
            {
                continue;
            }

            if (clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], recipient_identity_hash) == FALSE)
            {
                continue;
            }

            if (next_slot == -1 || g_offline_messages[i].sequence_number < lowest_sequence_number)
            {
                next_slot = (int64)i;
                lowest_sequence_number = g_offline_messages[i].sequence_number;
            }
        }

        if (next_slot == -1)
        {
            pthread_mutex_unlock(&g_offline_messages_mutex);
            break;
        }

        root = _server_msg_internal__new_message("offline_chat_message", &message);

        cJSON_AddStringToObject(message, "value", g_offline_messages[next_slot].base64_encrypted_message);
        cJSON_AddStringToObject(message, "sender_alias", &g_offline_messages[next_slot].sender_alias[0]);
        cJSON_AddNumberToObject(message, "queued_unix_seconds", (double)g_offline_messages[next_slot].queued_unix_seconds);
        cJSON_AddNumberToObject(message, "sequence_number", (double)g_offline_messages[next_slot].sequence_number);

        // free this one before sending, so a failing send cannot leave it queued forever
        if (g_offline_messages[next_slot].base64_encrypted_message != NULL_POINTER)
        {
            memorymanager__free((nuint)g_offline_messages[next_slot].base64_encrypted_message);
        }
        clib__null_memory(&g_offline_messages[next_slot], sizeof(offline_chat_message_t));

        pthread_mutex_unlock(&g_offline_messages_mutex);

        _server_msg_internal__send_json_to_client(receiver, root);

        cJSON_Delete(root);

        delivered_count++;
    }

    if (delivered_count > 0)
    {
        DBG_SERVER_MESSAGE log_info("%s %llu %s", "delivered", delivered_count, "offline message(s) on connect \n");
    }
}

/**
 * @brief broadcasts a tag-removed-from-client event to all clients
 *
 * @param client_t* subject -> the client that lost the tag
 * @param uint64 tag_id -> id of the tag removed
 *
 * @return void
 */
void server_msg__send_remove_tag_from_client_event_to_all_clients(client_t* subject, uint64 tag_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("remove_tag_from_client", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a new-icon-added event to all clients
 *
 * @param uint64 new_icon_id -> id of the new icon
 * @param char* icon_base64_value -> base64-encoded icon image
 *
 * @return void
 */
void server_msg__send_add_new_icon_event_to_all_clients(uint64 new_icon_id, char* icon_base64_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("icon_add", &message);

    cJSON_AddNumberToObject(message, "icon_id", new_icon_id);
    cJSON_AddStringToObject(message, "base64_icon", icon_base64_value);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a tag-deleted-from-pool event to all clients
 *
 * @param uint64 tag_id -> id of the tag that was deleted
 *
 * @return void
 */
void server_msg__send_remove_tag_event_to_all_clients(uint64 tag_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("tag_delete", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts an icon-deleted-from-pool event to all clients
 *
 * @param uint64 icon_id -> id of the icon that was deleted
 *
 * @return void
 */
void server_msg__send_remove_icon_event_to_all_clients(uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("icon_delete", &message);

    cJSON_AddNumberToObject(message, "icon_id", icon_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a tag-icon-changed event to all clients
 *
 * @param uint64 tag_id -> id of the tag whose icon changed
 * @param boole has_icon -> whether the tag now has an icon
 * @param uint64 icon_id -> the tag's icon id (meaningful only when has_icon is TRUE)
 *
 * @return void
 */
void server_msg__send_tag_icon_changed_event_to_all_clients(uint64 tag_id, boole has_icon, uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("tag_icon_changed", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddItemToObject(message, "has_icon", cJSON_CreateBool(has_icon == TRUE));
    cJSON_AddNumberToObject(message, "tag_linked_icon_id", icon_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a channel's icon change to every authenticated client so their channel row updates live
 *
 * @param uint64 channel_id -> id of the channel whose icon changed
 * @param boole has_channel_icon -> whether the channel now has an icon
 * @param uint64 icon_id -> the channel's icon id (meaningful only when has_channel_icon is TRUE)
 *
 * @return void
 */
void server_msg__send_channel_icon_changed_event_to_all_clients(uint64 channel_id, boole has_channel_icon, uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("channel_icon_changed", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);
    cJSON_AddItemToObject(message, "has_channel_icon", cJSON_CreateBool(has_channel_icon == TRUE));
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)icon_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a new-tag-created event to all clients
 *
 * @param uint64 tag_id -> id of the new tag
 * @param char* tag_name -> name of the new tag
 * @param uint64 tag_linked_icon_id -> id of the icon linked to the tag
 * @param boole has_icon -> TRUE when the tag really has an icon, sent as a bool so the client knows
 *                          whether tag_linked_icon_id is meaningful
 *
 * @return void
 */
void server_msg__send_create_new_tag_event_to_all_clients(uint64 tag_id, char* tag_name, uint64 tag_linked_icon_id, boole has_icon)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("tag_add", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddStringToObject(message, "tag_name", tag_name);
    cJSON_AddNumberToObject(message, "tag_linked_icon_id", tag_linked_icon_id);
    cJSON_AddItemToObject(message, "has_icon", cJSON_CreateBool(has_icon == TRUE));

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief notifies an idle client that another client is calling them
 *
 * @param client_t* receiver -> the idle client being called
 * @param client_t* caller -> the client placing the call
 *
 * @return void
 */
void server_msg__send_call_event_to_idle_client(client_t* receiver, client_t* caller)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("call", &message);

    cJSON_AddNumberToObject(message, "caller_client_id", caller->client_id);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends client_going_to_idle_mode to all authenticated clients
 *
 * @param client_t* subject -> the client that is going to idle mode
 *
 * @attention
 *
 * @return void
 */
void server_msg__send_client_going_to_idle_mode_info_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("client_going_to_idle_mode", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief This function sends client_coming_back_from_idle_mode to all authenticated clients
 *
 * @param client_t* subject -> the client coming back from idle mode
 * @param uint64 channel_the_client_joins -> id of the channel the client joins on return
 *
 * @attention
 *
 * @return void
 */
void server_msg__send_client_coming_back_from_idle_mode_info_to_all_clients(client_t* subject, uint64 channel_the_client_joins)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("client_coming_back_from_idle_mode", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel_the_client_joins);

    _server_msg_internal__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief sends song list of music bot to single_client
 *
 * @param client_t* receiver -> the admin that asked for the list
 * @param client_t* music_bot -> the music bot whose song list is sent
 *
 * @attention clients read lock held by the caller, who has checked that the receiver is an admin and the bot a live music bot
 *
 * @return void
 */
void server_msg__send_music_bot_song_list_to_single_client(client_t* receiver, client_t* music_bot)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_songs_array = 0;
    music_bot_single_song_data_t* single_song_in_loop = 0;
    uint64 loop_index = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("music_bot_song_list", &message);
    json_songs_array = cJSON_CreateArray();

    // create array of clients
    for (loop_index = 0; loop_index < MUSIC_BOT_MAX_FILE_COUNT; loop_index++)
    {
        cJSON* song = NULL_POINTER;

        single_song_in_loop = &music_bot->music_bot_client_extension.songs[loop_index];

        if (single_song_in_loop->is_existing == FALSE)
        {
            continue;
        }

        song = cJSON_CreateObject();
        cJSON_AddStringToObject(song, "name", single_song_in_loop->song_name);
        cJSON_AddNumberToObject(song, "id", loop_index);
        cJSON_AddNumberToObject(song, "duration_seconds", single_song_in_loop->length_seconds);
        cJSON_AddItemToArray(json_songs_array, song);
    }

    cJSON_AddItemToObject(message, "songs", json_songs_array);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells the client its file upload completed successfully, so it can send instructions on what to do with the file
 *
 * @param client_t* receiver -> the client whose file upload completed
 * @todo make image file upload use this function
 *
 * @attention this function is used within acquired readlock within client_message.c , multiple functions
 *
 * @return void
 */
void server_msg__send_file_send_completed_status_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("file_send_success", &message);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends one chunk of a file transfer as a file_receive_chunk message to the single receiving
 *        client, encrypted with that receiver's shared secret.
 *
 *        the receiver is looked up by index in g_clients_array. the chunk is passed through as the
 *        "value" field exactly as given, together with the sender id and the server chat message id so
 *        the client can append the chunk to the right transfer.
 *
 * @param client_t* receiver -> the client this chunk goes to
 * @param client_t* sender -> the client that sends the file
 * @param char* chunk -> the already prepared chunk payload, sent as the "value" field
 * @param uint64 current_size -> size counter of the transfer so far, not written into the message
 * @param uint64 server_chat_message_id -> id of the chat message this file belongs to
 *
 * @return void
 */
int server_msg__send_file_by_chunk_to_single_client(client_t* receiver, client_t* sender, char* chunk, uint64 current_size, uint64 server_chat_message_id)
{
    char* plaintext = server_msg__prepare_file_chunk(sender->client_id, chunk, server_chat_message_id);
    int result = -1;

    if (plaintext != NULL_POINTER)
    {
        result = server_msg__queue_file_chunk(receiver, plaintext);
    }
    cJSON_free(plaintext);
    return result;
}

/**
 * @brief Builds one file-piece message so it can be reused for every receiver.
 * @param uint64 sender_id -> client sending the file
 * @param char* chunk -> encrypted file bytes, ending with a zero byte
 * @param uint64 server_chat_message_id -> message containing the file
 * @return char* -> JSON text to release with cJSON_free, or NULL_POINTER on failure
 */
char* server_msg__prepare_file_chunk(uint64 sender_id, char* chunk, uint64 server_chat_message_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    char* plaintext = NULL_POINTER;

    root = _server_msg_internal__new_message("file_receive_chunk", &message);

    cJSON_AddStringToObject(message, "value", chunk);
    cJSON_AddNumberToObject(message, "sender_id", (double)sender_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", server_chat_message_id);

    plaintext = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return plaintext;
}

/**
 * @brief Tries to queue a file piece without waiting for a slow receiver.
 * @param client_t* receiver -> current connection, protected by the clients lock
 * @param const char* plaintext -> prepared message; the caller keeps its copy
 * @return int -> 0 queued, 1 retry later, -1 connection unavailable
 */
int server_msg__queue_file_chunk(client_t* receiver, const char* plaintext)
{
    return _server_msg_internal__queue_text(receiver->p_ws_connection,
        &receiver->session_keys, plaintext, TRUE, FALSE);
}

/**
 * @brief tells one receiving client that a file transfer is finished, as a file_receive_completed
 *        message encrypted with that receiver's shared secret.
 *
 *        sender and receiver are both looked up by index in g_clients_array. the message carries the
 *        receive type, the sender's client id and the server chat message id, so the client can close
 *        the right transfer and render it.
 *
 * @param client_t* receiver -> the client that gets the notification
 * @param data_for_file_send_thread_t* info -> the transfer descriptor, read for client_sender_id and server_chat_message_id
 * @param char* receive_type -> kind of transfer, sent as the "receive_type" field
 *
 * @return void
 */
void server_msg__send_file_receive_completed_to_single_client(client_t* receiver, data_for_file_send_thread_t* info, char* receive_type)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("file_receive_completed", &message);

    cJSON_AddStringToObject(message, "receive_type", receive_type);
    cJSON_AddNumberToObject(message, "sender_id", (double)info->client_sender_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", info->server_chat_message_id);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells every client in a channel that a video stream started or ended there, with what the streamer announced while it runs
 *        the streamer gets it too, that is how its popup confirms the start; late joiners to the
 *        server learn about a running stream from is_streaming_video in the client list instead
 *
 * @param uint64 channel_id -> the channel
 * @param boole is_active -> TRUE for a start, FALSE for an end
 * @param uint64 streamer_client_id -> the streamer (still valid on an end, its fields are already cleared then)
 * @param cstring reason -> "started", "stopped", "streamer_left", "channel_disabled", "disabled", ...
 *
 * @return void
 */
void server_msg__send_video_stream_state_to_clients_in_same_channel(uint64 channel_id, boole is_active, uint64 streamer_client_id, cstring reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    client_t* streamer = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("video_stream_state", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);
    cJSON_AddNumberToObject(message, "streamer_client_id", (double)streamer_client_id);
    cJSON_AddBoolToObject(message, "is_active", is_active);
    cJSON_AddStringToObject(message, "reason", reason);

    if (is_active == TRUE && streamer_client_id < g_server_settings.max_client_count)
    {
        streamer = &g_clients_array[streamer_client_id];
        cJSON_AddStringToObject(message, "source", streamer->video_stream_source);
        cJSON_AddStringToObject(message, "codec", streamer->video_stream_codec);
        cJSON_AddNumberToObject(message, "width", (double)streamer->video_stream_width);
        cJSON_AddNumberToObject(message, "height", (double)streamer->video_stream_height);
        cJSON_AddNumberToObject(message, "fps", (double)streamer->video_stream_fps);
        cJSON_AddBoolToObject(message, "is_sound_included", streamer->is_video_stream_sound_included);
    }

    _server_msg_internal__send_json_to_channel(root, channel_id, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief hands one member the stream's offer: it may now press connect. sent to everybody present at the start and to a later joiner the streamer allowed
 *
 * @param client_t* receiver -> the member
 * @param client_t* streamer -> the streamer, whose announced source / codec / size are sent with it so the receiver can probe its decoder first, with whether sound is included and the ids of the members watching right now
 *
 * @return void
 */
void server_msg__send_video_stream_offer_to_single_client(client_t* receiver, client_t* streamer)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* watching_client_ids = NULL_POINTER;
    client_t* member = NULL_POINTER;
    uint64 i = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("video_stream_offer", &message);

    cJSON_AddNumberToObject(message, "streamer_client_id", (double)streamer->client_id);
    cJSON_AddStringToObject(message, "source", streamer->video_stream_source);
    cJSON_AddStringToObject(message, "codec", streamer->video_stream_codec);
    cJSON_AddNumberToObject(message, "width", (double)streamer->video_stream_width);
    cJSON_AddNumberToObject(message, "height", (double)streamer->video_stream_height);
    cJSON_AddNumberToObject(message, "fps", (double)streamer->video_stream_fps);
    cJSON_AddBoolToObject(message, "is_sound_included", streamer->is_video_stream_sound_included);

    // who watches right now, so the receiver can mark those rows at once; every later change reaches
    // it as a video_stream_viewer_state message, which an allowed member receives from here on
    watching_client_ids = cJSON_AddArrayToObject(message, "watching_client_ids");
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        member = &g_clients_array[i];
        if (member->is_existing == TRUE && member->channel_id == streamer->channel_id && member->is_video_viewer_watching == TRUE)
        {
            cJSON_AddItemToArray(watching_client_ids, cJSON_CreateNumber((double)member->client_id));
        }
    }

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief asks the streamer whether a member that joined the channel after the start may watch
 *
 * @param client_t* receiver -> the streamer
 * @param client_t* newcomer -> the member that just arrived
 *
 * @return void
 */
void server_msg__send_video_stream_viewer_request_to_single_client(client_t* receiver, client_t* newcomer)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("video_stream_viewer_request", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)newcomer->client_id);
    cJSON_AddStringToObject(message, "username", newcomer->username);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells one client that a viewer connected or disconnected. the streamer counts its viewers from it and sends a keyframe for a
 *        new one; a member the streamer allowed marks the watching row in its channel tree. video_stream.c sends it to those clients
 *        only, so a member that was not allowed never learns who watches
 *
 * @param client_t* receiver -> the streamer, or a member the streamer allowed
 * @param uint64 viewer_client_id -> the viewer
 * @param boole is_watching -> TRUE when it connected, FALSE when it dropped out
 *
 * @return void
 */
void server_msg__send_video_stream_viewer_state_to_single_client(client_t* receiver, uint64 viewer_client_id, boole is_watching)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("video_stream_viewer_state", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)viewer_client_id);
    cJSON_AddBoolToObject(message, "is_watching", is_watching);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief asks the streamer for a keyframe on behalf of a viewer that lost frames
 *
 * @param client_t* receiver -> the streamer
 *
 * @return void
 */
void server_msg__send_video_stream_keyframe_request_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("video_stream_keyframe_request", &message);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells a client why its stream request was refused ("disabled", "channel_disabled", "busy", "not_allowed", "no_stream", "revoked")
 *
 * @param client_t* receiver -> the client that asked
 * @param cstring reason -> the short reason code sent to the clients
 *
 * @return void
 */
void server_msg__send_video_stream_refused_to_single_client(client_t* receiver, cstring reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = _server_msg_internal__new_message("video_stream_refused", &message);

    cJSON_AddStringToObject(message, "reason", reason);

    _server_msg_internal__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Queues an encrypted URL preview reply for the client that requested it.
 *
 * @param client_t* client -> receiving client
 * @param const char* request_id -> id used to match the reply to its request
 * @param const char* url -> requested website URL
 * @param const cJSON* result -> preview to copy, or NULL_POINTER when unavailable or disabled
 *
 * @attention The caller holds the clients lock and keeps ownership of result.
 *
 * @return void
 */
void server_msg__send_url_resolved_to_single_client(client_t* client, const char* request_id,
    const char* url, const cJSON* result)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* message = result != NULL_POINTER ? cJSON_Duplicate(result, TRUE) : cJSON_CreateObject();
    cJSON_AddItemToObject(root, "message", message);
    cJSON_AddStringToObject(message, "type", "url_resolved");
    cJSON_AddStringToObject(message, "request_id", request_id);
    if (result == NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "url", url);
        cJSON_AddStringToObject(message, "status", g_server_settings.allow_link_previews ? "unavailable" : "disabled");
    }
    _server_msg_internal__send_json_to_client(client, root);
    cJSON_Delete(root);
}
