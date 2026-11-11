#include "../../util.h"
#include "../../clib/clib.h"
// This file sends music bot audio keys, song lists and playback notices.

#include "server_msg.h"
#include "../../base.h"
#include "../../../third-party/rxi-log/log.h"

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
void server_msg__musicbot__add_keys(cJSON *object, client_t *receiver, client_t *bot)
{
    cJSON *keys = NULL_POINTER;
    music_bot_audio_keys_t *audio_keys = bot->music_bot_client_extension.audio_keys;
    char key_hex[65] = { 0 };
    uint64 i = 0;
    uint64 j = 0;

    // check bot and keys
    if (bot->is_music_bot == FALSE || audio_keys == NULL_POINTER)
    {
        return;
    }

    // add the encryption flag
    cJSON_AddNumberToObject(object, "music_bot_encryption", 1);

    // check the receiver channel
    if (receiver->is_idle == TRUE || receiver->channel_id != bot->channel_id)
    {
        return;
    }

    // create the key array
    keys = cJSON_AddArrayToObject(object, "music_bot_keys");
    if (keys == NULL_POINTER)
    {
        return;
    }

    // add keys as hex
    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 32; j++)
        {
            clib__string_format(key_hex + j * 2, 3, "%02x", audio_keys->keys[i][j]);
        }
        util__json_add_owned_array(keys, cJSON_CreateString(key_hex));
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
void server_msg__musicbot__send_key(client_t *receiver, client_t *bot)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    char *plaintext = NULL_POINTER;

    // build the key message
    root = server_msg__delivery__new_message("music_bot_key", &message);
    if (message != NULL_POINTER)
    {
        cJSON_AddNumberToObject(message, "client_id", (double)bot->client_id);
        cJSON_AddNumberToObject(message, "channel_id", (double)bot->channel_id);
        server_msg__musicbot__add_keys(message, receiver, bot);
        plaintext = cJSON_PrintUnformatted(root);
    }

    // send to the receiver
    if (plaintext != NULL_POINTER)
    {
        server_msg__delivery__send_text_to_client(receiver, plaintext);
        cJSON_free(plaintext);
    }

    // free the message
    cJSON_Delete(root);
}

/**
 * @brief tells clients in the channel that a song stream has started
 *
 * @param client_t* subject -> the streaming client
 *
 * @return void
 */
void server_msg__musicbot__send_start_song_stream_message_to_clients_in_same_channel(client_t *subject)
{
    uint64 i = 0;
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    client_t *client = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create song start message
    root = server_msg__delivery__new_message("start_song_stream", &message);

    // add client and song
    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    cJSON_AddStringToObject(message, "song_name", subject->song_name);

    // find the receivers
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        // skip unauthenticated clients
        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        // only the streamer's channel hears about it, unless the marquee is shown to everyone
        if (client->channel_id != subject->channel_id && g_server_settings.is_music_bot_marquee_visible_to_everyone == FALSE)
        {
            continue;
        }

        // skip music bots
        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        // send to this client
        server_msg__delivery__send_json_to_client(client, root);
    }

    // free the message
    cJSON_Delete(root);
}

/**
 * @brief tells clients in the channel that a song stream has stopped
 *
 * @param client_t* subject -> the streaming client
 *
 * @return void
 */
void server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(client_t *subject)
{
    uint64 i = 0;
    char discard_counter[32] = { 0 };
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    client_t *client = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create song stop message
    root = server_msg__delivery__new_message("stop_song_stream", &message);

    // add client and counter
    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    if (subject->is_music_bot && subject->music_bot_client_extension.audio_keys != NULL_POINTER)
    {
        snprintf(discard_counter, sizeof(discard_counter), "%llu", (uint64)subject->music_bot_client_extension.audio_keys->packet_counter);
        cJSON_AddStringToObject(message, "discard_audio_through_counter", discard_counter);
    }

    // find the receivers
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        // skip unauthenticated clients
        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        // skip music bots
        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        // only the streamer's channel hears about it, unless the marquee is shown to everyone
        if (client->channel_id != subject->channel_id && g_server_settings.is_music_bot_marquee_visible_to_everyone == FALSE)
        {
            continue;
        }

        // send to this client
        server_msg__delivery__send_json_to_client(client, root);
    }

    // free the message
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
void server_msg__musicbot__send_song_list_to_single_client(client_t *receiver, client_t *music_bot)
{
    cJSON *song = NULL_POINTER;
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    cJSON *json_songs_array = NULL_POINTER;
    music_bot_single_song_data_t *single_song_in_loop = NULL_POINTER;
    uint64 loop_index = 0;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // build song list message
    root = server_msg__delivery__new_message("music_bot_song_list", &message);
    cJSON_AddNumberToObject(message, "bot_id", (double)music_bot->client_id);
    cJSON_AddBoolToObject(message, "is_paused", atomic_load(&music_bot->music_bot_client_extension.is_playback_paused));
    json_songs_array = cJSON_CreateArray();

    // create array of clients
    for (loop_index = 0; loop_index < MUSIC_BOT_MAX_FILE_COUNT; loop_index++)
    {
        song = NULL_POINTER;

        single_song_in_loop = &music_bot->music_bot_client_extension.songs[loop_index];

        // skip empty slots
        if (single_song_in_loop->is_existing == FALSE)
        {
            continue;
        }

        // add one song
        song = cJSON_CreateObject();
        cJSON_AddStringToObject(song, "name", single_song_in_loop->song_name);
        cJSON_AddNumberToObject(song, "id", loop_index);
        cJSON_AddNumberToObject(song, "duration_seconds", single_song_in_loop->length_seconds);
        util__json_add_owned_array(json_songs_array, song);
    }

    // add the song array
    util__json_add_owned_object(message, "songs", json_songs_array);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}
