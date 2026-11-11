#include "../../clib/clib.h"
// This file sends channel rights, moderation results and the permitted ban details.
#include "server_msg.h"
#include "../../base.h"
#include "../../core/channel_groups.h"
#include "../../core/channel.h"
#include "../../core/permissions.h"
#include "../../util.h"
#include <stdio.h>

/**
 * @brief Reports whether a requested change was applied.
 * @param receiver Client receiving the result with the clients lock held.
 * @param text Short explanation for the user.
 * @param succeeded Whether the operation succeeded.
 */
void server_msg__moderation__send_result(client_t *receiver, cstring text, boole succeeded)
{
    cJSON *message = NULL_POINTER;
    cJSON *root = server_msg__delivery__new_message("moderation_result", &message);

    // add result and text
    cJSON_AddBoolToObject(message, "succeeded", succeeded);
    cJSON_AddStringToObject(message, "text", text);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends the receiver's rights for one channel.
 * @param receiver Authenticated client with the clients lock held.
 * @param channel Existing channel with the channels lock held.
 */
void server_msg__moderation__send_channel_access(client_t *receiver, channel_t *channel)
{
    cJSON *message = NULL_POINTER;
    cJSON *root = server_msg__delivery__new_message("channel_access", &message);
    cJSON *rights = cJSON_CreateObject();
    cJSON *powers = cJSON_CreateObject();
    cstring power_names[] = { "talk_power", "channel_message_power", "channel_picture_power", "stream_power" };
    permission_t actions[] = { PERMISSION_EDIT_CHANNEL_NAME, PERMISSION_EDIT_CHANNEL_DESCRIPTION, PERMISSION_EDIT_CHANNEL_PASSWORD, PERMISSION_EDIT_CHANNEL_POWERS, PERMISSION_EDIT_CHANNEL_OPTIONS, PERMISSION_DELETE_CHANNEL, PERMISSION_ASSIGN_CHANNEL_GROUPS, PERMISSION_MOVE_CLIENTS, PERMISSION_KICK_FROM_CHANNEL, PERMISSION_MUTE_CLIENTS, PERMISSION_STOP_CLIENT_STREAMS };
    uint64 i = 0;
    cstring type_error = NULL_POINTER;

    // add the channel type
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddBoolToObject(message, "is_temp_channel", channel->is_temp_channel);

    // add allowed type changes
    type_error = channel__type_change_error(receiver, channel, TRUE);
    cJSON_AddBoolToObject(message, "can_make_temporary", type_error == NULL_POINTER);
    cJSON_AddStringToObject(message, "temporary_type_error", type_error ? type_error : "");
    type_error = channel__type_change_error(receiver, channel, FALSE);
    cJSON_AddBoolToObject(message, "can_make_permanent", type_error == NULL_POINTER);
    cJSON_AddStringToObject(message, "permanent_type_error", type_error ? type_error : "");

    // add bypass and role
    cJSON_AddBoolToObject(message, "can_bypass_password", permissions__can_bypass_channel_password(receiver, channel));
    cJSON_AddNumberToObject(message, "role", channel_groups__role(receiver, channel));

    // add the channel rights
    for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
    {
        cJSON_AddBoolToObject(rights, permissions__name(actions[i]), channel_groups__is_allowed(receiver, channel, actions[i]));
    }
    util__json_add_owned_object(message, "rights", rights);

    // add the channel powers
    for (i = 0; i < sizeof(power_names) / sizeof(power_names[0]); i++)
    {
        cJSON_AddNumberToObject(powers, power_names[i], permissions__power_in_channel(receiver, channel, (permission_power_t)(PERMISSION_TALK_POWER + i)));
    }
    util__json_add_owned_object(message, "powers", powers);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/* These hints use the same checks as the action handler. Every action is checked again on receipt. */
void server_msg__moderation__send_client_access(client_t *receiver, client_t *target, channel_t *channel)
{
    cJSON *message = NULL_POINTER;
    cJSON *root = server_msg__delivery__new_message("client_context_access", &message);
    cJSON *actions = cJSON_CreateObject();
    cJSON *roles = cJSON_CreateArray();
    permission_t permissions[] = { PERMISSION_MOVE_CLIENTS, PERMISSION_KICK_FROM_CHANNEL, PERMISSION_MUTE_CLIENTS, PERMISSION_STOP_CLIENT_STREAMS };
    cstring names[] = { "move", "kick_from_channel", "mute", "stop_stream" };
    uint64 i = 0;

    // add client and channel
    cJSON_AddNumberToObject(message, "client_id", (double)target->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)channel->channel_id);
    cJSON_AddStringToObject(message, "identity", target->public_key);

    // fill the allowed actions
    for (i = 0; i < sizeof(permissions) / sizeof(permissions[0]); i++)
    {
        cJSON_AddBoolToObject(actions, names[i], !target->is_idle && channel_groups__can_moderate(receiver, target, channel, permissions[i]));
    }

    // fill assignable channel groups
    if (g_server_settings.is_permission_system_enabled && !(channel->is_temp_channel && target->is_temp_admin_channel && target->temp_channel_id == channel->channel_id))
    {
        for (i = 0; i < CHANNEL_GROUP_LIMIT; i++)
        {
            if ((i == CHANNEL_GROUP_MEMBER || g_channel_groups[i].is_existing) && channel_groups__can_assign(receiver, target, channel, i))
            {
                util__json_add_owned_array(roles, cJSON_CreateNumber(i));
            }
        }
    }

    // add both to message
    util__json_add_owned_object(message, "actions", actions);
    util__json_add_owned_object(message, "channel_groups", roles);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Updates visible channel assignments without sending a channel join event.
 * @param subject Changed client with the clients and channels locks held.
 */
void server_msg__moderation__send_channel_group_changed(client_t *subject)
{
    cJSON *message = NULL_POINTER;
    cJSON *root = server_msg__delivery__new_message("channel_group_changed", &message);
    uint64 i = 0;
    client_t *receiver = NULL_POINTER;

    // add client, channel, group
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "channel_id", (double)subject->channel_id);
    cJSON_AddNumberToObject(message, "channel_group_id", subject->is_idle ? 0 : subject->channel_group_role);

    // send to allowed clients
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        receiver = &g_clients_array[i];
        if (!base__is_client_valid(i) || receiver->is_music_bot)
        {
            continue;
        }
        if (channel__is_client_location_visible(receiver, subject) == FALSE)
        {
            continue;
        }
        server_msg__delivery__send_json_to_client(receiver, root);
    }

    // free the message
    cJSON_Delete(root);
}

/**
 * @brief Sends bans without exposing addresses to a user who may not view them.
 * @param receiver Permitted reader with the clients lock held.
 * @param show_addresses Whether the reader may view IP addresses.
 * @attention The caller holds the bans write lock while tokens are assigned.
 */
void server_msg__moderation__send_bans(client_t *receiver, boole show_addresses)
{
    cJSON *message = NULL_POINTER;
    cJSON *root = server_msg__delivery__new_message("ban_list", &message);
    cJSON *entries = cJSON_CreateArray();
    cJSON *entry = NULL_POINTER;
    ban_entry_t *ban = NULL_POINTER;
    ubyte token[16] = { 0 };
    uint64 i = 0;
    uint64 j = 0;

    // add every ban
    for (i = 0; i < MAX_BANS; i++)
    {
        // skip empty slots
        ban = &g_ban_array[i];
        if (!ban->is_existing)
        {
            continue;
        }

        // create missing removal token
        if (!ban->removal_token[0])
        {
            if (util__fill_secure_random_bytes(token, sizeof(token)) == FALSE)
            {
                continue;
            }
            for (j = 0; j < sizeof(token); j++)
            {
                clib__string_format(ban->removal_token + j * 2, 3, "%02x", token[j]);
            }
        }

        // add token, time, identity
        entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "token", ban->removal_token);
        cJSON_AddNumberToObject(entry, "timestamp", (double)ban->timestamp_banned);
        cJSON_AddStringToObject(entry, "identity", ban->identity);

        // add address if allowed
        if (show_addresses)
        {
            cJSON_AddStringToObject(entry, "ip_address", ban->ip_address);
        }

        // add ban to array
        util__json_add_owned_array(entries, entry);
    }

    // add the ban array
    util__json_add_owned_object(message, "entries", entries);

    // send to the receiver
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}
