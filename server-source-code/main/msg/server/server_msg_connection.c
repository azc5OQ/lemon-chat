// This file sends login replies, client lists, connection notices and keepalive replies.

#include "server_msg.h"
#include "../../base.h"
#include "../../metadata_crypto.h"
#include "../../memory_manager.h"
#include "../../util.h"
#include "../../../third-party/eteran-cvector/cvector.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include <limits.h>

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
static char* _server_msg_connection_internal__encode_client_list(void* context, size_t* size);
static void _server_msg_connection_internal__release_client_list(void* context);

/**
 * @brief tells a client its rsa key is too weak, naming the required size, so it can offer to
 *        regenerate. only sent when the admin enabled is_minimum_rsa_key_bits_announced; otherwise
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
void server_msg__connection__send_rsa_key_too_weak_to_single_client(client_t* receiver)
{
    char* plaintext = NULL_POINTER;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("rsa_key_too_weak", &message);
    cJSON_AddNumberToObject(message, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);

    // in plaintext on purpose, see above: no shared secret exists for this socket yet
    plaintext = cJSON_PrintUnformatted(root);
    if (plaintext != NULL_POINTER)
    {
        ws_sendframe_txt(receiver->p_ws_connection, plaintext);
    }
    util__free_json_message(root, plaintext);
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
void server_msg__connection__send_public_key_challenge_to_single_client(client_t* receiver, char* random_value_challenge_string, char* dh_public_mix_for_client)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("public_key_challenge", &message);
    cJSON_AddStringToObject(message, "value", random_value_challenge_string);
    cJSON_AddStringToObject(message, "dh_public_mix", dh_public_mix_for_client);

    // the key exchange has not happened yet, so the lowest sender gets no secret
    server_msg__delivery__send_json(receiver->p_ws_connection, NULL_POINTER, root);
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
void server_msg__connection__send_authentication_status_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("authentication_status", &message);

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
    cJSON_AddBoolToObject(message, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed);
    cJSON_AddNumberToObject(message, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients);
    cJSON_AddNumberToObject(message, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients);
    // the receiver keeps marquees of streaming bots in other channels visible when this is on
    cJSON_AddBoolToObject(message, "is_music_bot_marquee_visible_to_everyone", g_server_settings.is_music_bot_marquee_visible_to_everyone);
    cJSON_AddBoolToObject(message, "is_alias_registration_allowed", g_server_settings.is_alias_registration_allowed);
    // avatars policy travels in-protocol so clients that were NOT served by this server's http
    // server (the android app loads the page from its assets) still learn it - otherwise they
    // fall back to "avatars off" and hide the set/delete avatar actions
    cJSON_AddBoolToObject(message, "is_avatar_upload_allowed", g_server_settings.is_avatar_upload_allowed);
    cJSON_AddBoolToObject(message, "is_typing_indicator_allowed", g_server_settings.is_typing_indicator_allowed);
    // rename policy travels in-protocol so the receiver can grey its rename input out when users may not rename
    cJSON_AddBoolToObject(message, "is_client_rename_allowed", g_server_settings.is_client_rename_allowed);
    cJSON_AddBoolToObject(message, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed);
    cJSON_AddNumberToObject(message, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
    // the settings tab's icon upload refuses oversize files itself, with the same cap the server enforces
    cJSON_AddNumberToObject(message, "icon_max_size", (double)g_server_settings.icon_max_size_bytes);
    // chat file policy: the receiver refuses oversize/forbidden files itself, with a reason, before uploading
    server_msg__server_settings__add_attachment_receive_policy(message);
    cJSON_AddBoolToObject(message, "is_file_upload_allowed", g_server_settings.is_file_upload_allowed);
    cJSON_AddNumberToObject(message, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(message, "chat_picture_max_size", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_AddBoolToObject(message, "is_chat_picture_allowed", g_server_settings.is_chat_picture_allowed);
    // A masquerading tunnel cannot report the visitor's reflexive address.
    // Clients initiate ICE toward our explicitly advertised public candidates instead.
    cJSON_AddNumberToObject(message, "stun_port", g_server_settings.is_wireguard_mode_active ? 0 : 3478);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
    server_msg__server_settings__send_info_to_single_client(receiver, FALSE, (boole)(g_server_settings.server_banner != NULL_POINTER));
}

/**
 * @brief Builds and encrypts the copied client list without taking the clients lock.
 * @param void* context -> client details and connection keys copied during login
 * @param size_t* size -> receives the encrypted message length
 * @return char* -> encrypted message owned by the outgoing queue, or NULL_POINTER
 */
static char* _server_msg_connection_internal__encode_client_list(void* context, size_t* size)
{
    outgoing_client_list_t* outgoing = context;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_client_array = 0;
    cJSON* json_tag_ids_array = 0;
    listed_client_t* client_in_loop = 0;
    cJSON* single_client = NULL_POINTER;
    cJSON* music_keys = NULL_POINTER;
    char key_hex[65] = { 0 };
    char* plaintext = NULL_POINTER;
    char* encoded = NULL_POINTER;
    int64 encoded_size = 0;
    uint key_index = 0;
    uint byte_index = 0;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("client_list", &message);
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
 * @brief Erases and frees the client-list copy, including its copied connection and music keys.
 * @param void* context -> client-list copy owned by the outgoing queue
 * @return void
 */
static void _server_msg_connection_internal__release_client_list(void* context)
{
    outgoing_client_list_t* outgoing = context;
    size_t allocation_size = outgoing->allocation_size;

    mbedtls_platform_zeroize(outgoing, allocation_size);
    memorymanager__free((nuint)outgoing);
}

/**
 * @brief Copies the visible client details so a worker can build the login list later.
 * @param client_t* receiver -> newly authenticated client receiving the list
 * @attention The caller holds the clients and channels locks. The queued copy
 * contains no pointers to live client records, so disconnects cannot change it.
 * @return void
 */
void server_msg__connection__send_client_list_to_single_client(client_t* receiver)
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
        if (tag_count > INT_MAX || allocation_size > SIZE_MAX - sizeof(listed_client_t) || tag_count > (SIZE_MAX - allocation_size - sizeof(listed_client_t)) / sizeof(int))
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
        snprintf(listed->country_iso_code, sizeof(listed->country_iso_code), "%s", server_msg__profile__country_code_to_send(client));
        listed->tag_count = (int)cvector_size(client->tag_ids);
        if (listed->tag_count != 0)
        {
            listed->tag_ids = tags;
            memcpy(tags, client->tag_ids, (size_t)listed->tag_count * sizeof(int));
            tags += listed->tag_count;
        }
        // Decide what this receiver may see while the channel settings are locked.
        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE && receiver->channel_id != client->channel_id && client->channel_id < g_server_settings.max_channel_count && g_channel_array[client->channel_id].is_existing == TRUE && g_channel_array[client->channel_id].is_using_password == TRUE)
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
    ws_queue_text(receiver->p_ws_connection, outgoing, allocation_size * 6 + 8192, _server_msg_connection_internal__encode_client_list, _server_msg_connection_internal__release_client_list, server_msg__delivery__release_encoded_text);
}

/**
 * @brief Announces a connected client, building each version of the message only once.
 *
 * @param client_t* subject -> the client that connected
 *
 * @attention it doesnt need readlock, already called within writelock in msg/client/
 *
 * Recipients in other channels must not receive active microphone state or
 * music bot keys. Those recipients receive a separate version of the message.
 *
 * @return void
 */
void server_msg__connection__send_client_connect_message_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_tag_ids_array = 0;
    uint64 x = 0;
    client_t* client_in_loop = 0;
    char* texts[4] = { 0 };
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
        if (subject->is_music_bot == TRUE && client_in_loop->is_idle == FALSE && client_in_loop->channel_id == subject->channel_id)
        {
            variant |= 2;
        }
        if (texts[variant] != NULL_POINTER)
        {
            server_msg__delivery__send_text_to_client(client_in_loop, texts[variant]);
            continue;
        }

        root = server_msg__delivery__new_message("client_connect", &message);
        // carry the connecting client's tags so identity-restored tags show up on join without a page reload
        json_tag_ids_array = cJSON_CreateIntArray(subject->tag_ids, (int)cvector_size(subject->tag_ids));

        server_msg__musicbot__add_keys(message, client_in_loop, subject);
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
        cJSON_AddStringToObject(message, "country_iso_code", server_msg__profile__country_code_to_send(subject));
        cJSON_AddItemToObject(message, "tag_ids", json_tag_ids_array);

        texts[variant] = cJSON_PrintUnformatted(root);
        if (texts[variant] != NULL_POINTER)
        {
            server_msg__delivery__send_text_to_client(client_in_loop, texts[variant]);
        }
        cJSON_Delete(root);
    }
    for (variant = 0; variant < 4; variant++)
    {
        cJSON_free(texts[variant]);
    }
}

/**
 * @brief Queues a keepalive reply ahead of waiting uploads and announcements.
 * @param client_t* receiver -> authenticated connection, kept alive by the clients lock
 * @return void
 */
void server_msg__connection__send_connection_check_response_to_single_client(client_t* receiver)
{
    server_msg__delivery__queue_text(receiver->p_ws_connection, &receiver->session_keys, "{\"message\":{\"type\":\"connection_check_response\"}}", FALSE, TRUE);
}

/**
 * @brief tells a returning client that its session was adopted (fast reconnect), so it keeps its
 *        state and treats the lists that follow as a refresh. sent before authentication_status
 *
 * @param client_t* receiver -> the resumed session, already carrying the new socket
 *
 * @return void
 */
void server_msg__connection__send_fast_reconnect_ok_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("fast_reconnect_ok", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)receiver->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)receiver->channel_id);

    server_msg__delivery__send_json_to_client(receiver, root);
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
void server_msg__connection__send_client_disconnect_message_to_all_clients(uint64 client_index)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("client_disconnect", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "client_id", client_index);

    server_msg__delivery__send_json_to_all_clients(root, &g_clients_array[client_index]);

    cJSON_Delete(root);
}
