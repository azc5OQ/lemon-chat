// This file tells clients about channels, membership changes and the selected channel maintainer.

#include "server_msg.h"
#include "../../base.h"
#include "../../core/channel.h"
#include "../../util.h"
#include "../../../third-party/rxi-log/log.h"

/**
 * @brief tells a single client that the channel they tried to join is full (used by the capacity gate)
 *
 * @param client_t* receiver -> the client to notify
 * @param uint64 channel_id -> the full channel
 *
 * @return void
 */
void server_msg__channel__send_full_to_single_client(client_t* receiver, uint64 channel_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("channel_full", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends channel list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @return void
 */
void server_msg__channel__send_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_channel_array = 0;
    uint64 i = 0;
    cJSON* single_channel = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_list", &message);
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
        channel__write_required_powers(single_channel, &g_channel_array[i]);
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

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
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
void server_msg__channel__send_maintainer_id_to_single_client(client_t* receiver, channel_t* channel, uint64 maintainer_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_maintainer_id", &message);

    cJSON_AddNumberToObject(message, "maintainer_id", (double)maintainer_id);
    cJSON_AddNumberToObject(message, "maintainer_generation", (double)channel->maintainer_generation);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);

    server_msg__delivery__send_json_to_client(receiver, root);
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
void server_msg__channel__send_create_message_to_all_clients(channel_t* channel, client_t* creator)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("channel_create", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(message, "parent_channel_id", channel->parent_channel_id);
    cJSON_AddStringToObject(message, "name", channel->name);
    cJSON_AddStringToObject(message, "description", channel->description);
    cJSON_AddNumberToObject(message, "maintainer_id", channel->maintainer_id);
    cJSON_AddBoolToObject(message, "is_using_password", channel->is_using_password);
    cJSON_AddBoolToObject(message, "is_audio_enabled", channel->is_audio_enabled);
    channel__write_required_powers(message, channel);
    cJSON_AddBoolToObject(message, "is_video_stream_enabled", channel->is_video_stream_enabled);
    cJSON_AddBoolToObject(message, "is_root_channel", channel->is_root_channel);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);
    cJSON_AddBoolToObject(message, "is_temp_channel", channel->is_temp_channel);
    cJSON_AddBoolToObject(message, "is_client_limit_active", channel->is_client_limit_active);
    cJSON_AddNumberToObject(message, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(message, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)channel->icon_id);
    cJSON_AddNumberToObject(message, "channel_creator_id", (double)creator->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

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
void server_msg__channel__send_edit_message_to_all_clients(channel_t* channel, client_t* editor)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("channel_edit", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "channel_id", channel->channel_id);
    cJSON_AddStringToObject(message, "channel_name", channel->name);
    cJSON_AddStringToObject(message, "channel_description", channel->description);
    cJSON_AddBoolToObject(message, "is_using_password", (cJSON_bool)channel->is_using_password);
    cJSON_AddBoolToObject(message, "is_audio_enabled", (cJSON_bool)channel->is_audio_enabled);
    channel__write_required_powers(message, channel);
    cJSON_AddBoolToObject(message, "is_video_stream_enabled", (cJSON_bool)channel->is_video_stream_enabled);
    cJSON_AddBoolToObject(message, "is_client_limit_active", (cJSON_bool)channel->is_client_limit_active);
    cJSON_AddNumberToObject(message, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(message, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)channel->icon_id);
    cJSON_AddNumberToObject(message, "channel_editor_id", (double)editor->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

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
void server_msg__channel__send_join_message_to_all_clients(client_t* subject, channel_t* new_channel)
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

    root = server_msg__delivery__new_message("channel_join", &message);

    root_hidden = server_msg__delivery__new_message("channel_join", &message_hidden);

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
        server_msg__delivery__send_text_to_client(client_in_loop, is_hide_client_active == TRUE ? plaintext_hidden : plaintext);
    }

    util__free_json_message(root, plaintext);
    util__free_json_message(root_hidden, plaintext_hidden);
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
void server_msg__channel__send_join_message_to_single_client(client_t* receiver, client_t* subject, channel_t* new_channel)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    char* song_name = 0;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    root = server_msg__delivery__new_message("channel_join", &message);

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

    server_msg__delivery__send_json_to_client(receiver, root);
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
void server_msg__channel__send_maintainer_id_to_clients_in_same_channel(channel_t* channel, uint64 maintainer_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_maintainer_id", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddNumberToObject(message, "maintainer_id", maintainer_id);
    cJSON_AddNumberToObject(message, "maintainer_generation", (double)channel->maintainer_generation);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);

    server_msg__delivery__send_json_to_channel(root, channel->channel_id, NULL_POINTER);

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
void server_msg__channel__send_delete_message_to_all_clients(uint64 deleted_channel_id, uint64 channel_deletor_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_delete", &message);

    cJSON_AddNumberToObject(message, "channel_id", deleted_channel_id);
    if (channel_deletor_id == UINT64_MAX)
    {
        cJSON_AddNullToObject(message, "channel_deletor_id");
    }
    else
    {
        cJSON_AddNumberToObject(message, "channel_deletor_id", channel_deletor_id);
    }

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

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
void server_msg__channel__send_icon_changed_event_to_all_clients(uint64 channel_id, boole has_channel_icon, uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_icon_changed", &message);

    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);
    cJSON_AddItemToObject(message, "has_channel_icon", cJSON_CreateBool(has_channel_icon == TRUE));
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)icon_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

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
void server_msg__channel__send_call_event_to_idle_client(client_t* receiver, client_t* caller)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("call", &message);

    cJSON_AddNumberToObject(message, "caller_client_id", caller->client_id);

    server_msg__delivery__send_json_to_client(receiver, root);
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
void server_msg__channel__send_client_going_to_idle_mode_info_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("client_going_to_idle_mode", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

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
void server_msg__channel__send_client_coming_back_from_idle_mode_info_to_all_clients(client_t* subject, uint64 channel_the_client_joins)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("client_coming_back_from_idle_mode", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel_the_client_joins);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}
