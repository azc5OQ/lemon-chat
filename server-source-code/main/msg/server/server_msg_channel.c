#include "../../core/channel_groups.h"
// This file tells clients about channels, membership changes and the selected channel maintainer.

#include "server_msg.h"
#include "../../base.h"
#include "../../core/channel.h"
#include "../../util.h"
#include "../../../third-party/rxi-log/log.h"

/**
 * @brief tells a single client that the channel they tried to join is full (used by the capacity
 *        gate)
 *
 * @param client_t* receiver -> the client to notify
 * @param uint64 channel_id -> the full channel
 *
 * @return void
 */
void server_msg__channel__send_full_to_single_client(client_t *receiver, uint64 channel_id)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // create channel full message
    root = server_msg__delivery__new_message("channel_full", &message);

    // add the channel id
    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends a message about one channel to every client in one of two versions: the complete
 *        one, or one with an empty description for the clients that may not read this
 *        channel's description.
 *
 * @param cJSON* root -> the complete message; this function changes it and the caller deletes it
 * @param cJSON* message -> the "message" object inside root
 * @param const char* description_key -> name of the field that holds the description
 * @param channel_t* channel -> the channel the message is about
 *
 * @attention the caller holds the clients lock and the channels lock
 *
 * @return void
 */
static void _server_msg_channel_internal__send_to_all_clients_with_visible_description(cJSON *root, cJSON *message, const char *description_key, channel_t *channel)
{
    char *plaintext = NULL_POINTER;
    char *plaintext_hidden = NULL_POINTER;
    client_t *client_in_loop = NULL_POINTER;
    uint64 x = 0;

    // complete version
    cJSON_AddBoolToObject(message, "is_description_hidden", FALSE);
    plaintext = cJSON_PrintUnformatted(root);

    // version without description
    cJSON_ReplaceItemInObjectCaseSensitive(message, description_key, cJSON_CreateString(""));
    cJSON_ReplaceItemInObjectCaseSensitive(message, "is_description_hidden", cJSON_CreateBool(TRUE));
    plaintext_hidden = cJSON_PrintUnformatted(root);

    // send to clients
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];
        if (client_in_loop->is_existing == FALSE || client_in_loop->is_authenticated == FALSE || client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }
        server_msg__delivery__send_text_to_client(client_in_loop, channel__is_description_visible(client_in_loop, channel) == TRUE ? plaintext : plaintext_hidden);
    }

    // free both texts
    cJSON_free(plaintext);
    cJSON_free(plaintext_hidden);
}

/**
 * @brief Sends one client a channel's description, or tells it that the description is hidden
 *        from now on. Used when the client's rights or the channel's required view power changed.
 *
 * @param client_t* receiver -> the client that is told
 * @param channel_t* channel -> the channel
 * @param boole is_visible -> TRUE to send the description; FALSE to send an empty one marked as hidden
 *
 * @attention the caller holds the clients lock and the channels lock
 *
 * @return void
 */
void server_msg__channel__send_description_to_single_client(client_t *receiver, channel_t *channel, boole is_visible)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create the message
    root = server_msg__delivery__new_message("channel_description", &message);

    // add the fields
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddStringToObject(message, "description", is_visible == TRUE ? channel->description : "");
    cJSON_AddBoolToObject(message, "is_description_hidden", is_visible == FALSE);

    // send and free
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends channel list to single client. Called when client sents
 *        challenge_response message to server during authentication process.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @return void
 */
void server_msg__channel__send_list_to_single_client(client_t *receiver)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    cJSON *json_channel_array = NULL_POINTER;
    uint64 i = 0;
    cJSON *single_channel = NULL_POINTER;
    boole is_description_visible = FALSE;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create channel list message
    root = server_msg__delivery__new_message("channel_list", &message);
    json_channel_array = cJSON_CreateArray();

    // add every channel
    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        // skip empty slots
        if (g_channel_array[i].is_existing == FALSE)
        {
            continue;
        }

        // add id and name
        single_channel = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_channel, "channel_id", g_channel_array[i].channel_id);
        cJSON_AddNumberToObject(single_channel, "parent_channel_id", g_channel_array[i].parent_channel_id);
        cJSON_AddStringToObject(single_channel, "name", g_channel_array[i].name);

        // a client that may not read this channel's description gets an empty one
        is_description_visible = channel__is_description_visible(receiver, &g_channel_array[i]);
        cJSON_AddStringToObject(single_channel, "description", is_description_visible == TRUE ? g_channel_array[i].description : "");
        cJSON_AddBoolToObject(single_channel, "is_description_hidden", is_description_visible == FALSE);

        // add the channel settings
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

        // add channel to array
        util__json_add_owned_array(json_channel_array, single_channel);
    }

    // add the channel array
    util__json_add_owned_object(message, "channels", json_channel_array);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends the maintainer of the current channel the client just joined. Used
 *        when the client joins the server or when it switches channel
 *
 * @param client_t* receiver -> the client to send the maintainer id to
 * @param channel_t* channel -> the channel
 * @param uint64 maintainer_id -> the maintainer client id to announce, as a payload value
 *
 * @attention sometimes parameter id_of_client_that_joined_the_channel can have the value as id_of_client_that_is_maintainer_of_channel
 *
 * @return void
 */
void server_msg__channel__send_maintainer_id_to_single_client(client_t *receiver, channel_t *channel, uint64 maintainer_id)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create maintainer message
    root = server_msg__delivery__new_message("channel_maintainer_id", &message);

    // add maintainer and channel
    cJSON_AddNumberToObject(message, "maintainer_id", (double)maintainer_id);
    cJSON_AddNumberToObject(message, "maintainer_generation", (double)channel->maintainer_generation);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);

    // send to the receiver
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
void server_msg__channel__send_create_message_to_all_clients(channel_t *channel, client_t *creator)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // create new channel message
    root = server_msg__delivery__new_message("channel_create", &message);

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // add ids and texts
    cJSON_AddNumberToObject(message, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(message, "parent_channel_id", channel->parent_channel_id);
    cJSON_AddStringToObject(message, "name", channel->name);
    cJSON_AddStringToObject(message, "description", channel->description);
    cJSON_AddNumberToObject(message, "maintainer_id", channel->maintainer_id);

    // add the channel settings
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

    // add the creator id
    cJSON_AddNumberToObject(message, "channel_creator_id", (double)creator->client_id);

    // send both versions
    _server_msg_channel_internal__send_to_all_clients_with_visible_description(root, message, "description", channel);

    // free the message
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
void server_msg__channel__send_edit_message_to_all_clients(channel_t *channel, client_t *editor)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // create channel edit message
    root = server_msg__delivery__new_message("channel_edit", &message);

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // add id, name, description
    cJSON_AddNumberToObject(message, "channel_id", channel->channel_id);
    cJSON_AddBoolToObject(message, "is_temp_channel", channel->is_temp_channel);
    cJSON_AddStringToObject(message, "channel_name", channel->name);
    cJSON_AddStringToObject(message, "channel_description", channel->description);

    // add the channel settings
    cJSON_AddBoolToObject(message, "is_using_password", (cJSON_bool)channel->is_using_password);
    cJSON_AddBoolToObject(message, "is_audio_enabled", (cJSON_bool)channel->is_audio_enabled);
    channel__write_required_powers(message, channel);
    cJSON_AddBoolToObject(message, "is_video_stream_enabled", (cJSON_bool)channel->is_video_stream_enabled);
    cJSON_AddBoolToObject(message, "is_client_limit_active", (cJSON_bool)channel->is_client_limit_active);
    cJSON_AddNumberToObject(message, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(message, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)channel->icon_id);

    // add the editor id
    cJSON_AddNumberToObject(message, "channel_editor_id", (double)editor->client_id);

    // send both versions
    _server_msg_channel_internal__send_to_all_clients_with_visible_description(root, message, "channel_description", channel);

    // free the message
    cJSON_Delete(root);
}

/**
 * @brief sends channel join message to all clients
 *
 * @param client_t* subject -> self explanatory
 * @param channel_t* new_channel -> id of server message
 *
 * @attention this function gets called AFTER new channel id is assigned to client struct, not before so this function must assume the client is already in its new channel, if it is going to do any logical operations based on that
 *
 * @return void
 */
void server_msg__channel__send_join_message_to_all_clients(client_t *subject, channel_t *new_channel, const char *reason)
{
    boole is_hide_client_active = FALSE;
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    cJSON *root_hidden = NULL_POINTER;
    cJSON *message_hidden = NULL_POINTER;
    uint64 x = 0;
    char *plaintext = NULL_POINTER;
    char *plaintext_hidden = NULL_POINTER;
    client_t *client_in_loop = NULL_POINTER;
    char *song_name = NULL_POINTER;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create the join message
    root = server_msg__delivery__new_message("channel_join", &message);

    // create the hidden version
    root_hidden = server_msg__delivery__new_message("channel_join", &message_hidden);

    // add reason to both
    if (reason != NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "reason", reason);
        cJSON_AddStringToObject(message_hidden, "reason", reason);
    }

    // clients not in same channel will not receive real time microphone usage information from the client that switched the channel
    is_streaming_song = subject->is_streaming_song;
    audio_state = subject->audio_state;
    song_name = subject->song_name;

    // clear talking and song
    if (subject->channel_id != new_channel->channel_id)
    {
        if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        is_streaming_song = FALSE;
        song_name = "";
    }

    // fill the visible version
    cJSON_AddNumberToObject(message, "channel_id", new_channel->channel_id);
    cJSON_AddNumberToObject(message, "channel_group_id", channel_groups__role(subject, new_channel));
    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    cJSON_AddNumberToObject(message, "audio_state", audio_state);
    cJSON_AddBoolToObject(message, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(message, "song_name", song_name);

    // convert it to text
    plaintext = cJSON_PrintUnformatted(root);

    // fill the hidden version
    cJSON_AddNumberToObject(message_hidden, "channel_id", (double)-1);
    cJSON_AddNumberToObject(message_hidden, "channel_group_id", 0);
    cJSON_AddNumberToObject(message_hidden, "client_id", subject->client_id);
    cJSON_AddNumberToObject(message_hidden, "audio_state", audio_state);
    cJSON_AddBoolToObject(message_hidden, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(message_hidden, "song_name", song_name);
    plaintext_hidden = cJSON_PrintUnformatted(root_hidden);

    // send the matching version
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        is_hide_client_active = FALSE;

        client_in_loop = &g_clients_array[x];

        // skip empty slots
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        // skip unauthenticated clients
        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        // skip music bots
        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        // check member visibility
        is_hide_client_active = (boole)(channel__are_members_visible(client_in_loop, new_channel) == FALSE);

        // a client that may not see who is in the new channel gets the variant without the channel id
        server_msg__delivery__send_text_to_client(client_in_loop, is_hide_client_active == TRUE ? plaintext_hidden : plaintext);
    }

    // free both versions
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
 *        the message carries the reason "visibility": the subject did not move. the receiver is
 *        told where the subject is because it may see that now, or that the subject's place is
 *        hidden from now on.
 *
 * @param client_t* subject -> the client whose channel change is being announced
 * @param channel_t* new_channel -> channel that the subject joined
 * @param client_t* receiver -> client that receives the channel change
 * @param boole is_channel_visible -> FALSE sends -1 instead of the channel id: the receiver may not see where the subject is
 *
 * @return void
 */
void server_msg__channel__send_join_message_to_single_client(client_t *receiver, client_t *subject, channel_t *new_channel, boole is_channel_visible)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    char *song_name = NULL_POINTER;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    // create the join message
    root = server_msg__delivery__new_message("channel_join", &message);

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // clients not in same channel will not receive real time microphone usage information from the client that switched the channel
    is_streaming_song = subject->is_streaming_song;
    audio_state = subject->audio_state;
    song_name = subject->song_name;

    // clear talking and song
    if (subject->channel_id != new_channel->channel_id)
    {
        if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        is_streaming_song = FALSE;
        song_name = "";
    }

    // add the fields
    cJSON_AddStringToObject(message, "reason", "visibility");
    cJSON_AddNumberToObject(message, "channel_id", is_channel_visible == TRUE ? (double)new_channel->channel_id : (double)-1);
    cJSON_AddNumberToObject(message, "channel_group_id", is_channel_visible == TRUE ? (double)channel_groups__role(subject, new_channel) : 0);
    cJSON_AddNumberToObject(message, "client_id", subject->client_id);
    cJSON_AddNumberToObject(message, "audio_state", audio_state);
    cJSON_AddBoolToObject(message, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(message, "song_name", song_name);

    // send to the receiver
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
void server_msg__channel__send_maintainer_id_to_clients_in_same_channel(channel_t *channel, uint64 maintainer_id)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create maintainer message
    root = server_msg__delivery__new_message("channel_maintainer_id", &message);

    // add maintainer and channel
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddNumberToObject(message, "maintainer_id", maintainer_id);
    cJSON_AddNumberToObject(message, "maintainer_generation", (double)channel->maintainer_generation);
    cJSON_AddBoolToObject(message, "has_maintainer", channel->is_channel_maintainer_present);

    // send to channel members
    server_msg__delivery__send_json_to_channel(root, channel->channel_id, NULL_POINTER);

    // free the message
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
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create channel deleted message
    root = server_msg__delivery__new_message("channel_delete", &message);

    // add the two ids
    cJSON_AddNumberToObject(message, "channel_id", deleted_channel_id);
    if (channel_deletor_id == UINT64_MAX)
    {
        cJSON_AddNullToObject(message, "channel_deletor_id");
    }
    else
    {
        cJSON_AddNumberToObject(message, "channel_deletor_id", channel_deletor_id);
    }

    // send to all clients
    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    // free the message
    cJSON_Delete(root);
}

/**
 * @brief broadcasts a channel's icon change to every authenticated client so their channel row
 *        updates live
 *
 * @param uint64 channel_id -> id of the channel whose icon changed
 * @param boole has_channel_icon -> whether the channel now has an icon
 * @param uint64 icon_id -> the channel's icon id (meaningful only when has_channel_icon is TRUE)
 *
 * @return void
 */
void server_msg__channel__send_icon_changed_event_to_all_clients(uint64 channel_id, boole has_channel_icon, uint64 icon_id)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create icon changed message
    root = server_msg__delivery__new_message("channel_icon_changed", &message);

    // add channel and icon
    cJSON_AddNumberToObject(message, "channel_id", (double)channel_id);
    util__json_add_owned_object(message, "has_channel_icon", cJSON_CreateBool(has_channel_icon == TRUE));
    cJSON_AddNumberToObject(message, "channel_icon_id", (double)icon_id);

    // send to all clients
    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    // free the message
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
void server_msg__channel__send_call_event_to_idle_client(client_t *receiver, client_t *caller)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create the call message
    root = server_msg__delivery__new_message("call", &message);

    // add the caller id
    cJSON_AddNumberToObject(message, "caller_client_id", caller->client_id);

    // send to the receiver
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
void server_msg__channel__send_client_going_to_idle_mode_info_to_all_clients(client_t *subject)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;

    // write the debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create idle mode message
    root = server_msg__delivery__new_message("client_going_to_idle_mode", &message);

    // add the client id
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    // send to all clients
    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    // free the message
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
void server_msg__channel__send_client_coming_back_from_idle_mode_info_to_all_clients(client_t *subject, uint64 channel_the_client_joins)
{
    cJSON *root = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    char *plaintext = NULL_POINTER;
    char *plaintext_hidden = NULL_POINTER;
    client_t *client_in_loop = NULL_POINTER;
    uint64 x = 0;

    // debug log
    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    // create the message
    root = server_msg__delivery__new_message("client_coming_back_from_idle_mode", &message);

    // add the fields
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel_the_client_joins);
    cJSON_AddNumberToObject(message, "channel_group_id", subject->channel_group_role);
    plaintext = cJSON_PrintUnformatted(root);

    // the version for the clients that may not see who is in that channel
    cJSON_ReplaceItemInObjectCaseSensitive(message, "channel_id", cJSON_CreateNumber((double)-1));
    cJSON_ReplaceItemInObjectCaseSensitive(message, "channel_group_id", cJSON_CreateNumber(0));
    plaintext_hidden = cJSON_PrintUnformatted(root);

    // send to clients
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];
        if (client_in_loop->is_existing == FALSE || client_in_loop->is_authenticated == FALSE || client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }
        server_msg__delivery__send_text_to_client(client_in_loop, channel__is_client_location_visible(client_in_loop, subject) == TRUE ? plaintext : plaintext_hidden);
    }

    // free both texts
    cJSON_free(plaintext);
    cJSON_free(plaintext_hidden);
    cJSON_Delete(root);
}
