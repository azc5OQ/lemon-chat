// This file checks requests to create or remove music bots and to manage or play their songs.
#include "client_msg.h"
#include "../../core/permissions.h"
#include "../../core/client_session.h"
#include "../server/server_msg.h"
#include "../../core/musicbot.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_musicbot_internal__is_json_start_song_stream_message_valid(cJSON *json_root);
static boole _client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid(cJSON *json_root);
static boole _client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid(cJSON *json_root);
static boole _client_msg_musicbot_internal__is_musicbot_get_song_list_request_valid(cJSON *json_root);
static boole _client_msg_musicbot_internal__is_remove_song_from_music_bot_request_valid(cJSON *json_root);

/**
 * @brief Validates a whole-number slot index before the request uses the cached integer value.
 */
static boole _client_msg_musicbot_internal__is_index_valid(cJSON *value, uint64 count)
{
    uint64 index = 0;

    return (boole)(count > 0 && util__json_read_uint64(value, count - 1, &index) == TRUE);
}

/**
 * @brief validates a start-song-stream message: the song name is present, non-empty and within the
 *        length limit
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_musicbot_internal__is_json_start_song_stream_message_valid(cJSON *json_root)
{
    cJSON *json_song_name = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    int64 song_name_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");
    if (cJSON_IsString(json_song_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_json_start_song_stream_message_valid cJSON_IsString(json_song_name) \n");
        return FALSE;
    }

    if (json_song_name->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_json_start_song_stream_message_valid json_song_name->valuestring == NULL_POINTER \n");
        return FALSE;
    }

    song_name_length = clib__utf8_string_length_check_max_length(json_song_name->valuestring, SONG_NAME_MAX_LENGTH - 1);
    if (song_name_length == -1 || song_name_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_json_start_song_stream_message_valid song_name_length == -1 || song_name_length == 0 \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes a client's request to start streaming a song to its channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__musicbot__process_start_song_stream_message(cJSON *json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t *client = NULL_POINTER;
    cJSON *json_song_name = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    status = _client_msg_musicbot_internal__is_json_start_song_stream_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (base__is_current_client_request(sender_client_id) == FALSE || client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_start_song_stream_end;
    }

    clib__read_lock(&g_channels_global_rwlock_guard);
    status = client->channel_id < g_server_settings.max_channel_count && permissions__can_use_channel(client, &g_channel_array[client->channel_id], PERMISSION_TALK_POWER);
    clib__unlock(&g_channels_global_rwlock_guard);
    if (status == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        goto label_client_msg__process_start_song_stream_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");

    client->is_streaming_song = TRUE;
    clib__null_memory(client->song_name, SONG_NAME_MAX_LENGTH);
    clib__string_copy(client->song_name, sizeof(client->song_name), json_song_name->valuestring);

    server_msg__musicbot__send_start_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_start_song_stream_end:

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request to stop streaming its song
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__musicbot__process_stop_song_stream_message(cJSON *json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t *client = NULL_POINTER;

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (base__is_current_client_request(sender_client_id) == FALSE || client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_stop_song_stream_end;
    }

    client->is_streaming_song = FALSE;
    clib__null_memory(client->song_name, sizeof(client->song_name));
    server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_stop_song_stream_end:

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a create-music-bot request: the channel id and bot username are present and
 *        valid
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid(cJSON *json_root)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_music_bot_username = NULL_POINTER;
    uint64 json_music_bot_username_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (json_channel_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id == NULL_POINTER \n");
        return FALSE;
    }

    if (_client_msg_musicbot_internal__is_index_valid(json_channel_id, g_server_settings.max_channel_count) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id is invalid");
        return FALSE;
    }

    json_music_bot_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "music_bot_username");

    if (json_music_bot_username == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_music_bot_username == NULL_POINTER \n");
        return FALSE;
    }

    if (json_music_bot_username->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid json_music_bot_username->valuestring == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsString(json_music_bot_username) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid cJSON_IsString(json_music_bot_username)");
        return FALSE;
    }

    json_music_bot_username_length = clib__utf8_string_length(json_music_bot_username->valuestring);

    if (json_music_bot_username_length == 0 || json_music_bot_username_length >= USERNAME_MAX_LENGTH)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin request to create a music bot client in a channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__musicbot__process_create_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    cJSON *json_music_bot_username = NULL_POINTER;
    client_t *admin = NULL_POINTER;
    client_t *music_bot = NULL_POINTER;

    if (_client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_music_bot is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_music_bot_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "music_bot_username");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }
    admin = &g_clients_array[sender_client_id];

    if (permissions__is_allowed(admin, PERMISSION_CREATE_MUSIC_BOTS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_create_music_bot_end;
    }

    // Repeat the capacity check after taking the locks: settings may shrink
    // the channel allocation while this request waits.
    if (_client_msg_musicbot_internal__is_index_valid(json_channel_id, g_server_settings.max_channel_count) == FALSE || g_channel_array[json_channel_id->valueint].is_existing == FALSE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

#ifndef MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL
    // one music bot per channel. define MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL (definitions.h)
    // to lift this limit - debug aid only: several bots give several simultaneous audio senders for
    // testing multi-speaker mixing without needing several people. everything downstream (per-bot frame
    // ids, per-bot decoders, delete-all-bots-in-channel) works in both modes
    if (g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel == TRUE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }
#endif

    // music bots are not allowed in temp channels
    if (g_channel_array[json_channel_id->valueint].is_temp_channel == TRUE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

    music_bot = musicbot__allocate((uint64)json_channel_id->valueint, json_music_bot_username->valuestring);
    if (music_bot == NULL_POINTER)
    {
        goto label_client_msg__process_create_music_bot_end;
    }
    if (!musicbot__start(music_bot))
    {
        musicbot__free_unstarted(music_bot);
        goto label_client_msg__process_create_music_bot_end;
    }
    server_msg__connection__send_client_connect_message_to_all_clients(music_bot);

label_client_msg__process_create_music_bot_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a delete-music-bot request: the channel id is present and valid
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid(cJSON *json_root)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (json_channel_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid json_channel_id == NULL_POINTER \n");
        return FALSE;
    }

    if (_client_msg_musicbot_internal__is_index_valid(json_channel_id, g_server_settings.max_channel_count) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin request to delete a music bot from its channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__musicbot__process_delete_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_channel_id = NULL_POINTER;
    client_t *music_bot = NULL_POINTER;
    client_t *admin = NULL_POINTER;
    uint64 bot_loop_index = 0;

    if (_client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_delete_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto label_client_msg__process_delete_music_bot_end;
    }
    admin = &g_clients_array[sender_client_id];

    if (permissions__is_allowed(admin, PERMISSION_DELETE_MUSIC_BOTS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_delete_music_bot_end;
    }

    // Repeat the capacity check under the resize locks before indexing.
    if (_client_msg_musicbot_internal__is_index_valid(json_channel_id, g_server_settings.max_channel_count) == FALSE || g_channel_array[json_channel_id->valueint].is_existing == FALSE)
    {
        goto label_client_msg__process_delete_music_bot_end;
    }

    // check if there is at least one music bot in the channel
    if (g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel == FALSE)
    {
        goto label_client_msg__process_delete_music_bot_end;
    }

    // tear down EVERY music bot in the channel (there can be several since multi-bot was allowed).
    // nothing is freed here: musicbot__begin_delete only signals the bot's stream thread and hides the
    // bot; a detached reaper thread joins the stream thread and THEN frees the songs and the slot.
    // freeing inline here was a use-after-free against the stream/preload threads, and nulling the
    // client_t made the slot reusable while the old bot thread still wrote into it
    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_delete_request \n");

    for (bot_loop_index = 0; bot_loop_index < g_server_settings.max_client_count; bot_loop_index++)
    {
        music_bot = &g_clients_array[bot_loop_index];

        if (music_bot->is_existing == FALSE || music_bot->is_music_bot == FALSE || music_bot->channel_id != (uint64)json_channel_id->valueint)
        {
            continue;
        }

        DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__musicbot__process_delete_request music bot id -> ", music_bot->client_id, "\n");
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__musicbot__process_delete_request music bot username -> ", music_bot->username, "\n");

        if (musicbot__begin_delete(music_bot) == TRUE)
        {
            server_msg__connection__send_client_disconnect_message_to_all_clients(music_bot->client_id, NULL_POINTER);
        }
    }

    g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel = FALSE;
    for (bot_loop_index = 0; bot_loop_index < g_server_settings.max_client_count; bot_loop_index++)
    {
        music_bot = &g_clients_array[bot_loop_index];
        if (music_bot->is_existing == TRUE && music_bot->is_music_bot == TRUE && music_bot->channel_id == (uint64)json_channel_id->valueint)
        {
            g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel = TRUE;
            break;
        }
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_delete_request END \n");

label_client_msg__process_delete_music_bot_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a music-bot song-list request: the music bot id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_musicbot_internal__is_musicbot_get_song_list_request_valid(cJSON *json_root)
{
    cJSON *json_musicbot_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (json_musicbot_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_musicbot_id == NULL_POINTER \n");
        return FALSE;
    }

    if (_client_msg_musicbot_internal__is_index_valid(json_musicbot_id, g_server_settings.max_client_count) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(musicbot_id) \n");
        return FALSE;
    }

    if (json_musicbot_id->valueint < 0 || json_musicbot_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_musicbot_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes a client's request for a music bot's current song list
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__musicbot__process_get_song_list_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_musicbot_id = NULL_POINTER;
    client_t *client = NULL_POINTER;
    client_t *music_bot = NULL_POINTER;

    if (_client_msg_musicbot_internal__is_musicbot_get_song_list_request_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_get_song_list_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    clib__read_lock(&g_clients_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within read lock like here
    client = &g_clients_array[sender_client_id];

    if (base__is_current_client_request(sender_client_id) == FALSE || client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client->is_authenticated == FALSE || client->is_existing == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    if (permissions__is_allowed(client, PERMISSION_UPLOAD_MUSIC_TRACKS) == FALSE &&
        permissions__is_allowed(client, PERMISSION_DELETE_MUSIC_TRACKS) == FALSE &&
        permissions__is_allowed(client, PERMISSION_CONTROL_MUSIC_PLAYBACK) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client->is_admin == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    music_bot = &g_clients_array[json_musicbot_id->valueint];
    if (music_bot->is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "music_bot->is_music_bot == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__musicbot__send_song_list_to_single_client");

    server_msg__musicbot__send_song_list_to_single_client(&g_clients_array[sender_client_id], music_bot);

label_client_msg__process_musicbot_get_song_list_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a remove-song-from-music-bot request: the song id and music bot id are present
 *        and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_musicbot_internal__is_remove_song_from_music_bot_request_valid(cJSON *json_root)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_song_id = NULL_POINTER;
    cJSON *json_musicbot_id = NULL_POINTER;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_id");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (json_song_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(json_song_id) \n");
        return FALSE;
    }

    if (_client_msg_musicbot_internal__is_index_valid(json_song_id, MUSIC_BOT_MAX_FILE_COUNT) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(json_song_id) \n");
        return FALSE;
    }

    if (json_song_id->valueint < 0 || json_song_id->valueint >= MUSIC_BOT_MAX_FILE_COUNT)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_song_id is invalid");
        return FALSE;
    }

    if (json_musicbot_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(musicbot_id) \n");
        return FALSE;
    }

    if (_client_msg_musicbot_internal__is_index_valid(json_musicbot_id, g_server_settings.max_client_count) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(musicbot_id) \n");
        return FALSE;
    }

    if (json_musicbot_id->valueint < 0 || json_musicbot_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_musicbot_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin request to remove a song from a music bot and refreshes the song list
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__musicbot__process_remove_song_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_song_id = NULL_POINTER;
    cJSON *json_musicbot_id = NULL_POINTER;
    client_t *sender_client = NULL_POINTER;
    client_t *music_bot = NULL_POINTER;

    if (_client_msg_musicbot_internal__is_remove_song_from_music_bot_request_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_remove_song_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_id");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }
    sender_client = &g_clients_array[sender_client_id];
    music_bot = &g_clients_array[json_musicbot_id->valueint];

    if (music_bot->is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "not music bot");
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }

    if (sender_client->is_existing == FALSE || sender_client->is_authenticated == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "not valid client");
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }

    if (permissions__is_allowed(sender_client, PERMISSION_DELETE_MUSIC_TRACKS) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "sender is not admin");
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }

    // questionable thread safety
    musicbot__remove_song(music_bot, json_song_id->valueint);

    server_msg__musicbot__send_song_list_to_single_client(&g_clients_array[sender_client_id], music_bot);

    if (music_bot->music_bot_client_extension.music_bot_songs_count == 0)
    {
        server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(music_bot);
    }

label_client_msg__process_remove_song_from_music_bot_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}
