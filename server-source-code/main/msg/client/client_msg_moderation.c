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

static boole _client_msg_moderation_internal__read_index(cJSON* object, cstring name, uint64 limit, uint64* result);
static boole _client_msg_moderation_internal__save(void);

/**
 * @brief Creates, edits or deletes a channel group without granting server rights.
 * @param json_root Complete group definition and requested action.
 * @param sender_client_id Authenticated client making the change.
 */
void client_msg__moderation__process_channel_group(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* action = cJSON_GetObjectItemCaseSensitive(message, "action");
    cJSON* name = cJSON_GetObjectItemCaseSensitive(message, "name");
    cJSON* icon = cJSON_GetObjectItemCaseSensitive(message, "icon_id");
    channel_group_t proposed = {0};
    channel_group_t previous;
    client_t* actor = NULL_POINTER;
    client_t* client = NULL_POINTER;
    uint64 id = 0;
    uint64 i = 0;
    uint64 j = 0;
    boole deleting = FALSE;
    boole saved = FALSE;
    permission_t permission = PERMISSION_CREATE_CHANNEL_GROUPS;

    if (!cJSON_IsString(action))
    {
        return;
    }
    deleting = !clib__strcmp(action->valuestring, "delete");
    permission = deleting ? PERMISSION_DELETE_CHANNEL_GROUPS
        : !clib__strcmp(action->valuestring, "create") ? PERMISSION_CREATE_CHANNEL_GROUPS : PERMISSION_EDIT_CHANNEL_GROUPS;
    if (!deleting)
    {
        status0 = (!clib__strcmp(action->valuestring, "create") || !clib__strcmp(action->valuestring, "update")) == FALSE;
        if (status0 == FALSE)
        {
            status0 = !cJSON_IsString(name);
        }
        if (status0 == FALSE)
        {
            status0 = !name->valuestring[0];
        }
        if (status0 == FALSE)
        {
            status0 = clib__strlen(name->valuestring) >= sizeof(proposed.name);
        }
        if (status0 == FALSE)
        {
            status0 = !cJSON_IsNumber(icon);
        }
        if (status0 == FALSE)
        {
            status0 = icon->valuedouble >= -2;
            if (status0 == TRUE)
            {
                status0 = icon->valuedouble < MAX_ICONS;
            }
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
        if (status0 == TRUE)
        {
            clib__read_lock(&g_clients_global_rwlock_guard);
            status = base__is_current_client_request(sender_client_id);
            if (status == FALSE)
            {
                clib__unlock(&g_clients_global_rwlock_guard);
                return;
            }
            server_msg__moderation__send_result(&g_clients_array[sender_client_id], "Invalid channel group settings.", FALSE);
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }
        proposed.is_existing = TRUE;
        proposed.icon_id = icon->valueint;
        clib__string_copy(proposed.name, sizeof(proposed.name), name->valuestring);
    }
    status0 = (clib__strcmp(action->valuestring, "create")) != FALSE;
    if (status0 == TRUE)
    {
        status0 = !_client_msg_moderation_internal__read_index(message, "group_id", CHANNEL_GROUP_LIMIT, &id);
        if (status0 == FALSE)
        {
            status0 = id == 0;
        }
        if (status0 == FALSE)
        {
            status0 = (deleting) != FALSE;
            if (status0 == TRUE)
            {
                status0 = id == CHANNEL_GROUP_ADMIN;
            }
        }
    }
    if (status0 == TRUE)
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);
    actor = &g_clients_array[sender_client_id];
    if (!base__is_client_valid(sender_client_id) || !g_server_settings.is_permission_system_enabled
        || !permissions__is_allowed(actor, permission))
    {
        server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
        goto end;
    }
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
    status = channel_groups__can_edit_definition(actor, permission, g_channel_groups[id].is_existing ? &g_channel_groups[id] : NULL_POINTER,
        deleting ? NULL_POINTER : &proposed);
    if (status == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
        goto end;
    }
    clib__read_lock(&g_icons_global_rwlock_guard);
    if (!deleting && proposed.icon_id >= 0 && !g_icons_array[proposed.icon_id].is_existing)
    {
        clib__unlock(&g_icons_global_rwlock_guard);
        goto end;
    }
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
    if (saved)
    {
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
    server_msg__moderation__send_result(actor, saved ? "Channel group saved." : "The channel group was not saved.", saved);
    clib__unlock(&g_channels_global_rwlock_guard);
    if (saved)
    {
        server_msg__administration__send_permission_updates_to_all_clients();
        audio_channel__apply_permission_changes();
        video_channel__apply_permission_changes();
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Persists emoji changes without accepting other server settings.
 * @param json_root Request without settings fields.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_save_emojis(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* actor = NULL_POINTER;
    boole saved = FALSE;

    (void)json_root;
    clib__write_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
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
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Applies a permitted playback control to a music bot.
 * @param json_root Request containing bot_id and play, pause or next.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_music_control(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* action = cJSON_GetObjectItemCaseSensitive(message, "action");
    uint64 bot_id = 0;
    client_t* actor = NULL_POINTER;
    client_t* bot = NULL_POINTER;

    if (!cJSON_IsString(action) || !_client_msg_moderation_internal__read_index(message, "bot_id", g_server_settings.max_client_count, &bot_id))
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    actor = &g_clients_array[sender_client_id];
    bot = &g_clients_array[bot_id];
    if (!permissions__is_allowed(actor, PERMISSION_CONTROL_MUSIC_PLAYBACK))
    {
        server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
    }
    else if (base__is_client_valid_musicbot(bot_id))
    {
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
        server_msg__musicbot__send_song_list_to_single_client(actor, bot);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Reads an integer before it is used as an array index.
 * @param object Request fields.
 * @param name Field to read.
 * @param limit Exclusive upper bound.
 * @param result Validated index.
 */
static boole _client_msg_moderation_internal__read_index(cJSON* object, cstring name, uint64 limit, uint64* result)
{
    boole status0 = FALSE;
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);

    status0 = !cJSON_IsNumber(value);
    if (status0 == FALSE)
    {
        status0 = value->valuedouble >= 0;
        if (status0 == TRUE)
        {
            status0 = value->valuedouble < limit;
        }
        status0 = status0 == FALSE;
    }
    if (status0 == FALSE)
    {
        status0 = value->valuedouble != (double)(uint64)value->valuedouble;
    }
    if (status0 == TRUE)
    {
        return FALSE;
    }
    *result = (uint64)value->valuedouble;
    return TRUE;
}

/**
 * @brief Saves a channel assignment while the clients and channels locks are held.
 */
static boole _client_msg_moderation_internal__save(void)
{
    boole saved = FALSE;

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
void client_msg__moderation__process_channel_access(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    uint64 channel_id = 0;

    status = _client_msg_moderation_internal__read_index(message, "channel_id", g_server_settings.max_channel_count, &channel_id);
    if (status == FALSE)
    {
        return;
    }
    clib__read_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__read_lock(&g_channels_global_rwlock_guard);
    if (base__is_client_valid(sender_client_id) && g_channel_array[channel_id].is_existing)
    {
        server_msg__moderation__send_channel_access(&g_clients_array[sender_client_id], &g_channel_array[channel_id]);
        uint64 target_id = 0;
        cJSON* identity = cJSON_GetObjectItemCaseSensitive(message, "identity");
        status0 = (_client_msg_moderation_internal__read_index(message, "client_id", g_server_settings.max_client_count, &target_id)) != FALSE;
        if (status0 == TRUE)
        {
            status0 = (cJSON_IsString(identity)) != FALSE;
        }
        if (status0 == TRUE)
        {
            status0 = (base__is_client_valid(target_id)) != FALSE;
        }
        if (status0 == TRUE)
        {
            status0 = g_clients_array[target_id].channel_id == channel_id;
        }
        if (status0 == TRUE)
        {
            status0 = !clib__strcmp(g_clients_array[target_id].public_key, identity->valuestring);
        }
        if (status0 == TRUE)
        {
            server_msg__moderation__send_client_access(&g_clients_array[sender_client_id], &g_clients_array[target_id], &g_channel_array[channel_id]);
        }
    }
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Applies a permitted action to a client in the specified channel.
 * @param json_root Request containing the action and the target's current identity.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_client_action(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    boole status3 = FALSE;
    boole status4 = FALSE;
    boole status5 = FALSE;
    boole status6 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* action = cJSON_GetObjectItemCaseSensitive(message, "action");
    cJSON* identity = cJSON_GetObjectItemCaseSensitive(message, "identity");
    uint64 target_id = 0;
    uint64 channel_id = 0;
    uint64 destination_id = 0;
    uint64 duration = 0;
    uint64 role = 0;
    uint64 now = 0;
    uint old_role = 0;
    client_t* actor = NULL_POINTER;
    client_t* target = NULL_POINTER;
    channel_t* channel = NULL_POINTER;
    permission_t permission = PERMISSION_COUNT;
    boole succeeded = FALSE;
    boole delete_temp = FALSE;
    char identity_hash[IDENTITY_HASH_MAX_LENGTH] = {0};

    status0 = !cJSON_IsString(action);
    if (status0 == FALSE)
    {
        status0 = !cJSON_IsString(identity);
    }
    if (status0 == FALSE)
    {
        status0 = !_client_msg_moderation_internal__read_index(message, "client_id", g_server_settings.max_client_count, &target_id);
    }
    if (status0 == FALSE)
    {
        status0 = !_client_msg_moderation_internal__read_index(message, "channel_id", g_server_settings.max_channel_count, &channel_id);
    }
    if (status0 == TRUE)
    {
        return;
    }
    if (!clib__strcmp(action->valuestring, "move"))
    {
        permission = PERMISSION_MOVE_CLIENTS;
        status = _client_msg_moderation_internal__read_index(message, "destination_channel_id", g_server_settings.max_channel_count, &destination_id);
        if (status == FALSE)
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
        status = _client_msg_moderation_internal__read_index(message, "duration_seconds", 86401, &duration);
        if (status == FALSE)
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
        status = _client_msg_moderation_internal__read_index(message, "role", CHANNEL_GROUP_LIMIT, &role);
        if (status == FALSE)
        {
            return;
        }
    }
    else
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);
    actor = &g_clients_array[sender_client_id];
    target = &g_clients_array[target_id];
    channel = &g_channel_array[channel_id];
    status0 = !base__is_client_valid(sender_client_id);
    if (status0 == FALSE)
    {
        status0 = !base__is_client_valid(target_id);
    }
    if (status0 == FALSE)
    {
        status4 = !channel->is_existing;
        status5 = (target->is_music_bot) != FALSE;
        status6 = target->channel_id != channel_id;
        status0 = status4 == TRUE || status5 == TRUE || status6 == TRUE;
    }
    if (status0 == FALSE)
    {
        status0 = (clib__strcmp(target->public_key, identity->valuestring)) != FALSE;
    }
    if (status0 == TRUE)
    {
        goto end;
    }
    util__hash_password_to_base64(target->public_key, identity_hash, sizeof(identity_hash));
    if (permission == PERMISSION_ASSIGN_CHANNEL_GROUPS)
    {
        old_role = channel_groups__role(target, channel);
        status0 = !g_server_settings.is_permission_system_enabled;
        if (status0 == FALSE)
        {
            status0 = !channel_groups__can_assign(actor, target, channel, (uint)role);
        }
        if (status0 == FALSE)
        {
            status1 = (channel->is_temp_channel) != FALSE;
            status2 = (target->is_temp_admin_channel) != FALSE;
            status3 = target->temp_channel_id == channel_id;
            status0 = status1 == TRUE && status2 == TRUE && status3 == TRUE;
        }
        if (status0 == TRUE)
        {
            goto denied;
        }
        succeeded = channel_groups__set(channel, identity_hash, (uint)role);
        if (succeeded && !channel->is_temp_channel && !_client_msg_moderation_internal__save())
        {
            channel_groups__set(channel, identity_hash, old_role);
            succeeded = FALSE;
        }
        channel_groups__refresh(target);
        server_msg__moderation__send_channel_access(target, channel);
        server_msg__moderation__send_channel_group_changed(target);
    }
    else
    {
        status = channel_groups__can_moderate(actor, target, channel, permission);
        if (status == FALSE)
        {
            goto denied;
        }
        if (permission == PERMISSION_MOVE_CLIENTS || permission == PERMISSION_KICK_FROM_CHANNEL)
        {
            if (channel_id == destination_id || !g_channel_array[destination_id].is_existing || target->is_idle)
            {
                goto end;
            }
            if (permission == PERMISSION_MOVE_CLIENTS
                && !channel_groups__is_allowed(actor, &g_channel_array[destination_id], PERMISSION_MOVE_CLIENTS))
            {
                goto denied;
            }
            status = channel__check_channel_access(target, &g_channel_array[destination_id], NULL_POINTER, TRUE);
            if (status == FALSE)
            {
                goto end;
            }
            delete_temp = target->is_temp_admin_channel && target->temp_channel_id == channel_id && channel->is_temp_channel;
            if (delete_temp)
            {
                target->is_temp_admin_channel = FALSE;
                target->temp_channel_id = 0;
            }
            channel__move_client_into_channel(target_id, destination_id);
            channel__send_hidden_members(target, &g_channel_array[destination_id]);
            if (delete_temp)
            {
                channel__destroy_temp_channel(channel_id);
            }
            succeeded = TRUE;
        }
        else if (permission == PERMISSION_MUTE_CLIENTS)
        {
            status = util__get_monotonic_ms(&now);
            if (status == FALSE)
            {
                goto end;
            }
            succeeded = channel_groups__mute(channel, identity_hash, duration ? now + duration * 1000 : 0);
            channel_groups__refresh(target);
            server_msg__moderation__send_result(target, duration ? "A moderator muted your microphone in this channel." : "Your channel mute was removed.", TRUE);
        }
        else if (channel->is_video_stream_active && channel->video_streamer_client_id == target_id)
        {
            video_channel__stop(channel_id, "moderator_stopped");
            succeeded = TRUE;
        }
    }
    goto end;

denied:
    server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
end:
    status = base__is_client_valid(sender_client_id);
    if (status != FALSE)
    {
        server_msg__moderation__send_result(actor, succeeded ? "Change applied." : "The change could not be applied.", succeeded);
    }
    clib__unlock(&g_channels_global_rwlock_guard);
    audio_channel__apply_permission_changes();
    video_channel__apply_permission_changes();
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Lists bans or removes a selected ban without requiring its IP address.
 * @param json_root Request with an optional removal token.
 * @param sender_client_id Authenticated requester.
 */
void client_msg__moderation__process_bans(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* token = cJSON_GetObjectItemCaseSensitive(message, "remove_token");
    client_t* actor = NULL_POINTER;
    ban_entry_t saved;
    uint64 i = 0;
    boole show_addresses = FALSE;
    boole removed = FALSE;

    status0 = token != NULL_POINTER;
    if (status0 == TRUE)
    {
        status0 = !cJSON_IsString(token);
        if (status0 == FALSE)
        {
            status0 = clib__strlen(token->valuestring) != 32;
        }
    }
    if (status0 == TRUE)
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    status = base__is_current_client_request(sender_client_id);
    if (status == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    actor = &g_clients_array[sender_client_id];
    status0 = !permissions__is_allowed(actor, PERMISSION_VIEW_BANS);
    if (status0 == FALSE)
    {
        status0 = token != NULL_POINTER;
        if (status0 == TRUE)
        {
            status0 = !permissions__is_allowed(actor, PERMISSION_REMOVE_BANS);
        }
    }
    if (status0 == TRUE)
    {
        server_msg__administration__send_access_denied_to_single_client(actor, NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    show_addresses = permissions__is_allowed(actor, PERMISSION_VIEW_IP_ADDRESSES);
    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_groups_global_rwlock_guard);
    clib__write_lock(&g_bans_global_rwlock_guard);
    if (token != NULL_POINTER)
    {
        for (i = 0; i < MAX_BANS; i++)
        {
            if (g_ban_array[i].is_existing && !clib__strcmp(g_ban_array[i].removal_token, token->valuestring))
            {
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
        server_msg__moderation__send_result(actor, removed ? "Ban removed." : "The ban was not removed.", removed);
    }
    server_msg__moderation__send_bans(actor, show_addresses);
    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}
