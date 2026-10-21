// This file checks requests to create or remove music bots and to manage or play their songs.
#include "client_msg.h"
#include "../../core/client_session.h"
#include "../server/server_msg.h"
#include "../../core/musicbot.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_musicbot_internal__is_json_start_song_stream_message_valid(cJSON* json_root);
static boole _client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid(cJSON* json_root);
static boole _client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid(cJSON* json_root);
static boole _client_msg_musicbot_internal__is_musicbot_get_song_list_request_valid(cJSON* json_root);
static boole _client_msg_musicbot_internal__is_remove_song_from_music_bot_request_valid(cJSON* json_root);

/**
 * @brief validates a start-song-stream message: the song name is present, non-empty and within the length limit
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_musicbot_internal__is_json_start_song_stream_message_valid(cJSON* json_root)
{
    cJSON* json_song_name = 0;
    cJSON* json_message_object = 0;
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
void client_msg__musicbot__process_start_song_stream_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* json_song_name = 0;
    cJSON* json_message_object = 0;

    status = _client_msg_musicbot_internal__is_json_start_song_stream_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_start_song_stream_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");

    client->is_streaming_song = TRUE;
    clib__null_memory(client->song_name, SONG_NAME_MAX_LENGTH);
    clib__copy_memory(json_song_name->valuestring, client->song_name, clib__utf8_string_length(json_song_name->valuestring), SONG_NAME_MAX_LENGTH);

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
void client_msg__musicbot__process_stop_song_stream_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_stop_song_stream_end;
    }

    server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_stop_song_stream_end:

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a create-music-bot request: the channel id and bot username are present and valid
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_music_bot_username = 0;
    uint64 json_music_bot_username_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (json_channel_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_channel_id) == FALSE)
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
void client_msg__musicbot__process_create_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_music_bot_username = 0;

    client_t* admin = 0;
    client_t* music_bot = 0;
    int64 new_music_bot_index = 0;

    status = _client_msg_musicbot_internal__is_client_msg__process_create_music_bot_request_valid(json_root);
    if (status == FALSE)
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
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_create_music_bot_end;
    }

    // check if channel exists
    if (g_channel_array[json_channel_id->valueint].is_existing == FALSE)
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

    // load mp3 file on server side from disk. don't upload it yet, that will on on the end

    // first create client
    // Bot audio uses its own keys. The server never receives clients' channel keys.
    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_create_request \n");

    new_music_bot_index = client_session__get_new_index_for_client();

    DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__musicbot__process_create_request", new_music_bot_index, "\n");

    if (new_music_bot_index < 0)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

    music_bot = &g_clients_array[new_music_bot_index];
    music_bot->music_bot_client_extension.songs = (music_bot_single_song_data_t*)memorymanager__allocate(sizeof(music_bot_single_song_data_t) * MUSIC_BOT_MAX_FILE_COUNT, MEMALLOC_MUSICBOT_SONG);
    if (music_bot->music_bot_client_extension.songs == NULL_POINTER)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

    music_bot->music_bot_client_extension.audio_keys = calloc(1, sizeof(music_bot_audio_keys_t));
    if (music_bot->music_bot_client_extension.audio_keys == NULL_POINTER ||
        util__fill_secure_random_bytes(&music_bot->music_bot_client_extension.audio_keys->keys[0][0], 128) == FALSE
        // the packet counter goes into the encryption of every bot frame and must never repeat under the same
        // keys; a random start keeps it apart from the counters of an earlier bot in the same slot
        || util__fill_secure_random_bytes((unsigned char*)&music_bot->music_bot_client_extension.audio_keys->packet_counter, 8) == FALSE)
    {
        free(music_bot->music_bot_client_extension.audio_keys);
        memorymanager__free((nuint)music_bot->music_bot_client_extension.songs);
        clib__null_memory(music_bot, sizeof(*music_bot));
        goto label_client_msg__process_create_music_bot_end;
    }

    music_bot->is_authenticated = TRUE;
    music_bot->timestamp_connected = util__get_timestamp_ms();
    music_bot->p_ws_connection = NULL_POINTER;
    music_bot->is_existing = TRUE;
    music_bot->client_id = new_music_bot_index;
    music_bot->audio_state = AUDIO_STATE__PUSH_TO_TALK_ACTIVE;
    music_bot->is_music_bot = TRUE;
    music_bot->timestamp_connected = util__get_timestamp_ms();
    music_bot->is_streaming_song = TRUE;
    music_bot->channel_id = json_channel_id->valueint;

    DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__musicbot__process_create_request client id is -> ", music_bot->client_id, "\n");
    DBG_CLIENT_MESSAGE log_info("%s %p %s", "client_msg__musicbot__process_create_request client id is -> ", (void*)&music_bot->client_id, "\n");

    clib__null_memory(music_bot->username, USERNAME_MAX_LENGTH);
    clib__copy_memory(json_music_bot_username->valuestring, music_bot->username, clib__utf8_string_length(json_music_bot_username->valuestring), USERNAME_MAX_LENGTH);

    music_bot->music_bot_client_extension.is_music_bot_running = TRUE;
    if (pthread_create((pthread_t*)&music_bot->music_bot_client_extension.music_bot_pthread_handle, 0, musicbot__threadstart, (void*)music_bot) != 0)
    {
        free(music_bot->music_bot_client_extension.audio_keys);
        memorymanager__free((nuint)music_bot->music_bot_client_extension.songs);
        clib__null_memory(music_bot, sizeof(*music_bot));
        goto label_client_msg__process_create_music_bot_end;
    }
    g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel = TRUE;
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
static boole _client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (json_channel_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid json_channel_id == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_channel_id) == FALSE)
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
void client_msg__musicbot__process_delete_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    client_t* music_bot = 0;
    client_t* admin = 0;
    uint64 bot_loop_index = 0;

    status = _client_msg_musicbot_internal__is_client_msg__process_delete_music_bot_request_valid(json_root);
    if (status == FALSE)
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
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_delete_music_bot_end;
    }

    // check if channel exists
    if (g_channel_array[json_channel_id->valueint].is_existing == FALSE)
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
            server_msg__connection__send_client_disconnect_message_to_all_clients(music_bot->client_id);
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
static boole _client_msg_musicbot_internal__is_musicbot_get_song_list_request_valid(cJSON* json_root)
{
    cJSON* json_musicbot_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (json_musicbot_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_musicbot_id == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_musicbot_id) == FALSE)
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
void client_msg__musicbot__process_get_song_list_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_musicbot_id = 0;
    client_t* client = 0;
    client_t* music_bot = 0;

    status = _client_msg_musicbot_internal__is_musicbot_get_song_list_request_valid(json_root);
    if (status == FALSE)
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

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client->is_authenticated == FALSE || client->is_existing == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    if (client->is_admin == FALSE)
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
 * @brief validates a remove-song-from-music-bot request: the song id and music bot id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_musicbot_internal__is_remove_song_from_music_bot_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_song_id = 0;
    cJSON* json_musicbot_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_id");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (json_song_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(json_song_id) \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_song_id) == FALSE)
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

    if (cJSON_IsNumber(json_musicbot_id) == FALSE)
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
void client_msg__musicbot__process_remove_song_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_song_id = 0;
    cJSON* json_musicbot_id = 0;

    client_t* sender_client = 0;
    client_t* music_bot = 0;

    status = _client_msg_musicbot_internal__is_remove_song_from_music_bot_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__musicbot__process_remove_song_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_id");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

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

    if (sender_client->is_admin == FALSE)
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
