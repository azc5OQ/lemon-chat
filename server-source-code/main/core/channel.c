// This file manages channels, their members and the client chosen to maintain each channel's keys.
#include "channel.h"
#include "../video_websocket.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"
#include "../msg/server/server_msg.h"
#include "../memory_manager.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"
#include "../util.h"
#include "audio_channel.h"
#include "video_channel.h"
#include "server_logs.h"

// declarations

/**
 * @brief Checks channel power requirements and applies them only when all are valid.
 * @param object Request or saved channel containing optional requirements.
 * @param channel Channel to update, or NULL when only checking the values.
 */
boole channel__read_required_powers(cJSON* object, channel_t* channel)
{
    const char* names[] =
    {
        "required_talk_power", "required_message_power", "required_picture_power", "required_stream_power"
    };
    cJSON* values[4];
    uint i = 0;

    for (i = 0; i < 4; i++)
    {
        values[i] = cJSON_GetObjectItemCaseSensitive(object, names[i]);
        if (values[i] != NULL_POINTER && (cJSON_IsNumber(values[i]) == FALSE
            || !(values[i]->valuedouble >= 0 && values[i]->valuedouble <= PERMISSION_POWER_MAX)
            || values[i]->valuedouble != (double)values[i]->valueint))
        {
            return FALSE;
        }
    }
    if (channel != NULL_POINTER)
    {
        if (values[0] != NULL_POINTER)
        {
            channel->required_talk_power = (uint)values[0]->valueint;
        }
        if (values[1] != NULL_POINTER)
        {
            channel->required_message_power = (uint)values[1]->valueint;
        }
        if (values[2] != NULL_POINTER)
        {
            channel->required_picture_power = (uint)values[2]->valueint;
        }
        if (values[3] != NULL_POINTER)
        {
            channel->required_stream_power = (uint)values[3]->valueint;
        }
    }
    return TRUE;
}

/**
 * @brief Adds channel power requirements to saved settings or a client message.
 * @param object JSON object receiving the channel requirements.
 * @param channel Channel whose requirements are copied.
 */
void channel__write_required_powers(cJSON* object, const channel_t* channel)
{
    cJSON_AddNumberToObject(object, "required_talk_power", channel->required_talk_power);
    cJSON_AddNumberToObject(object, "required_message_power", channel->required_message_power);
    cJSON_AddNumberToObject(object, "required_picture_power", channel->required_picture_power);
    cJSON_AddNumberToObject(object, "required_stream_power", channel->required_stream_power);
}

/**
 * @brief counts the authenticated, non-music-bot clients in the given channel
 *
 * @param uint64 channel_id -> id of the channel
 *
 * @return uint64 number of clients in the channel
 *
 */
uint64 channel__get_client_count_for_channel(uint64 channel_id)
{
    uint64 x = 0;
    client_t* client = NULL_POINTER;
    uint64 result = 0;

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client = &g_clients_array[x];

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

        if (client->channel_id == channel_id)
        {
            result = result + 1;
        }
    }

    return result;
}

/**
 * @brief recursively marks the child channels of the given channel for deletion
 *
 * @param uint64 channel_id -> id of the channel to start marking from
 * @param uint64* out_current_index -> in/out count of marked channels, must not be NULL
 * @param uint64* out_channel_indices -> array receiving the marked channel ids, must not be NULL
 *
 * @return void
 */
void channel__mark_channels_for_deletion(uint64 channel_id, uint64* out_current_index, uint64* out_channel_indices)
{
    uint64 i = 0;

    if (out_current_index == NULL_POINTER || out_channel_indices == NULL_POINTER)
    {
        return;
    }

    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        if (channel_id == g_channel_array[i].parent_channel_id)
        {
            out_channel_indices[*out_current_index] = i;
            *out_current_index += 1;
            DBG_CLIENT_MESSAGE log_info("%s %s %s", "mark_channels_for_deletion need to remove channel ", g_channel_array[i].name, " \n");
            channel__mark_channels_for_deletion(g_channel_array[i].channel_id, out_current_index, out_channel_indices);
        }
    }
}

/**
 * @brief finds the music bot client in the given channel, if one exists
 *
 * @param uint64 channel_id -> id of the channel to search
 *
 * @return client_t* the music bot client, or NULL_POINTER if the channel has none
 */
client_t* channel__find_music_bot_in_channel(uint64 channel_id)
{
    uint64 i = 0;
    client_t* client_in_loop = NULL_POINTER;

    client_t* result = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
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

        if (client_in_loop->is_music_bot == FALSE)
        {
            continue;
        }

        DBG_DLLMAIN log_info("%s %s %s", "channel__find_music_bot_in_channel client ", client_in_loop->username, " is the music bot \n");

        result = client_in_loop;
    }

    return result;
}

/**
 * @brief Randomly chooses an authenticated channel member to maintain its keys.
 *
 * @param uint64* _out__new_index_of_maintainer -> receives the chosen maintainer's client id
 * @param uint64 channel_id -> id of the channel needing a new maintainer
 * @param uint64 index_of_client_that_left -> id of the client that just left
 * @param boole do_not_include_client_that_left_when_searching_for_new_maintainer -> TRUE to skip the client that left
 *
 * @return boole -> TRUE if a maintainer was found and written to _out__new_index_of_maintainer, FALSE if not
 * @attention The caller holds the clients and channels write locks.
 */
boole channel__find_new_maintainer_for_channel(uint64* _out__new_index_of_maintainer, uint64 channel_id, uint64 index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;
    boole maintainer_found = FALSE;
    uint64* possible_new_maintainers = 0;
    uint64 possible_new_maintainers_count = 0;
    int random_index = 0;

    possible_new_maintainers = (uint64*)memorymanager__allocate(sizeof(uint64) * g_server_settings.max_client_count, MEMALLOC_FIND_MAINTAINER);
    // maintainer will be randomly chosen
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

        if (client->channel_id != channel_id)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        if (do_not_include_client_that_left_when_searching_for_new_maintainer == TRUE)
        {
            if (i == index_of_client_that_left)
            {
                continue;
            }
        }

        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "channel__find_new_maintainer_for_channel client hopped on is: ", i, "\n");

        maintainer_found = TRUE;

        possible_new_maintainers[possible_new_maintainers_count] = i;
        possible_new_maintainers_count++;
    }

    if (maintainer_found == TRUE)
    {
        random_index = (int)(rand() % (possible_new_maintainers_count));
        *_out__new_index_of_maintainer = possible_new_maintainers[random_index];
        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "channel__find_new_maintainer_for_channel random chosen maintainer index", *_out__new_index_of_maintainer, "\n");
    }
    else
    {
        DBG_CLIENT_DISCONNECT log_info("%s ", "channel__find_new_maintainer_for_channel maintainer not found \n");
    }

    memorymanager__free((nuint)possible_new_maintainers);

    return maintainer_found;
}

/**
 * @brief Records a new maintainer, or clears the role when nobody remains.
 * @param channel_t* channel -> channel being changed
 * @param int64 client_id -> new maintainer, or -1 to clear the role
 * @attention The caller holds the clients and channels write locks.
 */
void channel__set_maintainer(channel_t* channel, int64 client_id)
{
    channel->maintainer_id = client_id >= 0 ? client_id : 0;
    channel->is_channel_maintainer_present = client_id >= 0;
    channel->maintainer_generation++;
}

/**
 * @brief destroys a temp channel: moves any remaining members to the root channel and frees the slot,
 *        then broadcasts the deletion. used when a temp channel's owner leaves it, disconnects, or
 *        deletes it. temp channels never have child channels or music bots, so no cascade is needed.
 *
 * @param uint64 temp_channel_id -> id of the temp channel to destroy
 *
 * @attention caller must hold the clients and channels write locks
 *
 * @return void
 */
void channel__destroy_temp_channel(uint64 temp_channel_id)
{
    uint64 i = 0;
    uint64 index_of_new_maintainer = 0;
    boole status = FALSE;
    client_t* client_to_move = NULL_POINTER;

    if (temp_channel_id >= g_server_settings.max_channel_count || g_channel_array[temp_channel_id].is_existing == FALSE || g_channel_array[temp_channel_id].is_temp_channel == FALSE)
    {
        return;
    }

    // move any remaining members of the temp channel to the root channel
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_to_move = &g_clients_array[i];

        if (client_to_move->is_existing == FALSE)
        {
            continue;
        }
        if (client_to_move->is_authenticated == FALSE)
        {
            continue;
        }
        if (client_to_move->channel_id != temp_channel_id)
        {
            continue;
        }

        video_channel__process_client_leaving_channel(client_to_move, temp_channel_id);

        client_to_move->channel_id = ROOT_CHANNEL_ID;
        client_to_move->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

        // keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this client
        // on the channel-mismatch check after the move to root
        audio_channel__process_client_channel_join(client_to_move);
        video_channel__process_client_joined_channel(client_to_move);

        server_msg__channel__send_join_message_to_all_clients(client_to_move, &g_channel_array[ROOT_CHANNEL_ID]);

        if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == TRUE)
        {
            server_msg__channel__send_maintainer_id_to_single_client(client_to_move, &g_channel_array[ROOT_CHANNEL_ID], g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
        }

        server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move, ROOT_CHANNEL_ID);
    }

    // free the channel slot and tell everyone the channel is gone
    clib__null_memory(&g_channel_array[temp_channel_id], sizeof(channel_t));
    // No client clicked delete: the owner left this temporary channel.
    server_msg__channel__send_delete_message_to_all_clients(temp_channel_id, UINT64_MAX);

    // the root channel may have just gained members and have no maintainer; pick one
    if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
    {
        status = channel__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE);
        if (status == TRUE)
        {
            channel__set_maintainer(&g_channel_array[ROOT_CHANNEL_ID], index_of_new_maintainer);
            server_msg__channel__send_maintainer_id_to_clients_in_same_channel(&g_channel_array[ROOT_CHANNEL_ID], g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
        }
    }
}

/**
 * @brief moves a client into a channel server-side and tells everyone, the same way the delete path moves
 *        clients to the root channel: pass the maintainer role of the channel being left to another client, set the new
 *        channel id, broadcast the channel join, sync the audio peer, then send the new maintainer id and
 *        the active microphone usage to the moved client
 *
 * @param uint64 client_id -> the client to move
 * @param uint64 destination_channel_id -> the channel to move the client into
 *
 * @attention caller must hold the clients and channels write locks
 *
 * @return void
 */
void channel__move_client_into_channel(uint64 client_id, uint64 destination_channel_id)
{
    client_t* client = NULL_POINTER;
    channel_t* old_channel = NULL_POINTER;
    channel_t* new_channel = NULL_POINTER;
    boole status = FALSE;

    client = &g_clients_array[client_id];
    old_channel = &g_channel_array[client->channel_id];
    new_channel = &g_channel_array[destination_channel_id];

    status = channel__replace_departed_maintainer(old_channel, client_id);

    // move the client and tell everyone (same message order as the delete -> move-to-root path)
    video_channel__process_client_leaving_channel(client, old_channel->channel_id);

    client->channel_id = destination_channel_id;
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel
    server_msg__channel__send_join_message_to_all_clients(client, new_channel);
    // The new maintainer must see who left before it prepares the channel keys.
    if (status == TRUE)
    {
        server_msg__channel__send_maintainer_id_to_clients_in_same_channel(old_channel, old_channel->maintainer_id);
    }
    audio_channel__process_client_channel_join(client);
    video_channel__process_client_joined_channel(client);

    channel__welcome_client(client);
    server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client, destination_channel_id);
}

/**
 * @brief collects the ids of every valid client sitting in channel_id, except one, into
 *        out_receiving_client_ids.
 *
 *        used to build the recipient list when something has to be broadcast to a channel without
 *        echoing it back to the client that caused it. validity is base__is_client_valid, so
 *        free/unauthenticated slots are skipped. out_receiving_client_ids must have room for
 *        max_client_count entries - the function does no bounds checking of its own.
 *
 * @param int client_to_ignore -> index of the client to leave out, pass a value no slot can have to include everyone
 * @param uint64 channel_id -> id of the channel to collect clients from
 * @param int64* out_receiving_client_ids -> receives the matching client indexes
 *
 * @note the caller must hold the clients read lock (g_clients_array is read)
 *
 * @return uint64 -> how many client ids were written into out_receiving_client_ids
 */
uint64 channel__get_other_clients_in_channel(int client_to_ignore, uint64 channel_id, int64* out_receiving_client_ids)
{
    uint64 count = 0;
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        boole status = FALSE;

        status = base__is_client_valid(i);

        if (status == TRUE && i != client_to_ignore && channel_id == g_clients_array[i].channel_id)
        {
            out_receiving_client_ids[count] = i;
            count++;
        }
    }

    return count;
}

/**
 * @brief Checks channel capacity and password access, sending a refusal when either check fails.
 * @param client_t* client -> client attempting to enter; admins bypass both checks
 * @param channel_t* channel -> destination channel
 * @param char* password -> supplied password, or NULL when none was supplied
 * @param boole password_already_proven -> skips only the password check for a returning client
 * @return boole -> TRUE when entry is allowed; FALSE when refused
 * @attention The caller holds the clients and channels locks.
 */
boole channel__check_channel_access(client_t* client, channel_t* channel, char* password, boole password_already_proven)
{
    if (client->is_admin == TRUE)
    {
        return TRUE;
    }
    if (channel->is_client_limit_active == TRUE && (channel->max_client_count == 0 || channel__get_client_count_for_channel(channel->channel_id) >= channel->max_client_count))
    {
        server_msg__channel__send_full_to_single_client(client, channel->channel_id);
        return FALSE;
    }
    if (channel->is_using_password == TRUE && password_already_proven == FALSE && (password == NULL_POINTER || util__password_matches(password, channel->password) == FALSE))
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        return FALSE;
    }
    return TRUE;
}


/**
 * @brief Shows the members who were hidden while this client was outside the channel.
 * @param client_t* client -> client that has just joined or returned from idle
 * @param channel_t* channel -> channel the client entered
 * @attention The caller holds the clients and channels locks.
 */
void channel__send_hidden_members(client_t* client, channel_t* channel)
{
    uint64 i = 0;
    client_t* member = NULL_POINTER;

    if (g_server_settings.is_hide_clients_in_password_protected_channels_active == FALSE || channel->is_using_password == FALSE)
    {
        return;
    }
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        member = &g_clients_array[i];
        if (member->is_existing == TRUE && member->is_authenticated == TRUE && member->is_music_bot == FALSE && member->channel_id == channel->channel_id)
        {
            server_msg__channel__send_join_message_to_single_client(client, member, channel);
        }
    }
}


/**
 * @brief Replaces a departing maintainer with another member, or clears the role.
 * @param channel_t* channel -> channel the client is leaving
 * @param uint64 client_id -> departing client, excluded from the choice
 * @return boole -> TRUE when a replacement was chosen and needs to be announced
 * @attention The caller holds the clients and channels write locks. Announce the
 * departure before announcing the replacement, so clients update their member lists first.
 */
boole channel__replace_departed_maintainer(channel_t* channel, uint64 client_id)
{
    uint64 replacement = 0;
    boole found = FALSE;

    if (channel->is_existing == FALSE || channel->is_channel_maintainer_present == FALSE || channel->maintainer_id != client_id)
    {
        return FALSE;
    }

    found = channel__find_new_maintainer_for_channel(&replacement, channel->channel_id, client_id, TRUE);
    channel__set_maintainer(channel, found ? (int64)replacement : -1);
    return found;
}

/**
 * @brief Tells an arriving client who maintains the channel, choosing them if they are alone.
 * @param client_t* client -> authenticated client already placed in the channel
 * @attention The caller holds the clients and channels write locks and has already
 * announced the client's arrival and any previously hidden members.
 */
void channel__welcome_client(client_t* client)
{
    channel_t* channel = &g_channel_array[client->channel_id];

    if (channel__get_client_count_for_channel(channel->channel_id) == 1)
    {
        channel__set_maintainer(channel, client->client_id);
    }
    server_msg__channel__send_maintainer_id_to_single_client(client, channel, channel->maintainer_id);
}
