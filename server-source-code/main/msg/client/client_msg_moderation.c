#include "../../clib/clib.h"
// This file checks requests to moderate clients and manage channel assignments and bans.
#include "client_msg.h"
#include "../server/server_msg.h"
#include "../../base.h"
#include "../../util.h"
#include "../../core/channel.h"
#include "../../core/channel_groups.h"
#include "../../core/permissions.h"
#include "../../core/server_settings.h"
#include "../../core/audio_channel.h"
#include "../../core/video_channel.h"
#include "../../../third-party/eteran-cvector/cvector.h"
#include <string.h>

static boole _client_msg_moderation_internal__read_index(cJSON *object, cstring name, uint64 limit, uint64 *result);
static boole _client_msg_moderation_internal__save(void);

/**
 * @brief Creates, edits or deletes a channel group without granting server rights.
 * @param json_root Complete group definition and requested action.
 * @param sender_client_id Authenticated client making the change.
 */
void client_msg__moderation__process_channel_group(cJSON *json_root, uint64 sender_client_id)
{
    boole status0 = FALSE;
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON *action = cJSON_GetObjectItemCaseSensitive(message, "action");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");
    cJSON *icon = cJSON_GetObjectItemCaseSensitive(message, "icon_id");
    channel_group_t proposed = { 0 };
    channel_group_t previous;
    client_t *actor = NULL_POINTER;
    client_t *client = NULL_POINTER;
    uint64 id = 0;
    uint64 i = 0;
    uint64 j = 0;
    boole deleting = FALSE;
    boole saved = FALSE;
    permission_t permission = PERMISSION_CREATE_CHANNEL_GROUPS;

    // check the action field
    if (!cJSON_IsString(action))
    {
        return;
    }

    // choose the permission
    deleting = !clib__strcmp(action->valuestring, "delete");
    permission = deleting ? PERMISSION_DELETE_CHANNEL_GROUPS : !clib__strcmp(action->valuestring, "create") ? PERMISSION_CREATE_CHANNEL_GROUPS : PERMISSION_EDIT_CHANNEL_GROUPS;

    // read the proposed group
    if (!deleting)
    {
        // check name, icon, permissions
        status0 = (((((!clib__strcmp(action->valuestring, "create") || !clib__strcmp(action->valuestring, "update")) == FALSE) || (!cJSON_IsString(name))) || (!name->valuestring[0])) || (clib__strlen(name->valuestring) >= sizeof(proposed.name))) || (!cJSON_IsNumber(icon));
        if (status0 == FALSE)
        {
            status0 = (icon->valuedouble >= -2) && (icon->valuedouble < MAX_ICONS);
            status0 = status0 == FALSE;
        }
        if (status0 == FALSE)
        {
            status0 = icon->valuedouble != icon->valueint;
        }
        if (status0 == FALSE)
        {
            status0 = !channel_groups__read_permissions(cJSON_GetObjectItemCaseSensitive(message, "permissions"), &proposed.permissions);
        }

        // report invalid settings
        if (status0 == TRUE)
        {
            clib__read_lock(&g_clients_global_rwlock_guard);
            if (base__is_current_client_request(sender_client_id) == FALSE)
            {
                clib__unlock(&g_clients_global_rwlock_guard);
                return;
            }
            server_msg__moderation__send_result(&g_clients_array[sender_client_id], "Invalid channel group settings.", FALSE);
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }

        // copy name and icon
        proposed.is_existing = TRUE;
        proposed.icon_id = icon->valueint;
        clib__string_copy(proposed.name, sizeof(proposed.name), name->valuestring);
    }

    // check the group id
    status0 = ((clib__strcmp(action->valuestring, "create")) != FALSE) && (((!_client_msg_moderation_internal__read_index(message, "group_id", CHANNEL_GROUP_LIMIT, &id)) || (id == 0)) || (((deleting) != FALSE) && (id == CHANNEL_GROUP_ADMIN)));
    if (status0 == TRUE)
    {
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // lock the channels
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check the permission
    actor = &g_clients_array[sender_client_id];
    if (!base__is_client_valid(sender_client_id) || !g_server_settings.is_permission_system_enabled || !permissions__is_allowed(actor, permission))
    {
        server_msg__administration__send_permission_denied_to_single_client(actor, g_server_settings.is_permission_system_enabled ? permissions__name(permission) : NULL_POINTER);
        goto end;
    }

    // find the group slot
    if (!clib__strcmp(action->valuestring, "create"))
    {
        for (id = CHANNEL_GROUP_ADMIN + 1; id < CHANNEL_GROUP_LIMIT; id++)
        {
            if (!g_channel_groups[id].is_existing)
            {
                break;
            }
        }
        if (id == CHANNEL_GROUP_LIMIT)
        {
            goto end;
        }
    }
    else if (!g_channel_groups[id].is_existing)
    {
        goto end;
    }

    // check the edit rights
    if (channel_groups__can_edit_definition(actor, permission, g_channel_groups[id].is_existing ? &g_channel_groups[id] : NULL_POINTER, deleting ? NULL_POINTER : &proposed) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
        goto end;
    }

    // icon must exist
    clib__read_lock(&g_icons_global_rwlock_guard);
    if (!deleting && proposed.icon_id >= 0 && !g_icons_array[proposed.icon_id].is_existing)
    {
        clib__unlock(&g_icons_global_rwlock_guard);
        goto end;
    }

    // save the group change
    clib__read_lock(&g_groups_global_rwlock_guard);
    clib__read_lock(&g_bans_global_rwlock_guard);
    previous = g_channel_groups[id];
    g_channel_groups[id] = proposed;
    saved = settings__save_server_settings_to_file();
    if (!saved)
    {
        g_channel_groups[id] = previous;
    }
    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);

    // update channels and clients
    if (saved)
    {
        // replace the deleted role
        if (deleting)
        {
            for (i = 0; i < g_server_settings.max_channel_count; i++)
            {
                for (j = 0; j < cvector_size(g_channel_array[i].group_members); j++)
                {
                    if (g_channel_array[i].group_members[j].role == id)
                    {
                        g_channel_array[i].group_members[j].role = CHANNEL_GROUP_MEMBER;
                    }
                }
            }
        }

        // update the connected clients
        for (i = 0; i < g_server_settings.max_client_count; i++)
        {
            client = &g_clients_array[i];
            if (base__is_client_valid(i) && !client->is_music_bot)
            {
                channel_groups__refresh(client);
                if (client->channel_id < g_server_settings.max_channel_count && !client->is_idle)
                {
                    server_msg__moderation__send_channel_access(client, &g_channel_array[client->channel_id]);
                    server_msg__moderation__send_channel_group_changed(client);
                }
            }
        }
    }
end:
    // send the result
    server_msg__moderation__send_result(actor, saved ? "Channel group saved." : "The channel group was not saved.", saved);

    // unlock the channels
    clib__unlock(&g_channels_global_rwlock_guard);

    // apply permission changes
    if (saved)
    {
        server_msg__administration__send_permission_updates_to_all_clients();
        audio_channel__apply_permission_changes();
        video_channel__apply_permission_changes();
        channel__apply_permission_changes();
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Persists emoji changes without accepting other server settings.
 * @param json_root Request without settings fields.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_save_emojis(cJSON *json_root, uint64 sender_client_id)
{
    client_t *actor = NULL_POINTER;
    boole saved = FALSE;

    // JSON is not used
    (void)json_root;

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check permission, then save
    actor = &g_clients_array[sender_client_id];
    if (permissions__is_allowed(actor, PERMISSION_UPLOAD_EMOJIS) || permissions__is_allowed(actor, PERMISSION_DELETE_EMOJIS))
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        saved = _client_msg_moderation_internal__save();
        clib__unlock(&g_channels_global_rwlock_guard);
        server_msg__server_settings__send_saved(actor, saved);
    }
    else
    {
        server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Applies a permitted playback control to a music bot.
 * @param json_root Request containing bot_id and play, pause or next.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_music_control(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON *action = cJSON_GetObjectItemCaseSensitive(message, "action");
    uint64 bot_id = 0;
    client_t *actor = NULL_POINTER;
    client_t *bot = NULL_POINTER;

    // check the request fields
    if (!cJSON_IsString(action) || !_client_msg_moderation_internal__read_index(message, "bot_id", g_server_settings.max_client_count, &bot_id))
    {
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // find actor and bot
    actor = &g_clients_array[sender_client_id];
    bot = &g_clients_array[bot_id];

    // check the playback permission
    if (!permissions__is_allowed(actor, PERMISSION_CONTROL_MUSIC_PLAYBACK))
    {
        server_msg__administration__send_permission_denied_to_single_client(actor, permissions__name(PERMISSION_CONTROL_MUSIC_PLAYBACK));
    }
    else if (base__is_client_valid_musicbot(bot_id))
    {
        // apply play, pause, next
        if (!clib__strcmp(action->valuestring, "play"))
        {
            atomic_store(&bot->music_bot_client_extension.is_playback_paused, FALSE);
        }
        else if (!clib__strcmp(action->valuestring, "pause"))
        {
            atomic_store(&bot->music_bot_client_extension.is_playback_paused, TRUE);
        }
        else if (!clib__strcmp(action->valuestring, "next"))
        {
            atomic_store(&bot->music_bot_client_extension.is_skip_requested, TRUE);
        }

        // send the song list
        server_msg__musicbot__send_song_list_to_single_client(actor, bot);
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Reads an integer before it is used as an array index.
 * @param object Request fields.
 * @param name Field to read.
 * @param limit Exclusive upper bound.
 * @param result Validated index.
 */
static boole _client_msg_moderation_internal__read_index(cJSON *object, cstring name, uint64 limit, uint64 *result)
{
    boole status0 = FALSE;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);

    // value must be number
    status0 = !cJSON_IsNumber(value);

    // check the range
    if (status0 == FALSE)
    {
        status0 = (value->valuedouble >= 0) && (value->valuedouble < limit);
        status0 = status0 == FALSE;
    }

    // check for whole number
    if (status0 == FALSE)
    {
        status0 = value->valuedouble != (double)(uint64)value->valuedouble;
    }

    // refuse a wrong value
    if (status0 == TRUE)
    {
        return FALSE;
    }

    // return the index
    *result = (uint64)value->valuedouble;
    return TRUE;
}

/**
 * @brief Saves a channel assignment while the clients and channels locks are held.
 */
static boole _client_msg_moderation_internal__save(void)
{
    boole saved = FALSE;

    // save the settings file
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_groups_global_rwlock_guard);
    clib__read_lock(&g_bans_global_rwlock_guard);
    saved = settings__save_server_settings_to_file();
    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    return saved;
}

/**
 * @brief Returns the sender's current rights for a channel.
 * @param json_root Request containing channel_id.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_channel_access(cJSON *json_root, uint64 sender_client_id)
{
    uint64 target_id = 0;
    cJSON *identity = NULL_POINTER;
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    uint64 channel_id = 0;

    // check the channel id
    if (_client_msg_moderation_internal__read_index(message, "channel_id", g_server_settings.max_channel_count, &channel_id) == FALSE)
    {
        return;
    }

    // lock the clients
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // lock the channels
    clib__read_lock(&g_channels_global_rwlock_guard);

    // send the channel rights
    if (base__is_client_valid(sender_client_id) && channel_id < g_server_settings.max_channel_count && g_channel_array[channel_id].is_existing)
    {
        server_msg__moderation__send_channel_access(&g_clients_array[sender_client_id], &g_channel_array[channel_id]);

        // send the target rights
        target_id = 0;
        identity = cJSON_GetObjectItemCaseSensitive(message, "identity");
        if ((_client_msg_moderation_internal__read_index(message, "client_id", g_server_settings.max_client_count, &target_id)) != FALSE &&
            (cJSON_IsString(identity)) != FALSE &&
            (base__is_client_valid(target_id)) != FALSE &&
            g_clients_array[target_id].channel_id == channel_id &&
            // without this check the answer would tell a client, who may not see the members of the channel, that the target is in it
            channel__is_client_location_visible(&g_clients_array[sender_client_id], &g_clients_array[target_id]) == TRUE &&
            !clib__strcmp(g_clients_array[target_id].public_key, identity->valuestring))
        {
            server_msg__moderation__send_client_access(&g_clients_array[sender_client_id], &g_clients_array[target_id], &g_channel_array[channel_id]);
        }
    }

    // unlock channels and clients
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Applies a permitted action to a client in the specified channel.
 * @param json_root Request containing the action and the target's current identity.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_client_action(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON *action = cJSON_GetObjectItemCaseSensitive(message, "action");
    cJSON *identity = cJSON_GetObjectItemCaseSensitive(message, "identity");
    uint64 target_id = 0;
    uint64 channel_id = 0;
    uint64 destination_id = 0;
    uint64 duration = 0;
    uint64 role = 0;
    uint64 now = 0;
    uint64 old_role = 0;
    client_t *actor = NULL_POINTER;
    client_t *target = NULL_POINTER;
    channel_t *channel = NULL_POINTER;
    permission_t permission = PERMISSION_COUNT;
    boole succeeded = FALSE;
    boole delete_temp = FALSE;
    char identity_hash[IDENTITY_HASH_MAX_LENGTH] = { 0 };

    // check the request fields
    if (!cJSON_IsString(action) ||
        !cJSON_IsString(identity) ||
        !_client_msg_moderation_internal__read_index(message, "client_id", g_server_settings.max_client_count, &target_id) ||
        !_client_msg_moderation_internal__read_index(message, "channel_id", g_server_settings.max_channel_count, &channel_id))
    {
        return;
    }

    // choose permission by action
    if (!clib__strcmp(action->valuestring, "move"))
    {
        permission = PERMISSION_MOVE_CLIENTS;
        if (_client_msg_moderation_internal__read_index(message, "destination_channel_id", g_server_settings.max_channel_count, &destination_id) == FALSE)
        {
            return;
        }
    }
    else if (!clib__strcmp(action->valuestring, "kick_from_channel"))
    {
        permission = PERMISSION_KICK_FROM_CHANNEL;
        destination_id = ROOT_CHANNEL_ID;
    }
    else if (!clib__strcmp(action->valuestring, "mute"))
    {
        permission = PERMISSION_MUTE_CLIENTS;
        if (_client_msg_moderation_internal__read_index(message, "duration_seconds", 86401, &duration) == FALSE)
        {
            return;
        }
    }
    else if (!clib__strcmp(action->valuestring, "stop_stream"))
    {
        permission = PERMISSION_STOP_CLIENT_STREAMS;
    }
    else if (!clib__strcmp(action->valuestring, "channel_group"))
    {
        permission = PERMISSION_ASSIGN_CHANNEL_GROUPS;
        if (_client_msg_moderation_internal__read_index(message, "role", CHANNEL_GROUP_LIMIT, &role) == FALSE)
        {
            return;
        }
    }
    else
    {
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // lock the channels
    clib__write_lock(&g_channels_global_rwlock_guard);

    // find actor and target
    actor = &g_clients_array[sender_client_id];
    target = &g_clients_array[target_id];

    // check the channel ids
    if (channel_id >= g_server_settings.max_channel_count || ((permission == PERMISSION_MOVE_CLIENTS || permission == PERMISSION_KICK_FROM_CHANNEL) && destination_id >= g_server_settings.max_channel_count))
    {
        goto end;
    }

    // check the target client
    channel = &g_channel_array[channel_id];
    if (!base__is_client_valid(sender_client_id) ||
        !base__is_client_valid(target_id) ||
        !channel->is_existing == TRUE ||
        (target->is_music_bot) != FALSE ||
        target->channel_id != channel_id ||
        (clib__strcmp(target->public_key, identity->valuestring)) != FALSE)
    {
        goto end;
    }

    // hash the target key
    util__hash_password_to_base64(target->public_key, identity_hash, sizeof(identity_hash));

    // assign a channel group
    if (permission == PERMISSION_ASSIGN_CHANNEL_GROUPS)
    {
        // check the assign rights
        old_role = channel_groups__role(target, channel);
        if (!g_server_settings.is_permission_system_enabled ||
            !channel_groups__can_assign(actor, target, channel, (uint64)role) ||
            (channel->is_temp_channel) != FALSE &&
            (target->is_temp_admin_channel) != FALSE &&
            target->temp_channel_id == channel_id)
        {
            goto denied;
        }

        // set and save role
        succeeded = channel_groups__set(channel, identity_hash, (uint64)role);
        if (succeeded && !channel->is_temp_channel && !_client_msg_moderation_internal__save())
        {
            channel_groups__set(channel, identity_hash, old_role);
            succeeded = FALSE;
        }

        // send rights to target
        channel_groups__refresh(target);
        server_msg__moderation__send_channel_access(target, channel);
        server_msg__moderation__send_channel_group_changed(target);
    }
    else
    {
        // check the moderation rights
        if (channel_groups__can_moderate(actor, target, channel, permission) == FALSE)
        {
            goto denied;
        }

        // move or kick target
        if (permission == PERMISSION_MOVE_CLIENTS || permission == PERMISSION_KICK_FROM_CHANNEL)
        {
            // check the destination
            if (channel_id == destination_id || !g_channel_array[destination_id].is_existing || target->is_idle)
            {
                goto end;
            }

            // check the destination permission
            if (permission == PERMISSION_MOVE_CLIENTS && !channel_groups__is_allowed(actor, &g_channel_array[destination_id], PERMISSION_MOVE_CLIENTS))
            {
                goto denied;
            }

            // check the channel access
            if (channel__check_channel_access(target, &g_channel_array[destination_id], NULL_POINTER, TRUE) == FALSE)
            {
                goto end;
            }

            // clear the owner state
            delete_temp = target->is_temp_admin_channel && target->temp_channel_id == channel_id && channel->is_temp_channel;
            if (delete_temp)
            {
                target->is_temp_admin_channel = FALSE;
                target->temp_channel_id = 0;
            }

            // move the target
            channel__move_client_into_channel(target_id, destination_id, permission == PERMISSION_KICK_FROM_CHANNEL ? "kick" : "move");

            // delete the temp channel
            if (delete_temp)
            {
                channel__destroy_temp_channel(channel_id);
            }
            succeeded = TRUE;
        }
        else if (permission == PERMISSION_MUTE_CLIENTS)
        {
            // read the monotonic time
            if (util__get_monotonic_ms(&now) == FALSE)
            {
                goto end;
            }

            // set or remove mute
            succeeded = channel_groups__mute(channel, identity_hash, duration ? now + duration * 1000 : 0);
            channel_groups__refresh(target);
            server_msg__moderation__send_result(target, duration ? "A moderator muted your microphone in this channel." : "Your channel mute was removed.", TRUE);
        }
        else if (channel->is_video_stream_active && channel->video_streamer_client_id == target_id)
        {
            // stop the target stream
            video_channel__stop(channel_id, "moderator_stopped");
            succeeded = TRUE;
        }
    }

    // skip the denied message
    goto end;

denied:
    // send access denied
    server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
end:
    // send the result
    if (base__is_client_valid(sender_client_id) != FALSE)
    {
        server_msg__moderation__send_result(actor, succeeded ? "Change applied." : "The change could not be applied.", succeeded);
    }

    // unlock the channels
    clib__unlock(&g_channels_global_rwlock_guard);

    // apply permission changes
    audio_channel__apply_permission_changes();
    video_channel__apply_permission_changes();
    channel__apply_permission_changes();

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Lists bans or removes a selected ban without requiring its IP address.
 * @param json_root Request with an optional removal token.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_bans(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(message, "remove_token");
    client_t *actor = NULL_POINTER;
    ban_entry_t saved;
    uint64 i = 0;
    boole show_addresses = FALSE;
    boole removed = FALSE;

    // check the removal token
    if (token != NULL_POINTER && (!cJSON_IsString(token) || clib__strlen(token->valuestring) != 32))
    {
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check the ban permissions
    actor = &g_clients_array[sender_client_id];
    if (!permissions__is_allowed(actor, PERMISSION_VIEW_BANS) ||
        token != NULL_POINTER &&
        !permissions__is_allowed(actor, PERMISSION_REMOVE_BANS))
    {
        server_msg__administration__send_permission_denied_to_single_client(actor, permissions__name(permissions__is_allowed(actor, PERMISSION_VIEW_BANS) ? PERMISSION_REMOVE_BANS : PERMISSION_VIEW_BANS));
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check IP address permission
    show_addresses = permissions__is_allowed(actor, PERMISSION_VIEW_IP_ADDRESSES);

    // lock channels, icons, groups
    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_groups_global_rwlock_guard);

    // lock the bans
    clib__write_lock(&g_bans_global_rwlock_guard);

    // remove the chosen ban
    if (token != NULL_POINTER)
    {
        // find ban by token
        for (i = 0; i < MAX_BANS; i++)
        {
            if (g_ban_array[i].is_existing && !clib__strcmp(g_ban_array[i].removal_token, token->valuestring))
            {
                // clear the ban, save
                saved = g_ban_array[i];
                clib__memset(&g_ban_array[i], 0, sizeof(ban_entry_t));
                removed = settings__save_server_settings_to_file();
                if (!removed)
                {
                    g_ban_array[i] = saved;
                }
                break;
            }
        }

        // send the result
        server_msg__moderation__send_result(actor, removed ? "Ban removed." : "The ban was not removed.", removed);
    }

    // send the ban list
    server_msg__moderation__send_bans(actor, show_addresses);

    // unlock all five locks
    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}
