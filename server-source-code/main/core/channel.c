// This file manages channels, their members and the client chosen to maintain each channel's keys.
#include "channel.h"
#include "channel_groups.h"
#include "permissions.h"
#include "../video_websocket.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"
#include "../msg/server/server_msg.h"
#include "../memory_manager.h"
#include "../clib/clib.h"
#include "../util.h"
#include "audio_channel.h"
#include "video_channel.h"
#include "server_logs.h"

// declarations
static boole _channel_internal__can_maintain(const client_t *client, const channel_t *channel);
static boole _channel_internal__is_maintainer_admin(const client_t *client, const channel_t *channel);

/**
 * @brief Prepares an exact-sized channel array without changing the live array.
 * @attention Caller holds clients and channels write locks until commit or cancellation. Existing channel IDs and membership allocations retain their ownership.
 * @return NULL on success; otherwise a user-facing reason. Free an uncommitted replacement.
 */
cstring channel__prepare_resize(uint64 count, channel_t **replacement)
{
    uint64 index = 0;
    uint64 capacity = g_server_settings.max_channel_count;

    *replacement = NULL_POINTER;
    if (count < 1 || count > MAX_CHANNEL_COUNT || count > SIZE_MAX / sizeof(channel_t))
    {
        return "The channel count is outside the supported allocation range.";
    }
    if (count == capacity)
    {
        return NULL_POINTER;
    }
    for (index = count; index < capacity; index++)
    {
        if (g_channel_array[index].is_existing)
        {
            return "Delete channels whose IDs are outside the new channel count before lowering it.";
        }
    }
    *replacement = (channel_t *)memorymanager__allocate(count * sizeof(channel_t), MEMALLOC_CHANNELS_ARRAY);
    if (*replacement == NULL_POINTER)
    {
        return "Could not allocate the requested channel array. The channel count was not changed.";
    }
    return NULL_POINTER;
}

/** @brief Publishes a prepared array after persistence succeeds; caller still holds both write locks. */
void channel__commit_resize(channel_t *replacement, uint64 count)
{
    channel_t *previous = NULL_POINTER;
    uint64 capacity = 0;
    uint64 retained = 0;

    if (replacement != NULL_POINTER)
    {
        previous = g_channel_array;
        capacity = g_server_settings.max_channel_count;
        retained = count < capacity ? count : capacity;

        // Policy updates during the save may have changed channel state. Copy
        // the final locked state, transferring membership ownership exactly once.
        clib__memcpy(replacement, previous, retained * sizeof(channel_t));
        g_channel_array = replacement;
        atomic_store(&g_server_settings.max_channel_count, count);
        memorymanager__free((nuint)previous);
    }
}

/**
 * @brief Checks a type conversion while the clients and channels locks are held.
 * @return NULL when allowed, otherwise a user-facing reason.
 */
cstring channel__type_change_error(const client_t *actor, const channel_t *channel, boole temporary)
{
    uint64 i = 0;

    if (actor == NULL_POINTER || channel == NULL_POINTER)
    {
        return "The client or channel is no longer available.";
    }
    if (actor->is_existing == FALSE || actor->is_authenticated == FALSE || actor->is_music_bot != FALSE || channel->is_existing == FALSE)
    {
        return "The client or channel is no longer available.";
    }
    if (temporary == channel->is_temp_channel)
    {
        return NULL_POINTER;
    }
    if (channel->is_root_channel || channel->channel_id == ROOT_CHANNEL_ID)
    {
        return "The root channel must remain permanent.";
    }
    if (channel_groups__is_allowed(actor, channel, PERMISSION_EDIT_CHANNEL_OPTIONS) == FALSE)
    {
        return "Changing channel type requires Edit channel options.";
    }
    if (!temporary)
    {
        return permissions__is_allowed(actor, PERMISSION_CREATE_CHANNEL) ? NULL_POINTER : "Making a channel permanent requires Create permanent channels.";
    }
    if (permissions__is_allowed(actor, PERMISSION_CREATE_TEMP_CHANNEL) == FALSE ||
        g_server_settings.is_permission_system_enabled == FALSE &&
        g_server_settings.is_temp_channel_creation_allowed == FALSE &&
        actor->is_admin == FALSE)
    {
        return "Creating temporary channels is not permitted.";
    }
    if (channel_groups__is_allowed(actor, channel, PERMISSION_DELETE_CHANNEL) == FALSE)
    {
        return "Making a channel temporary requires Delete channels because it will be deleted when its owner leaves.";
    }
    if (actor->is_idle || actor->channel_id != channel->channel_id)
    {
        return "Join this channel first. You will own it until you leave.";
    }
    if (actor->is_temp_admin_channel && actor->temp_channel_id != channel->channel_id)
    {
        return "You already own another temporary channel.";
    }
    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        if (g_channel_array[i].is_existing && g_channel_array[i].parent_channel_id == (int64)channel->channel_id)
        {
            return "Remove or move the child channels before making this channel temporary.";
        }
    }
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing != FALSE &&
            g_clients_array[i].is_music_bot != FALSE &&
            g_clients_array[i].channel_id == channel->channel_id)
        {
            return "Remove the music bot before making this channel temporary.";
        }
    }
    return NULL_POINTER;
}

/**
 * @brief Applies a validated conversion without recreating the channel or moving members.
 * @attention Caller holds clients and channels write locks and has checked type_change_error.
 */
void channel__set_temporary(client_t *actor, channel_t *channel, boole temporary)
{
    uint64 i = 0;
    client_t *client = NULL_POINTER;
    char identity[IDENTITY_HASH_MAX_LENGTH] = { 0 };

    if (temporary == channel->is_temp_channel)
    {
        return;
    }
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client->is_temp_admin_channel && client->temp_channel_id == channel->channel_id)
        {
            if (!temporary && g_server_settings.is_permission_system_enabled && client->is_existing && client->is_authenticated)
            {
                util__hash_password_to_base64(client->public_key, identity, sizeof(identity));
                channel_groups__set(channel, identity, CHANNEL_GROUP_ADMIN);
            }
            client->is_temp_admin_channel = FALSE;
            client->temp_channel_id = 0;
        }
    }
    channel->is_temp_channel = temporary;
    if (temporary)
    {
        actor->is_temp_admin_channel = TRUE;
        actor->temp_channel_id = channel->channel_id;
    }
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client->is_existing && client->is_authenticated && client->channel_id == channel->channel_id)
        {
            channel_groups__refresh(client);
        }
    }
}

// Names of the channel requirements in settings and messages, in the order of _channel_internal__required_power.
static const char *g_channel_required_power_names[] = { "required_talk_power", "required_message_power", "required_picture_power", "required_stream_power", "required_file_power", "required_view_power", "required_create_power" };

#define CHANNEL_REQUIRED_POWER_COUNT (sizeof(g_channel_required_power_names) / sizeof(g_channel_required_power_names[0]))

/**
 * @brief Returns the field of a channel that holds one of its requirements.
 *
 * @param channel Channel whose field is needed.
 * @param index Position of the requirement in g_channel_required_power_names.
 *
 * @return uint64* -> the requirement with that index
 */
static uint64 *_channel_internal__required_power(channel_t *channel, uint64 index)
{
    // Field by index.
    switch (index)
    {
    case 0:
        return &channel->required_talk_power;
    case 1:
        return &channel->required_message_power;
    case 2:
        return &channel->required_picture_power;
    case 3:
        return &channel->required_stream_power;
    case 4:
        return &channel->required_file_power;
    case 5:
        return &channel->required_view_power;
    default:
        return &channel->required_create_power;
    }
}

/**
 * @brief Checks channel power requirements and applies them only when all are valid.
 * @param object Request or saved channel containing optional requirements.
 * @param channel Channel to update, or NULL when only checking the values.
 */
boole channel__read_required_powers(cJSON *object, channel_t *channel)
{
    cJSON *values[CHANNEL_REQUIRED_POWER_COUNT];
    uint64 i = 0;

    // Read each power.
    for (i = 0; i < CHANNEL_REQUIRED_POWER_COUNT; i++)
    {
        values[i] = cJSON_GetObjectItemCaseSensitive(object, g_channel_required_power_names[i]);
        if (values[i] != NULL_POINTER && (cJSON_IsNumber(values[i]) == FALSE || !(values[i]->valuedouble >= 0 && values[i]->valuedouble <= PERMISSION_POWER_MAX) || values[i]->valuedouble != (double)values[i]->valueint))
        {
            return FALSE;
        }
    }

    // Store the values.
    if (channel != NULL_POINTER)
    {
        for (i = 0; i < CHANNEL_REQUIRED_POWER_COUNT; i++)
        {
            if (values[i] != NULL_POINTER)
            {
                *_channel_internal__required_power(channel, i) = (uint64)values[i]->valueint;
            }
        }
    }
    return TRUE;
}

/**
 * @brief Tells whether two channels have the same power requirements.
 *
 * @param first Channel to compare.
 * @param second Channel to compare with.
 *
 * @return boole -> TRUE when all requirements are equal
 */
boole channel__has_same_required_powers(const channel_t *first, const channel_t *second)
{
    uint64 i = 0;

    // Compare each power.
    for (i = 0; i < CHANNEL_REQUIRED_POWER_COUNT; i++)
    {
        if (*_channel_internal__required_power((channel_t *)first, i) != *_channel_internal__required_power((channel_t *)second, i))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Sets every power requirement of a channel to 0, the value of a new channel.
 *
 * @param channel Channel to change.
 *
 * @return void
 */
void channel__clear_required_powers(channel_t *channel)
{
    uint64 i = 0;

    // Clear each power.
    for (i = 0; i < CHANNEL_REQUIRED_POWER_COUNT; i++)
    {
        *_channel_internal__required_power(channel, i) = 0;
    }
}

/**
 * @brief Adds channel power requirements to saved settings or a client message.
 *
 * @param object JSON object receiving the channel requirements.
 * @param channel Channel whose requirements are copied.
 *
 * @return boole -> TRUE when every requirement was added
 */
boole channel__write_required_powers(cJSON *object, const channel_t *channel)
{
    uint64 i = 0;

    // Write each power.
    for (i = 0; i < CHANNEL_REQUIRED_POWER_COUNT; i++)
    {
        if (!cJSON_AddNumberToObject(object, g_channel_required_power_names[i], (double)*_channel_internal__required_power((channel_t *)channel, i)))
        {
            return FALSE;
        }
    }
    return TRUE;
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
    client_t *client = NULL_POINTER;
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
void channel__mark_channels_for_deletion(uint64 channel_id, uint64 *out_current_index, uint64 *out_channel_indices)
{
    uint64 parent_index = 0;
    uint64 child_index = 0;
    uint64 seen_index = 0;
    uint64 parent_id = channel_id;
    boole already_marked = FALSE;

    if (out_current_index == NULL_POINTER || out_channel_indices == NULL_POINTER || channel_id >= g_server_settings.max_channel_count || *out_current_index > g_server_settings.max_channel_count)
    {
        return;
    }

    // Iterate the bounded output as a work queue. A cycle cannot append a node twice.
    for (parent_index = 0; parent_index <= *out_current_index; parent_index++)
    {
        parent_id = parent_index == 0 ? channel_id : out_channel_indices[parent_index - 1];
        for (child_index = 0; child_index < g_server_settings.max_channel_count; child_index++)
        {
            if (!g_channel_array[child_index].is_existing || child_index == channel_id || g_channel_array[child_index].parent_channel_id != parent_id)
            {
                continue;
            }
            already_marked = FALSE;
            for (seen_index = 0; seen_index < *out_current_index; seen_index++)
            {
                if (out_channel_indices[seen_index] == child_index)
                {
                    already_marked = TRUE;
                    break;
                }
            }
            if (already_marked == FALSE && *out_current_index < g_server_settings.max_channel_count)
            {
                out_channel_indices[*out_current_index] = child_index;
                *out_current_index += 1;
            }
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
client_t *channel__find_music_bot_in_channel(uint64 channel_id)
{
    uint64 i = 0;
    client_t *client_in_loop = NULL_POINTER;
    client_t *result = NULL_POINTER;

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
 * @brief Checks whether a connected person can maintain this channel.
 */
static boole _channel_internal__can_maintain(const client_t *client, const channel_t *channel)
{
    return client != NULL_POINTER && channel != NULL_POINTER && client->is_existing && client->is_authenticated && !client->is_idle && !client->is_music_bot && client->channel_id == channel->channel_id;
}

/**
 * @brief Recognizes server admins, channel admins and the owner of this temporary channel.
 * @attention Caller holds the clients and channels write locks. Editing powers alone do not make a channel admin.
 */
static boole _channel_internal__is_maintainer_admin(const client_t *client, const channel_t *channel)
{
    if (client->is_admin || (channel->is_temp_channel && client->is_temp_admin_channel && client->temp_channel_id == channel->channel_id))
    {
        return TRUE;
    }
    return channel_groups__role(client, channel) == CHANNEL_GROUP_ADMIN;
}

/**
 * @brief Chooses randomly among available admins, or other members when no admin is available.
 * @param uint64* _out__new_index_of_maintainer -> receives the chosen maintainer's client id
 * @param uint64 channel_id -> id of the channel needing a new maintainer
 * @param uint64 index_of_client_that_left -> departing or voted-out client to exclude when requested
 * @param boole do_not_include_client_that_left_when_searching_for_new_maintainer -> TRUE to exclude the supplied client
 * @return boole -> TRUE when a replacement was found
 * @attention The caller holds the clients and channels write locks. The excluded client cannot return through either candidate group.
 */
boole channel__find_new_maintainer_for_channel(uint64 *_out__new_index_of_maintainer, uint64 channel_id, uint64 index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer)
{
    uint64 i = 0;
    client_t *client = NULL_POINTER;
    channel_t *channel = NULL_POINTER;
    boole maintainer_found = FALSE;
    boole admin_found = FALSE;
    boole is_admin = FALSE;
    uint64 *possible_new_maintainers = NULL_POINTER;
    uint64 possible_new_maintainers_count = 0;
    uint64 random_index = 0;

    if (_out__new_index_of_maintainer == NULL_POINTER || channel_id >= g_server_settings.max_channel_count)
    {
        return FALSE;
    }
    channel = &g_channel_array[channel_id];
    if (channel->is_existing == FALSE)
    {
        return FALSE;
    }
    possible_new_maintainers = (uint64 *)memorymanager__allocate(sizeof(uint64) * g_server_settings.max_client_count, MEMALLOC_FIND_MAINTAINER);
    if (possible_new_maintainers == NULL_POINTER)
    {
        return FALSE;
    }

    // maintainer will be randomly chosen
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (_channel_internal__can_maintain(client, channel) == FALSE)
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

        // Once an admin is found, only admins remain in the random selection.
        is_admin = _channel_internal__is_maintainer_admin(client, channel);
        if (admin_found && !is_admin)
        {
            continue;
        }
        if (is_admin && !admin_found)
        {
            possible_new_maintainers_count = 0;
            admin_found = TRUE;
        }

        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "channel__find_new_maintainer_for_channel client hopped on is: ", i, "\n");

        maintainer_found = TRUE;

        possible_new_maintainers[possible_new_maintainers_count] = i;
        possible_new_maintainers_count++;
    }

    if (maintainer_found == TRUE)
    {
        random_index = (uint64)rand() % possible_new_maintainers_count;
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
 * @brief Finds a vote replacement, keeping an admin maintainer unless another admin can take over.
 * @param uint64* replacement -> receives the selected client id only on success
 * @param channel_t* channel -> channel whose current maintainer is being replaced
 * @return boole -> FALSE when no eligible replacement meets the vote rule
 * @attention The caller holds the clients and channels write locks. A departure uses the normal selector instead.
 */
boole channel__find_voted_maintainer_replacement(uint64 *replacement, channel_t *channel)
{
    uint64 candidate = 0;
    client_t *maintainer = NULL_POINTER;

    if (replacement == NULL_POINTER || channel == NULL_POINTER || !channel->is_channel_maintainer_present || channel->maintainer_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }
    maintainer = &g_clients_array[channel->maintainer_id];
    if (!channel__find_new_maintainer_for_channel(&candidate, channel->channel_id, channel->maintainer_id, TRUE))
    {
        return FALSE;
    }
    if (_channel_internal__is_maintainer_admin(maintainer, channel) && !_channel_internal__is_maintainer_admin(&g_clients_array[candidate], channel))
    {
        return FALSE;
    }
    *replacement = candidate;
    return TRUE;
}

/**
 * @brief Records a new maintainer, or clears the role when nobody remains.
 * @param channel_t* channel -> channel being changed
 * @param int64 client_id -> new maintainer, or -1 to clear the role
 * @attention The caller holds the clients and channels write locks.
 */
void channel__set_maintainer(channel_t *channel, int64 client_id)
{
    channel->maintainer_id = client_id >= 0 ? client_id : 0;
    channel->is_channel_maintainer_present = client_id >= 0;
    channel->maintainer_generation++;
}

/**
 * @brief destroys a temp channel: moves any remaining members to the root channel and frees the
 *        slot, then broadcasts the deletion. used when a temp channel's owner leaves it,
 *        disconnects, or deletes it. temp channels never have child channels or music bots, so no
 *        cascade is needed.
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
    client_t *client_to_move = NULL_POINTER;

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

        server_msg__channel__send_join_message_to_all_clients(client_to_move, &g_channel_array[ROOT_CHANNEL_ID], "move");

        channel__welcome_client(client_to_move);

        server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move, ROOT_CHANNEL_ID);
    }

    // free the channel slot and tell everyone the channel is gone
    channel_groups__clear(&g_channel_array[temp_channel_id]);
    clib__null_memory(&g_channel_array[temp_channel_id], sizeof(channel_t));

    // No client clicked delete: the owner left this temporary channel.
    server_msg__channel__send_delete_message_to_all_clients(temp_channel_id, UINT64_MAX);

    // the root channel may have just gained members and have no maintainer; pick one
    if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
    {
        if (channel__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE) == TRUE)
        {
            channel__set_maintainer(&g_channel_array[ROOT_CHANNEL_ID], index_of_new_maintainer);
            server_msg__channel__send_maintainer_id_to_clients_in_same_channel(&g_channel_array[ROOT_CHANNEL_ID], g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
        }
    }
}

/**
 * @brief Moves a client, replaces its former channel maintainer, and broadcasts the join. Updates
 *        its audio peer, maintainer id and microphone state.
 *
 * @param uint64 client_id -> the client to move
 * @param uint64 destination_channel_id -> the channel to move the client into
 *
 * @attention caller must hold the clients and channels write locks
 *
 * @return void
 */
void channel__move_client_into_channel(uint64 client_id, uint64 destination_channel_id, const char *reason)
{
    client_t *client = NULL_POINTER;
    channel_t *old_channel = NULL_POINTER;
    channel_t *new_channel = NULL_POINTER;
    boole status = FALSE;

    client = &g_clients_array[client_id];
    old_channel = &g_channel_array[client->channel_id];
    new_channel = &g_channel_array[destination_channel_id];

    status = channel__replace_departed_maintainer(old_channel, client_id);

    // move the client and tell everyone (same message order as the delete -> move-to-root path)
    video_channel__process_client_leaving_channel(client, old_channel->channel_id);

    client->channel_id = destination_channel_id;
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel
    server_msg__channel__send_join_message_to_all_clients(client, new_channel, reason);

    // The moved client now sees the members of its new channel and may no longer see those of the old one.
    channel__send_view_changes_to_client(client);

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
 * @brief Collects valid client ids in a channel, excluding the specified client.
 *
 * @attention out_receiving_client_ids must hold max_client_count entries; this function does not check its capacity. Free and unauthenticated slots are skipped.
 *
 * @param int64 client_to_ignore -> index of the client to leave out, pass a value no slot can have to include everyone
 * @param uint64 channel_id -> id of the channel to collect clients from
 * @param int64* out_receiving_client_ids -> receives the matching client indexes
 *
 * @note the caller must hold the clients read lock (g_clients_array is read)
 *
 * @return uint64 -> how many client ids were written into out_receiving_client_ids
 */
uint64 channel__get_other_clients_in_channel(int64 client_to_ignore, uint64 channel_id, int64 *out_receiving_client_ids)
{
    boole status = FALSE;
    uint64 count = 0;
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        status = FALSE;

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
boole channel__check_channel_access(client_t *client, channel_t *channel, char *password, boole password_already_proven)
{
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    boole status3 = FALSE;
    boole status4 = FALSE;

    if (client->is_admin == TRUE)
    {
        return TRUE;
    }
    status0 = channel->is_client_limit_active == TRUE;
    status1 = channel->max_client_count == 0;
    status2 = FALSE;
    if (status0 == TRUE && status1 == FALSE)
    {
        status2 = channel__get_client_count_for_channel(channel->channel_id) >= channel->max_client_count;
    }
    if (status0 == TRUE && (status1 == TRUE || status2 == TRUE))
    {
        server_msg__channel__send_full_to_single_client(client, channel->channel_id);
        return FALSE;
    }
    status0 = channel->is_using_password == TRUE;
    status1 = password_already_proven == FALSE;
    status2 = FALSE;
    status3 = password == NULL_POINTER;
    status4 = FALSE;

    // Keep permission resolution and password hashing behind the same gates.
    if (status0 == TRUE && status1 == TRUE)
    {
        status2 = permissions__can_bypass_channel_password(client, channel) == FALSE;
        if (status2 == TRUE && status3 == FALSE)
        {
            status4 = util__password_matches(password, channel->password) == FALSE;
        }
    }
    if (status0 == TRUE && status1 == TRUE && status2 == TRUE && (status3 == TRUE || status4 == TRUE))
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Collects the values that decide what a client may see of channels it is not in.
 *
 * @param client_t* client -> client whose values are read, with the clients lock held
 * @param channel_view_rights_t* rights -> receives the values as they are now
 *
 * @note The values taken from the client's groups are calculated by permissions__refresh_client_powers.
 *
 * @return void
 */
static void _channel_internal__read_view_rights(const client_t *client, channel_view_rights_t *rights)
{
    *rights = client->channel_view_rights;
    rights->is_admin = client->is_admin;
    rights->channel_id = client->channel_id;
}

/**
 * @brief Decides whether a client with the given rights is told who is inside a channel.
 *
 *        A client always sees the members of the channel it is in. From outside, two rules
 *        apply. The older server setting hides the members of every channel that has a
 *        password. With groups enabled, the client's groups must allow viewing channel members
 *        and its channel view power must reach the channel's required view power.
 *
 * @param const channel_view_rights_t* rights -> the viewing client's rights
 * @param const channel_t* channel -> channel whose members are asked for
 *
 * @return boole -> TRUE when the members are shown
 */
static boole _channel_internal__are_members_visible(const channel_view_rights_t *rights, const channel_t *channel)
{
    // Own channel.
    if (rights->channel_id == channel->channel_id)
    {
        return TRUE;
    }

    // Password channel hidden.
    if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE && channel->is_using_password == TRUE)
    {
        return FALSE;
    }

    // Admin sees all.
    if (g_server_settings.is_permission_system_enabled == FALSE || rights->is_admin == TRUE)
    {
        return TRUE;
    }

    // Action and power.
    return (boole)(rights->is_viewing_members_allowed == TRUE && rights->view_power >= channel->required_view_power);
}

/**
 * @brief Decides whether a client with the given rights is told a channel's description.
 *
 * @param const channel_view_rights_t* rights -> the viewing client's rights
 * @param const channel_t* channel -> channel whose description is asked for
 *
 * @return boole -> TRUE when the description is shown
 */
static boole _channel_internal__is_description_visible(const channel_view_rights_t *rights, const channel_t *channel)
{
    if (rights->channel_id == channel->channel_id || g_server_settings.is_permission_system_enabled == FALSE || rights->is_admin == TRUE)
    {
        return TRUE;
    }
    return (boole)(rights->is_viewing_descriptions_allowed == TRUE && rights->view_power >= channel->required_view_power);
}

/**
 * @brief Tells whether a client may currently see who is inside a channel.
 *
 * @param const client_t* viewer -> client that would receive the information, with the clients lock held
 * @param const channel_t* channel -> channel asked about, with the channels lock held
 *
 * @return boole -> TRUE when the viewer may see who is in the channel
 */
boole channel__are_members_visible(const client_t *viewer, const channel_t *channel)
{
    channel_view_rights_t rights;

    // Check arguments.
    if (viewer == NULL_POINTER || channel == NULL_POINTER)
    {
        return FALSE;
    }
    _channel_internal__read_view_rights(viewer, &rights);
    return _channel_internal__are_members_visible(&rights, channel);
}

/**
 * @brief Tells whether a client may currently read a channel's description.
 *
 * @param const client_t* viewer -> client that would receive the description, with the clients lock held
 * @param const channel_t* channel -> channel asked about, with the channels lock held
 *
 * @return boole -> TRUE when the viewer may read the description
 */
boole channel__is_description_visible(const client_t *viewer, const channel_t *channel)
{
    channel_view_rights_t rights;

    // Check arguments.
    if (viewer == NULL_POINTER || channel == NULL_POINTER)
    {
        return FALSE;
    }
    _channel_internal__read_view_rights(viewer, &rights);
    return _channel_internal__is_description_visible(&rights, channel);
}

/**
 * @brief Tells whether a client may see which channel another client is in.
 *
 * @param const client_t* viewer -> client that would receive the information, with the clients lock held
 * @param const client_t* subject -> client whose channel is asked about
 *
 * @attention The caller holds the channels lock. A subject in idle mode is in no channel, so nothing is hidden.
 *
 * @return boole -> TRUE when the viewer may see which channel the subject is in
 */
boole channel__is_client_location_visible(const client_t *viewer, const client_t *subject)
{
    if (viewer == NULL_POINTER || subject == NULL_POINTER)
    {
        return FALSE;
    }
    if (subject->channel_id >= g_server_settings.max_channel_count || g_channel_array[subject->channel_id].is_existing == FALSE)
    {
        return TRUE;
    }
    return channel__are_members_visible(viewer, &g_channel_array[subject->channel_id]);
}

/**
 * @brief Records that a client has just received the channel list and the client list, so later
 *        changes are compared with the rights used for those lists.
 *
 * @param client_t* client -> client that received the lists, with the clients write lock held
 *
 * @return void
 */
void channel__remember_sent_view(client_t *client)
{
    _channel_internal__read_view_rights(client, &client->sent_channel_view_rights);
}

/**
 * @brief Sends one client the current place of every member of a channel, or tells it that
 *        their place is hidden from now on.
 *
 * @param client_t* receiver -> client that is told
 * @param channel_t* channel -> channel whose members are sent
 * @param boole is_visible -> TRUE to show the members in the channel; FALSE to hide where they are
 *
 * @return void
 */
static void _channel_internal__send_members(client_t *receiver, channel_t *channel, boole is_visible)
{
    uint64 i = 0;
    client_t *member = NULL_POINTER;

    // Send each member.
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        member = &g_clients_array[i];
        if (member != receiver && member->is_existing == TRUE && member->is_authenticated == TRUE && member->channel_id == channel->channel_id)
        {
            server_msg__channel__send_join_message_to_single_client(receiver, member, channel, is_visible);
        }
    }
}

/**
 * @brief Tells a client about every channel whose members or description it may see now but
 *        could not see before, and the other way round.
 *
 *        The server remembers the rights it last used when it told the client about the
 *        channels (sent_channel_view_rights). This function compares them with the rights the
 *        client has now. They differ after the client changed channel, after its groups or
 *        their permissions changed, and after it gained or lost admin access. For every channel
 *        where the answer changed, the members and the description are sent again.
 *
 * @param client_t* client -> client to update
 *
 * @attention The caller holds the clients write lock and the channels lock.
 *
 * @return void
 */
void channel__send_view_changes_to_client(client_t *client)
{
    channel_view_rights_t current;
    channel_view_rights_t previous;
    channel_t *channel = NULL_POINTER;
    uint64 i = 0;
    boole is_visible = FALSE;

    // Skip invalid client.
    if (client == NULL_POINTER || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
    {
        return;
    }

    // Compare the rights.
    previous = client->sent_channel_view_rights;
    _channel_internal__read_view_rights(client, &current);
    if (previous.is_admin == current.is_admin &&
        previous.is_viewing_members_allowed == current.is_viewing_members_allowed &&
        previous.is_viewing_descriptions_allowed == current.is_viewing_descriptions_allowed &&
        previous.view_power == current.view_power &&
        previous.channel_id == current.channel_id)
    {
        return;
    }

    // Store new rights.
    client->sent_channel_view_rights = current;

    // Check every channel.
    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        channel = &g_channel_array[i];
        if (channel->is_existing == FALSE)
        {
            continue;
        }

        // Members.
        is_visible = _channel_internal__are_members_visible(&current, channel);
        if (is_visible != _channel_internal__are_members_visible(&previous, channel))
        {
            _channel_internal__send_members(client, channel, is_visible);
        }

        // Description.
        is_visible = _channel_internal__is_description_visible(&current, channel);
        if (is_visible != _channel_internal__is_description_visible(&previous, channel))
        {
            server_msg__channel__send_description_to_single_client(client, channel, is_visible);
        }
    }
}

/**
 * @brief Updates every connected client whose view of the channels changed.
 *
 * @attention The caller holds only the clients write lock.
 *
 * @return void
 */
void channel__apply_permission_changes(void)
{
    uint64 i = 0;

    // Update every client.
    clib__read_lock(&g_channels_global_rwlock_guard);
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        channel__send_view_changes_to_client(&g_clients_array[i]);
    }
    clib__unlock(&g_channels_global_rwlock_guard);
}

/**
 * @brief Updates the clients after a channel edit changed who may see its members or its
 *        description: the required view power or the password changed.
 *
 * @param const channel_t* previous -> copy of the channel as it was before the edit
 * @param channel_t* channel -> the channel after the edit
 *
 * @attention The caller holds the clients lock and the channels lock. A changed description is
 *            sent by the edit announcement itself; this function covers the clients for which
 *            the description became visible or hidden.
 *
 * @return void
 */
void channel__send_view_changes_for_channel(const channel_t *previous, channel_t *channel)
{
    channel_view_rights_t rights;
    client_t *client = NULL_POINTER;
    uint64 i = 0;
    boole is_visible = FALSE;

    // Update every client.
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
        {
            continue;
        }

        // Read the rights.
        _channel_internal__read_view_rights(client, &rights);

        // Members.
        is_visible = _channel_internal__are_members_visible(&rights, channel);
        if (is_visible != _channel_internal__are_members_visible(&rights, previous))
        {
            _channel_internal__send_members(client, channel, is_visible);
        }

        // Description.
        is_visible = _channel_internal__is_description_visible(&rights, channel);
        if (is_visible != _channel_internal__is_description_visible(&rights, previous))
        {
            server_msg__channel__send_description_to_single_client(client, channel, is_visible);
        }
    }
}

/**
 * @brief Replaces a departing maintainer with another member, or clears the role.
 * @param channel_t* channel -> channel the client is leaving
 * @param uint64 client_id -> departing client, excluded from the choice
 * @return boole -> TRUE when a replacement was chosen and needs to be announced
 * @attention The caller holds the clients and channels write locks. Announce the departure before announcing the replacement, so clients update their member lists first.
 */
boole channel__replace_departed_maintainer(channel_t *channel, uint64 client_id)
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
 * @brief Welcomes a member, preferring an arriving admin over an ordinary maintainer.
 * @param client_t* client -> authenticated client already placed in the channel
 * @attention The caller holds the clients and channels write locks and has already announced the client's arrival and any previously hidden members.
 */
void channel__welcome_client(client_t *client)
{
    channel_t *channel = NULL_POINTER;
    client_t *maintainer = NULL_POINTER;
    uint64 replacement = 0;
    boole choose_maintainer = FALSE;

    if (client == NULL_POINTER || client->channel_id >= g_server_settings.max_channel_count)
    {
        return;
    }
    channel = &g_channel_array[client->channel_id];
    if (channel->is_existing == FALSE || _channel_internal__can_maintain(client, channel) == FALSE)
    {
        return;
    }
    if (channel->is_channel_maintainer_present && channel->maintainer_id < g_server_settings.max_client_count)
    {
        maintainer = &g_clients_array[channel->maintainer_id];
    }
    choose_maintainer = !_channel_internal__can_maintain(maintainer, channel);
    if (!choose_maintainer && _channel_internal__is_maintainer_admin(client, channel))
    {
        choose_maintainer = !_channel_internal__is_maintainer_admin(maintainer, channel);
    }
    if (choose_maintainer && channel__find_new_maintainer_for_channel(&replacement, channel->channel_id, 0, FALSE))
    {
        // Announce the new generation to everyone so the new maintainer creates fresh keys.
        channel__set_maintainer(channel, (int64)replacement);
        server_msg__channel__send_maintainer_id_to_clients_in_same_channel(channel, channel->maintainer_id);
        return;
    }
    server_msg__channel__send_maintainer_id_to_single_client(client, channel, channel->maintainer_id);
}
