#include "../clib/clib_vector.h"
#include "../clib/clib.h"
// This file checks what a client's groups let them do and reads and saves group permissions.
#include "permissions.h"
#include "identity_store.h"
#include "channel_groups.h"
#include "../base.h"
#include "../util.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include <string.h>

uint64 g_guest_group_id = MAX_GROUPS;

static const char *g_permission_names[PERMISSION_COUNT] = { "create_channel", "create_temp_channel", "kick", "ban", "view_stored_users", "view_group_permissions", "view_group_members", "create_groups", "edit_groups", "delete_groups", "assign_groups", "remove_groups", "edit_group_permissions", "upload_icons", "view_client_details", "view_server_logs", "view_server_info", "send_offline_messages", "receive_offline_messages", "message_idle_clients", "call_idle_clients", "delete_icons", "upload_emojis", "delete_emojis", "view_ip_addresses", "view_bans", "remove_bans", "move_clients", "kick_from_channel", "mute_clients", "stop_client_streams", "edit_channel_name", "edit_channel_description", "edit_channel_password", "edit_channel_powers", "edit_channel_options", "delete_channel", "assign_channel_groups", "send_direct_pictures", "send_direct_files", "send_channel_pictures", "send_channel_files", "create_music_bots", "delete_music_bots", "upload_music_tracks", "delete_music_tracks", "control_music_playback", "use_reserved_slots", "create_channel_groups", "edit_channel_groups", "delete_channel_groups", "view_client_connection_time", "bypass_channel_password", "send_direct_messages", "send_channel_messages", "send_pokes", "view_channel_members", "view_channel_descriptions" };
static const char *g_permission_power_names[PERMISSION_POWER_COUNT] = { "kick_power", "kick_protection", "ban_power", "ban_protection", "group_management_power", "group_protection", "talk_power", "channel_message_power", "channel_picture_power", "stream_power", "move_power", "move_protection", "moderation_power", "moderation_protection", "permission_edit_power", "direct_message_power", "direct_message_protection", "direct_picture_power", "direct_picture_protection", "direct_file_power", "direct_file_protection", "poke_power", "poke_protection", "channel_file_power", "channel_view_power", "channel_create_power" };

// Powers for using a feature; a group that does not set one gets 25, like the Guest and Normal presets.
static const permission_power_t g_permission_usage_powers[] = { PERMISSION_TALK_POWER, PERMISSION_CHANNEL_MESSAGE_POWER, PERMISSION_CHANNEL_PICTURE_POWER, PERMISSION_STREAM_POWER, PERMISSION_DIRECT_MESSAGE_POWER, PERMISSION_DIRECT_PICTURE_POWER, PERMISSION_DIRECT_FILE_POWER, PERMISSION_POKE_POWER, PERMISSION_CHANNEL_FILE_POWER, PERMISSION_CHANNEL_VIEW_POWER, PERMISSION_CHANNEL_CREATE_POWER };

static void _permissions_internal__resolve(const client_t *client, group_permissions_t *result);
static void _permissions_internal__resolve_change(const client_t *client, uint64 group_id, const group_permissions_t *replacement, group_permissions_t *result);
static boole _permissions_internal__has_group(const client_t *client, uint64 group_id);
static boole _permissions_internal__allows_group_members_change(const client_t *sender, uint64 group_id, const group_permissions_t *proposed);
static boole _permissions_internal__allows_change(const client_t *sender, const client_t *target, uint64 group_id, const group_permissions_t *replacement);
static boole _permissions_internal__is_allowed(const client_t *client, const group_permissions_t *result, permission_t permission);
static void _permissions_internal__combine(group_permissions_t *result, const group_permissions_t *group);

/**
 * @brief Fills a new group's permissions from the selected preset.
 * @param type Preset selected when creating the group.
 * @param permissions Receives the initial permissions and powers.
 */
void permissions__set_preset(group_type_t type, group_permissions_t *permissions)
{
    uint64 i = 0;

    clib__memset(permissions, 0, sizeof(*permissions));
    permissions__init_grants(permissions);
    permissions->powers[PERMISSION_EDIT_POWER] = type == GROUP_TYPE_ADMIN ? 1000 : type == GROUP_TYPE_MODERATOR ? 50 : type == GROUP_TYPE_NORMAL ? 25 : 0;
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        permissions->values[i] = PERMISSION_DENY;
    }
    permissions->values[PERMISSION_CREATE_TEMP_CHANNEL] = PERMISSION_GRANT;
    permissions->values[PERMISSION_VIEW_SERVER_INFO] = PERMISSION_GRANT;
    permissions->values[PERMISSION_VIEW_CLIENT_CONNECTION_TIME] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_DIRECT_PICTURES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_DIRECT_FILES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_CHANNEL_PICTURES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_CHANNEL_FILES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_DIRECT_MESSAGES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_CHANNEL_MESSAGES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_SEND_POKES] = PERMISSION_GRANT;
    permissions->values[PERMISSION_VIEW_CHANNEL_MEMBERS] = PERMISSION_GRANT;
    permissions->values[PERMISSION_VIEW_CHANNEL_DESCRIPTIONS] = PERMISSION_GRANT;
    for (i = 0; i < sizeof(g_permission_usage_powers) / sizeof(g_permission_usage_powers[0]); i++)
    {
        permissions->powers[g_permission_usage_powers[i]] = type == GROUP_TYPE_MODERATOR ? 50 : 25;
    }
    if (type != GROUP_TYPE_GUEST)
    {
        permissions->values[PERMISSION_MESSAGE_IDLE_CLIENTS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_CALL_IDLE_CLIENTS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_CREATE_CHANNEL] = PERMISSION_GRANT;
        permissions->values[PERMISSION_VIEW_STORED_USERS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_VIEW_GROUP_PERMISSIONS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_SEND_OFFLINE_MESSAGES] = PERMISSION_GRANT;
        permissions->values[PERMISSION_RECEIVE_OFFLINE_MESSAGES] = PERMISSION_GRANT;
    }
    if (type == GROUP_TYPE_MODERATOR)
    {
        permissions->values[PERMISSION_KICK] = PERMISSION_GRANT;
        permissions->values[PERMISSION_BAN] = PERMISSION_GRANT;
        permissions->values[PERMISSION_VIEW_CLIENT_DETAILS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_MOVE_CLIENTS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_KICK_FROM_CHANNEL] = PERMISSION_GRANT;
        permissions->values[PERMISSION_MUTE_CLIENTS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_STOP_CLIENT_STREAMS] = PERMISSION_GRANT;
        permissions->values[PERMISSION_VIEW_BANS] = PERMISSION_GRANT;
        permissions->powers[PERMISSION_MOVE_POWER] = 50;
        permissions->powers[PERMISSION_MOVE_PROTECTION] = 50;
        permissions->powers[PERMISSION_MODERATION_POWER] = 50;
        permissions->powers[PERMISSION_MODERATION_PROTECTION] = 50;
        permissions->values[PERMISSION_VIEW_GROUP_MEMBERS] = PERMISSION_GRANT;
        permissions->powers[PERMISSION_KICK_POWER] = 50;
        permissions->powers[PERMISSION_KICK_PROTECTION] = 50;
        permissions->powers[PERMISSION_BAN_POWER] = 50;
        permissions->powers[PERMISSION_BAN_PROTECTION] = 50;
        permissions->powers[PERMISSION_GROUP_PROTECTION] = 50;
    }
    if (type == GROUP_TYPE_ADMIN)
    {
        for (i = 0; i < PERMISSION_COUNT; i++)
        {
            permissions->values[i] = PERMISSION_GRANT;
        }
        for (i = 0; i < PERMISSION_POWER_COUNT; i++)
        {
            permissions->powers[i] = PERMISSION_POWER_MAX;
        }

        // A member of this group stays reachable by messages and pokes until an admin raises these values.
        permissions->powers[PERMISSION_DIRECT_MESSAGE_PROTECTION] = 0;
        permissions->powers[PERMISSION_DIRECT_PICTURE_PROTECTION] = 0;
        permissions->powers[PERMISSION_DIRECT_FILE_PROTECTION] = 0;
        permissions->powers[PERMISSION_POKE_PROTECTION] = 0;
    }
}

/**
 * @brief Reads the preset name used in saved groups and creation requests.
 * @param name Preset name to check.
 * @param type Receives the preset when the name is valid.
 */
boole permissions__parse_group_type(cstring name, group_type_t *type)
{
    uint64 i = 0;

    if (name == NULL_POINTER || type == NULL_POINTER)
    {
        return FALSE;
    }
    for (i = GROUP_TYPE_NORMAL; i <= GROUP_TYPE_ADMIN; i++)
    {
        if (clib__strcmp(name, permissions__group_type_name((group_type_t)i)) == 0)
        {
            *type = (group_type_t)i;
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Returns the saved name of a group preset.
 * @param type Preset whose name is needed.
 */
cstring permissions__group_type_name(group_type_t type)
{
    switch (type)
    {
    case GROUP_TYPE_GUEST:
        return "guest";
    case GROUP_TYPE_MODERATOR:
        return "moderator";
    case GROUP_TYPE_ADMIN:
        return "admin";
    default:
        return "normal";
    }
}

/**
 * @brief Adds Guest when no other group remains and removes Guest otherwise.
 * @param client Client to update with the clients and groups locks held.
 * @return TRUE when the membership list changed.
 */
boole permissions__refresh_guest_membership(client_t *client)
{
    uint64 i = 0;
    int64 id = 0;
    boole has_other = FALSE;
    boole has_guest = FALSE;
    boole changed = FALSE;

    if (g_server_settings.is_permission_system_enabled == FALSE || g_guest_group_id >= MAX_GROUPS || client == NULL_POINTER || client->is_music_bot == TRUE)
    {
        return FALSE;
    }
    for (i = 0; i < cvector_size(client->group_ids); i++)
    {
        id = client->group_ids[i];
        if (id >= 0 && id < MAX_GROUPS && (uint64)id != g_guest_group_id && g_groups_array[id].is_existing == TRUE)
        {
            has_other = TRUE;
        }
    }
    i = 0;
    while (i < cvector_size(client->group_ids))
    {
        id = client->group_ids[i];
        if (id < 0 || id >= MAX_GROUPS || g_groups_array[id].is_existing == FALSE || ((uint64)id == g_guest_group_id && (has_other == TRUE || has_guest == TRUE)))
        {
            cvector_erase(client->group_ids, i);
            changed = TRUE;
            continue;
        }
        if ((uint64)id == g_guest_group_id)
        {
            has_guest = TRUE;
        }
        i++;
    }
    if (has_other == FALSE && has_guest == FALSE)
    {
        changed = clib__int_vector_push(&client->group_ids, (int)g_guest_group_id);
    }
    permissions__refresh_client_powers(client);
    return changed;
}

/**
 * @brief Combines ordinary values by maximum, or uses the lowest negated value.
 */
static void _permissions_internal__combine_value(uint64 *result, uint64 *result_flags, uint64 value, uint64 flags)
{
    if (flags & PERMISSION_FLAG_NEGATE)
    {
        if (!(*result_flags & PERMISSION_FLAG_NEGATE) || value < *result)
        {
            *result = value;
        }
    }
    else if (!(*result_flags & PERMISSION_FLAG_NEGATE) && value > *result)
    {
        *result = value;
    }
    *result_flags |= flags;
}

/**
 * @brief Returns the legacy fallback when an action has no explicit value.
 */
static uint64 _permissions_internal__action_value(permission_t permission, uint64 value)
{
    if (value != PERMISSION_DEFAULT)
    {
        return value;
    }
    if (permission == PERMISSION_CREATE_TEMP_CHANNEL || permission == PERMISSION_VIEW_SERVER_INFO || permission == PERMISSION_VIEW_CLIENT_CONNECTION_TIME || permission == PERMISSION_MESSAGE_IDLE_CLIENTS || permission == PERMISSION_CALL_IDLE_CLIENTS || permission == PERMISSION_SEND_DIRECT_PICTURES || permission == PERMISSION_SEND_DIRECT_FILES || permission == PERMISSION_SEND_CHANNEL_PICTURES || permission == PERMISSION_SEND_CHANNEL_FILES)
    {
        return PERMISSION_GRANT;
    }

    // These actions needed no permission before they were added, so a group saved without them allows them.
    if (permission == PERMISSION_SEND_DIRECT_MESSAGES || permission == PERMISSION_SEND_CHANNEL_MESSAGES || permission == PERMISSION_SEND_POKES || permission == PERMISSION_VIEW_CHANNEL_MEMBERS || permission == PERMISSION_VIEW_CHANNEL_DESCRIPTIONS)
    {
        return PERMISSION_GRANT;
    }
    return PERMISSION_DENY;
}

/**
 * @brief Combines server group values and their optional conflict flags.
 * @param result Permissions collected so far.
 * @param group Permissions to include.
 */
static void _permissions_internal__combine(group_permissions_t *result, const group_permissions_t *group)
{
    uint64 p = 0;

    if (group == NULL_POINTER)
    {
        return;
    }
    for (p = 0; p < PERMISSION_COUNT; p++)
    {
        _permissions_internal__combine_value(&result->values[p], &result->flags[p], group->values[p], group->flags[p]);
    }
    for (p = 0; p < PERMISSION_POWER_COUNT; p++)
    {
        _permissions_internal__combine_value(&result->powers[p], &result->power_flags[p], group->powers[p], group->power_flags[p]);
    }
}

/**
 * @brief Combines group permissions, optionally replacing one group to check a proposed change.
 * @param client -> client whose groups are read with the clients and groups locks held
 * @param group_id -> group to replace, or MAX_GROUPS to leave all groups unchanged
 * @param replacement -> proposed permissions, or NULL to remove the group
 * @param result -> combined permissions
 */
static void _permissions_internal__resolve_change(const client_t *client, uint64 group_id, const group_permissions_t *replacement, group_permissions_t *result)
{
    uint64 i = 0;
    uint64 group_count = 0;
    int64 id = 0;
    const group_permissions_t *group = NULL_POINTER;

    clib__memset(result, 0, sizeof(*result));
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        return;
    }
    for (i = 0; i <= cvector_size(client->group_ids); i++)
    {
        if (i == cvector_size(client->group_ids))
        {
            group = group_id == g_guest_group_id ? NULL_POINTER : replacement;
        }
        else
        {
            id = client->group_ids[i];
            if (id < 0 || id >= MAX_GROUPS || (uint64)id == group_id || (uint64)id == g_guest_group_id || g_groups_array[id].is_existing == FALSE)
            {
                continue;
            }
            group = &g_groups_array[id].permissions;
        }
        if (group == NULL_POINTER)
        {
            continue;
        }
        group_count++;
        _permissions_internal__combine(result, group);
    }
    if (group_count == 0 && g_guest_group_id < MAX_GROUPS && g_groups_array[g_guest_group_id].is_existing == TRUE)
    {
        group = group_id == g_guest_group_id ? replacement : &g_groups_array[g_guest_group_id].permissions;
        _permissions_internal__combine(result, group);
    }
}

/**
 * @brief Combines the client's current groups while the clients and groups locks are held.
 * @param client -> client whose permissions are read
 * @param result -> combined permissions
 */
static void _permissions_internal__resolve(const client_t *client, group_permissions_t *result)
{
    _permissions_internal__resolve_change(client, MAX_GROUPS, NULL_POINTER, result);
}

/**
 * @brief Adds the active channel group unless a server permission has Skip enabled.
 * @attention Caller holds the clients and groups locks.
 */
static void _permissions_internal__combine_channel(group_permissions_t *result, uint64 role)
{
    uint64 i = 0;
    const group_permissions_t *local = NULL_POINTER;

    if (!g_server_settings.is_permission_system_enabled || role == CHANNEL_GROUP_MEMBER || role >= CHANNEL_GROUP_LIMIT || !g_channel_groups[role].is_existing)
    {
        return;
    }
    local = &g_channel_groups[role].permissions;
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (channel_groups__supports((permission_t)i) && !(result->flags[i] & PERMISSION_FLAG_SKIP))
        {
            result->values[i] = _permissions_internal__action_value((permission_t)i, result->values[i]);
            _permissions_internal__combine_value(&result->values[i], &result->flags[i], _permissions_internal__action_value((permission_t)i, local->values[i]), local->flags[i]);
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (channel_groups__power_name((permission_power_t)i) != NULL_POINTER && !(result->power_flags[i] & PERMISSION_FLAG_SKIP))
        {
            _permissions_internal__combine_value(&result->powers[i], &result->power_flags[i], local->powers[i], local->power_flags[i]);
        }
    }
}

/**
 * @brief Saves current group powers so audio and video packets need no group lookup.
 * @param client Client to update with the clients write lock and groups lock held.
 */
void permissions__refresh_client_powers(client_t *client)
{
    group_permissions_t effective;

    if (client == NULL_POINTER)
    {
        return;
    }
    _permissions_internal__resolve(client, &effective);
    if (!client->is_idle)
    {
        _permissions_internal__combine_channel(&effective, client->channel_group_role);
    }
    client->talk_power = client->is_admin ? PERMISSION_POWER_MAX : effective.powers[PERMISSION_TALK_POWER];
    client->channel_message_power = client->is_admin ? PERMISSION_POWER_MAX : effective.powers[PERMISSION_CHANNEL_MESSAGE_POWER];
    client->channel_picture_power = client->is_admin ? PERMISSION_POWER_MAX : effective.powers[PERMISSION_CHANNEL_PICTURE_POWER];
    client->stream_power = client->is_admin ? PERMISSION_POWER_MAX : effective.powers[PERMISSION_STREAM_POWER];

    // Channel groups take no part in these three values: they describe channels the client is not in.
    _permissions_internal__resolve(client, &effective);
    client->channel_view_rights.is_admin = client->is_admin;
    client->channel_view_rights.is_viewing_members_allowed = (boole)(_permissions_internal__action_value(PERMISSION_VIEW_CHANNEL_MEMBERS, effective.values[PERMISSION_VIEW_CHANNEL_MEMBERS]) == PERMISSION_GRANT);
    client->channel_view_rights.is_viewing_descriptions_allowed = (boole)(_permissions_internal__action_value(PERMISSION_VIEW_CHANNEL_DESCRIPTIONS, effective.values[PERMISSION_VIEW_CHANNEL_DESCRIPTIONS]) == PERMISSION_GRANT);
    client->channel_view_rights.view_power = effective.powers[PERMISSION_CHANNEL_VIEW_POWER];
    client->channel_view_rights.channel_id = client->channel_id;
}

/**
 * @brief Updates connected clients after group permissions change.
 * @attention The caller holds the clients write lock and groups lock.
 */
void permissions__refresh_all_client_powers(void)
{
    uint64 i = 0;

    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == TRUE && g_clients_array[i].is_authenticated == TRUE)
        {
            permissions__refresh_client_powers(&g_clients_array[i]);
        }
    }
}

/**
 * @brief Compares a client's saved power with the channel's current requirement.
 * @param client Client sending audio, video, text or a picture.
 * @param channel Channel receiving the content.
 * @param power Action to check while the clients and channels locks are held.
 */
boole permissions__can_use_channel(const client_t *client, const channel_t *channel, permission_power_t power)
{
    uint64 available = 0;
    uint64 required = 0;
    uint64 now = 0;

    if (client == NULL_POINTER || channel == NULL_POINTER)
    {
        return FALSE;
    }
    if (client->is_existing == FALSE ||
        client->is_authenticated == FALSE ||
        channel->is_existing == FALSE ||
        client->is_idle != FALSE ||
        client->channel_id != channel->channel_id)
    {
        return FALSE;
    }
    if (power == PERMISSION_TALK_POWER && client->muted_until_ms != 0)
    {
        if (util__get_monotonic_ms(&now) == TRUE)
        {
            if (now < client->muted_until_ms)
            {
                return FALSE;
            }
        }
    }
    switch (power)
    {
    case PERMISSION_TALK_POWER:
        available = client->talk_power;
        required = channel->required_talk_power;
        break;
    case PERMISSION_CHANNEL_MESSAGE_POWER:
        available = client->channel_message_power;
        required = channel->required_message_power;
        break;
    case PERMISSION_CHANNEL_PICTURE_POWER:
        available = client->channel_picture_power;
        required = channel->required_picture_power;
        break;
    case PERMISSION_STREAM_POWER:
        available = client->stream_power;
        required = channel->required_stream_power;
        break;
    case PERMISSION_CHANNEL_FILE_POWER:
        // Files are sent rarely, so this power is calculated here instead of being saved in the client.
        available = permissions__power_in_channel(client, channel, PERMISSION_CHANNEL_FILE_POWER);
        required = channel->required_file_power;
        break;
    default:
        return FALSE;
    }
    if (g_server_settings.is_permission_system_enabled == FALSE || client->is_admin == TRUE)
    {
        return TRUE;
    }
    return (boole)(available >= required);
}

/**
 * @brief Checks whether one client may send a direct message, picture, file or poke to another.
 *
 *        Two things are checked. The sender's groups must enable the action. The sender's power
 *        for this kind of content must also be at least as high as the protection that the
 *        receiver's groups give against it. A receiver with protection 0 can be reached by
 *        everybody whose action is enabled.
 *
 * @param sender Client making the request, with the clients lock held.
 * @param target Client that would receive the content.
 * @param permission PERMISSION_SEND_DIRECT_MESSAGES, PERMISSION_SEND_DIRECT_PICTURES, PERMISSION_SEND_DIRECT_FILES or PERMISSION_SEND_POKES.
 *
 * @return NULL when the request is allowed; otherwise the name of the permission or power that refused it.
 */
cstring permissions__contact_refusal(const client_t *sender, const client_t *target, permission_t permission)
{
    group_permissions_t actor;
    group_permissions_t protected_client;
    permission_power_t power = PERMISSION_POWER_COUNT;
    cstring refusal = NULL_POINTER;

    // Choose power pair.
    switch (permission)
    {
    case PERMISSION_SEND_DIRECT_MESSAGES:
        power = PERMISSION_DIRECT_MESSAGE_POWER;
        break;
    case PERMISSION_SEND_DIRECT_PICTURES:
        power = PERMISSION_DIRECT_PICTURE_POWER;
        break;
    case PERMISSION_SEND_DIRECT_FILES:
        power = PERMISSION_DIRECT_FILE_POWER;
        break;
    case PERMISSION_SEND_POKES:
        power = PERMISSION_POKE_POWER;
        break;
    default:
        return "";
    }

    // Check both clients.
    if (sender == NULL_POINTER || target == NULL_POINTER || sender->is_existing == FALSE || sender->is_authenticated == FALSE || sender->is_music_bot == TRUE)
    {
        return g_permission_names[permission];
    }

    // No limits apply.
    if (g_server_settings.is_permission_system_enabled == FALSE || sender->is_admin == TRUE)
    {
        return NULL_POINTER;
    }

    // Compare with protection.
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(sender, &actor);
    _permissions_internal__resolve(target, &protected_client);
    if (_permissions_internal__is_allowed(sender, &actor, permission) == FALSE)
    {
        refusal = g_permission_names[permission];
    }
    else if (actor.powers[power] < protected_client.powers[power + 1])
    {
        refusal = g_permission_power_names[power];
    }
    clib__unlock(&g_groups_global_rwlock_guard);
    return refusal;
}

/**
 * @brief Checks whether a client may create a channel directly inside another channel.
 *
 * @param client Client making the request, with the clients and channels locks held.
 * @param parent Channel that would contain the new channel.
 *
 * @return TRUE when the client's channel create power reaches the parent's requirement.
 */
boole permissions__can_create_channel_in(const client_t *client, const channel_t *parent)
{
    if (client == NULL_POINTER || parent == NULL_POINTER || parent->is_existing == FALSE)
    {
        return FALSE;
    }
    if (g_server_settings.is_permission_system_enabled == FALSE || client->is_admin == TRUE)
    {
        return TRUE;
    }
    return (boole)(permissions__power_in_channel(client, parent, PERMISSION_CHANNEL_CREATE_POWER) >= parent->required_create_power);
}

/**
 * @brief Applies group choices or the existing guest rules when no group makes a choice.
 * @param client -> client making the request
 * @param result -> combined group permissions
 * @param permission -> action to check
 */
static boole _permissions_internal__is_allowed(const client_t *client, const group_permissions_t *result, permission_t permission)
{
    if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
    {
        return FALSE;
    }
    if (client->is_admin == TRUE)
    {
        return TRUE;
    }
    return (boole)(_permissions_internal__action_value(permission, result->values[permission]) == PERMISSION_GRANT);
}

/**
 * @brief Checks a permission using current groups. The caller holds the clients lock.
 * @param client -> client making the request
 * @param permission -> action to check
 */
boole permissions__is_allowed(const client_t *client, permission_t permission)
{
    group_permissions_t result;
    boole allowed = FALSE;

    if (client == NULL_POINTER || permission < 0 || permission >= PERMISSION_COUNT)
    {
        return FALSE;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(client, &result);
    allowed = _permissions_internal__is_allowed(client, &result, permission);
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}

/**
 * @brief Resolves an action for one channel without changing server-wide rights.
 */
boole permissions__is_allowed_in_channel(const client_t *client, const channel_t *channel, permission_t permission)
{
    group_permissions_t effective;
    boole allowed = FALSE;

    if (client == NULL_POINTER || channel == NULL_POINTER || !channel->is_existing || permission < 0 || permission >= PERMISSION_COUNT)
    {
        return FALSE;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(client, &effective);
    _permissions_internal__combine_channel(&effective, channel_groups__role(client, channel));
    allowed = _permissions_internal__is_allowed(client, &effective, permission);
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}

/**
 * @brief Checks entry using the identity's assignment in the destination channel only.
 * @attention Caller holds the clients and channels locks. Other channel rights still require presence.
 */
boole permissions__can_bypass_channel_password(const client_t *client, const channel_t *channel)
{
    group_permissions_t effective;
    boole allowed = FALSE;

    if (client == NULL_POINTER || channel == NULL_POINTER || !channel->is_existing || !client->is_existing || !client->is_authenticated || client->is_music_bot || (!g_server_settings.is_permission_system_enabled && !client->is_admin))
    {
        return FALSE;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(client, &effective);
    _permissions_internal__combine_channel(&effective, channel_groups__assigned_role(client, channel));
    allowed = _permissions_internal__is_allowed(client, &effective, PERMISSION_BYPASS_CHANNEL_PASSWORD);
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}

/**
 * @brief Resolves a power for one channel, including restrictions and Skip.
 */
uint64 permissions__power_in_channel(const client_t *client, const channel_t *channel, permission_power_t power)
{
    group_permissions_t effective;

    if (client == NULL_POINTER || !client->is_existing || !client->is_authenticated || channel == NULL_POINTER || !channel->is_existing || power < 0 || power >= PERMISSION_POWER_COUNT)
    {
        return 0;
    }
    if (client->is_admin)
    {
        return PERMISSION_POWER_MAX;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(client, &effective);
    _permissions_internal__combine_channel(&effective, channel_groups__role(client, channel));
    clib__unlock(&g_groups_global_rwlock_guard);
    return effective.powers[power];
}

/**
 * @brief Reads optional flags, rejecting unsupported scopes, names and malformed values.
 */
boole permissions__read_flags(cJSON *object, group_permissions_t *permissions, boole channel)
{
    cJSON *entry = NULL_POINTER;
    cJSON *negate = NULL_POINTER;
    cJSON *skip = NULL_POINTER;
    uint64 *destination = NULL_POINTER;
    cstring name = NULL_POINTER;
    uint64 i = 0;
    uint64 known = 0;
    boole can_skip = FALSE;

    if (!cJSON_IsObject(object))
    {
        return FALSE;
    }
    cJSON_ArrayForEach(entry, object)
    {
        destination = NULL_POINTER;
        can_skip = FALSE;
        if (!entry->string || cJSON_GetObjectItemCaseSensitive(object, entry->string) != entry || !cJSON_IsObject(entry))
        {
            return FALSE;
        }
        for (i = 0; i < PERMISSION_COUNT; i++)
        {
            if (!clib__strcmp(entry->string, g_permission_names[i]) && (!channel || channel_groups__supports((permission_t)i)))
            {
                if (permissions->values[i] == PERMISSION_DEFAULT)
                {
                    return FALSE;
                }
                destination = &permissions->flags[i];
                can_skip = !channel && channel_groups__supports((permission_t)i);
                break;
            }
        }
        for (i = 0; destination == NULL_POINTER && i < PERMISSION_POWER_COUNT; i++)
        {
            name = channel ? channel_groups__power_name((permission_power_t)i) : g_permission_power_names[i];
            if (name && !clib__strcmp(entry->string, name))
            {
                destination = &permissions->power_flags[i];
                can_skip = !channel && channel_groups__power_name((permission_power_t)i) != NULL_POINTER;
                break;
            }
        }
        negate = cJSON_GetObjectItemCaseSensitive(entry, "negate");
        skip = cJSON_GetObjectItemCaseSensitive(entry, "skip");
        known = (negate != NULL_POINTER) + (skip != NULL_POINTER);
        if (destination == NULL_POINTER || known != (uint64)cJSON_GetArraySize(entry) || (negate && !cJSON_IsBool(negate)) || (skip && (!can_skip || !cJSON_IsBool(skip))))
        {
            return FALSE;
        }
        *destination = (cJSON_IsTrue(negate) ? PERMISSION_FLAG_NEGATE : 0) | (cJSON_IsTrue(skip) ? PERMISSION_FLAG_SKIP : 0);
    }
    return TRUE;
}

/**
 * @brief Writes only active flags, keeping older unflagged settings unchanged.
 */
boole permissions__write_flags(cJSON *object, const group_permissions_t *permissions, boole channel)
{
    cJSON *flags = cJSON_CreateObject();
    cJSON *entry = NULL_POINTER;
    cstring name = NULL_POINTER;
    uint64 value = 0;
    uint64 i = 0;

    if (flags == NULL_POINTER)
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT + PERMISSION_POWER_COUNT; i++)
    {
        if (i < PERMISSION_COUNT)
        {
            name = !channel || channel_groups__supports((permission_t)i) ? g_permission_names[i] : NULL_POINTER;
            value = permissions->flags[i];
        }
        else
        {
            name = channel ? channel_groups__power_name((permission_power_t)(i - PERMISSION_COUNT)) : g_permission_power_names[i - PERMISSION_COUNT];
            value = permissions->power_flags[i - PERMISSION_COUNT];
        }
        if (name == NULL_POINTER || value == 0)
        {
            continue;
        }
        entry = cJSON_CreateObject();
        if (!util__json_add_owned_object(flags, name, entry))
        {
            goto json_failure;
        }
        if (value & PERMISSION_FLAG_NEGATE)
        {
            if (!cJSON_AddBoolToObject(entry, "negate", TRUE))
            {
                goto json_failure;
            }
        }
        if (!channel && (value & PERMISSION_FLAG_SKIP))
        {
            if (!cJSON_AddBoolToObject(entry, "skip", TRUE))
            {
                goto json_failure;
            }
        }
    }
    if (cJSON_GetArraySize(flags))
    {
        return util__json_add_owned_object(object, "flags", flags);
    }
    else
    {
        cJSON_Delete(flags);
    }
    return TRUE;

json_failure:
    util__json_delete_secure(flags);
    return FALSE;
}

/**
 * @brief Identifies groups whose restrictions may only be managed by a server admin.
 */
boole permissions__has_flags(const group_permissions_t *permissions)
{
    uint64 i = 0;

    if (permissions == NULL_POINTER)
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (permissions->flags[i])
        {
            return TRUE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (permissions->power_flags[i])
        {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Checks kick or ban rights and target protection. The caller holds the clients lock.
 * @param sender -> client requesting the kick or ban
 * @param target -> client affected by the request
 * @param permission -> kick or ban permission
 */
boole permissions__can_moderate(const client_t *sender, const client_t *target, permission_t permission)
{
    return permissions__can_moderate_in_channel(sender, target, NULL_POINTER, permission);
}

/**
 * @brief Compares moderation powers including rights limited to the affected channel.
 * @param sender Authenticated actor with the clients lock held.
 * @param target Authenticated client affected by the request.
 * @param permission Requested moderation action.
 * @param channel Affected channel, or null for server moderation.
 */
boole permissions__can_moderate_in_channel(const client_t *sender, const client_t *target, const channel_t *channel, permission_t permission)
{
    group_permissions_t actor;
    group_permissions_t protected_client;
    uint64 power = 0;
    boole allowed = FALSE;

    if (sender == NULL_POINTER || target == NULL_POINTER)
    {
        return FALSE;
    }
    if (sender->is_existing == FALSE ||
        sender->is_authenticated == FALSE ||
        sender->is_music_bot != FALSE ||
        target->is_existing == FALSE ||
        target->is_authenticated == FALSE ||
        target->is_music_bot != FALSE)
    {
        return FALSE;
    }
    switch (permission)
    {
    case PERMISSION_KICK:
        power = PERMISSION_KICK_POWER;
        break;
    case PERMISSION_BAN:
        power = PERMISSION_BAN_POWER;
        break;
    case PERMISSION_MOVE_CLIENTS:
    case PERMISSION_KICK_FROM_CHANNEL:
        power = PERMISSION_MOVE_POWER;
        break;
    case PERMISSION_MUTE_CLIENTS:
    case PERMISSION_STOP_CLIENT_STREAMS:
        power = PERMISSION_MODERATION_POWER;
        break;
    default:
        return FALSE;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(sender, &actor);
    _permissions_internal__resolve(target, &protected_client);
    if (channel != NULL_POINTER)
    {
        _permissions_internal__combine_channel(&actor, channel_groups__role(sender, channel));
        _permissions_internal__combine_channel(&protected_client, channel_groups__role(target, channel));
    }
    if (_permissions_internal__is_allowed(sender, &actor, permission) == TRUE)
    {
        if (sender->is_admin == TRUE || target->is_admin == FALSE && actor.powers[power] >= protected_client.powers[power + 1])
        {
            allowed = TRUE;
        }
    }
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}

/**
 * @brief Initializes per-setting edit thresholds for new and legacy definitions.
 */
void permissions__init_grants(group_permissions_t *permissions)
{
    uint64 i = 0;

    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        permissions->grants[i] = PERMISSION_POWER_MAX;
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        permissions->power_grants[i] = PERMISSION_POWER_MAX;
    }
}

/**
 * @brief Reads per-setting edit thresholds. Missing entries retain the safe default of 1000.
 */
boole permissions__read_grants(cJSON *object, group_permissions_t *permissions, boole channel)
{
    cJSON *entry = NULL_POINTER;
    uint64 i = 0;
    uint64 *destination = NULL_POINTER;
    cstring name = NULL_POINTER;

    if (!cJSON_IsObject(object))
    {
        return FALSE;
    }
    cJSON_ArrayForEach(entry, object)
    {
        destination = NULL_POINTER;
        if (!entry->string || cJSON_GetObjectItemCaseSensitive(object, entry->string) != entry || !cJSON_IsNumber(entry) || !(entry->valuedouble >= 0 && entry->valuedouble <= PERMISSION_POWER_MAX) || entry->valuedouble != entry->valueint)
        {
            return FALSE;
        }
        for (i = 0; i < PERMISSION_COUNT; i++)
        {
            if ((!channel || channel_groups__supports((permission_t)i)) && !clib__strcmp(entry->string, g_permission_names[i]))
            {
                destination = &permissions->grants[i];
                break;
            }
        }
        for (i = 0; destination == NULL_POINTER && i < PERMISSION_POWER_COUNT; i++)
        {
            name = channel ? channel_groups__power_name((permission_power_t)i) : g_permission_power_names[i];
            if (name != NULL_POINTER && !clib__strcmp(entry->string, name))
            {
                destination = &permissions->power_grants[i];
            }
        }
        if (destination == NULL_POINTER)
        {
            return FALSE;
        }
        *destination = (uint64)entry->valueint;
    }
    return TRUE;
}

/**
 * @brief Saves Grant thresholds with the group definition, including channel-specific names.
 */
boole permissions__write_grants(cJSON *object, const group_permissions_t *permissions, boole channel)
{
    cJSON *grants = cJSON_CreateObject();
    uint64 i = 0;
    cstring name = NULL_POINTER;

    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (!channel || channel_groups__supports((permission_t)i))
        {
            if (!cJSON_AddNumberToObject(grants, g_permission_names[i], permissions->grants[i]))
            {
                goto json_failure;
            }
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        name = channel ? channel_groups__power_name((permission_power_t)i) : g_permission_power_names[i];
        if (name != NULL_POINTER)
        {
            if (!cJSON_AddNumberToObject(grants, name, permissions->power_grants[i]))
            {
                goto json_failure;
            }
        }
    }
    return util__json_add_owned_object(object, "grants", grants);

json_failure:
    util__json_delete_secure(grants);
    return FALSE;
}

/**
 * @brief Checks changed rows against their SAVED thresholds, never a proposed lower threshold.
 *        Grant changes must also stay within the editor's power. Creation keeps default locks
 *        unless the creator has power 1000. Group and privilege checks remain separate.
 */
boole permissions__can_edit_values(uint64 edit_power, const group_permissions_t *previous, const group_permissions_t *proposed, boole channel)
{
    uint64 i = 0;
    uint64 before = 0;
    uint64 after = 0;

    if (proposed == NULL_POINTER)
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (channel && !channel_groups__supports((permission_t)i))
        {
            continue;
        }
        if (previous == NULL_POINTER)
        {
            if (proposed->grants[i] != PERMISSION_POWER_MAX && edit_power < PERMISSION_POWER_MAX)
            {
                return FALSE;
            }
            continue;
        }
        before = channel ? previous->values[i] : _permissions_internal__action_value((permission_t)i, previous->values[i]);
        after = channel ? proposed->values[i] : _permissions_internal__action_value((permission_t)i, proposed->values[i]);
        if ((before != after || previous->flags[i] != proposed->flags[i] || previous->grants[i] != proposed->grants[i]) && edit_power < previous->grants[i])
        {
            return FALSE;
        }
        if (previous->grants[i] != proposed->grants[i] && edit_power < proposed->grants[i])
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (channel && channel_groups__power_name((permission_power_t)i) == NULL_POINTER)
        {
            continue;
        }
        if (previous == NULL_POINTER)
        {
            if (proposed->power_grants[i] != PERMISSION_POWER_MAX && edit_power < PERMISSION_POWER_MAX)
            {
                return FALSE;
            }
            continue;
        }
        if ((previous->powers[i] != proposed->powers[i] || previous->power_flags[i] != proposed->power_flags[i] || previous->power_grants[i] != proposed->power_grants[i]) && edit_power < previous->power_grants[i])
        {
            return FALSE;
        }
        if (previous->power_grants[i] != proposed->power_grants[i] && edit_power < proposed->power_grants[i])
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Reads saved or proposed permissions without changing the output on invalid input.
 */
boole permissions__read(cJSON *object, group_permissions_t *permissions)
{
    group_permissions_t parsed;
    cJSON *value = NULL_POINTER;
    uint64 i = 0;
    uint64 known = 0;
    uint64 legacy_icons = PERMISSION_DEFAULT;

    clib__memset(&parsed, 0, sizeof(parsed));
    permissions__init_grants(&parsed);
    for (i = 0; i < sizeof(g_permission_usage_powers) / sizeof(g_permission_usage_powers[0]); i++)
    {
        parsed.powers[g_permission_usage_powers[i]] = 25;
    }
    if (cJSON_IsObject(object) == FALSE)
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        value = cJSON_GetObjectItemCaseSensitive(object, g_permission_names[i]);
        if (value == NULL_POINTER)
        {
            continue;
        }
        known++;
        if (cJSON_IsString(value) == FALSE)
        {
            return FALSE;
        }
        if (clib__strcmp(value->valuestring, "grant") == 0)
        {
            parsed.values[i] = PERMISSION_GRANT;
        }
        else if (clib__strcmp(value->valuestring, "deny") == 0)
        {
            parsed.values[i] = PERMISSION_DENY;
        }
        else if (clib__strcmp(value->valuestring, "default") != 0)
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        value = cJSON_GetObjectItemCaseSensitive(object, g_permission_power_names[i]);
        if (value == NULL_POINTER)
        {
            continue;
        }
        known++;
        if (cJSON_IsNumber(value) == FALSE || !(value->valuedouble >= 0 && value->valuedouble <= PERMISSION_POWER_MAX) || value->valuedouble != (double)value->valueint)
        {
            return FALSE;
        }
        parsed.powers[i] = (uint64)value->valueint;
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "grants");
    if (value != NULL_POINTER)
    {
        known++;
        if (!permissions__read_grants(value, &parsed, FALSE))
        {
            return FALSE;
        }
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "manage_icons");
    if (value != NULL_POINTER)
    {
        if (cJSON_IsString(value) == FALSE)
        {
            return FALSE;
        }
        if (clib__strcmp(value->valuestring, "grant") == 0)
        {
            legacy_icons = PERMISSION_GRANT;
        }
        else if (clib__strcmp(value->valuestring, "deny") == 0)
        {
            legacy_icons = PERMISSION_DENY;
        }
        else if (clib__strcmp(value->valuestring, "default") != 0)
        {
            return FALSE;
        }
        if (cJSON_GetObjectItemCaseSensitive(object, "upload_icons") == NULL_POINTER)
        {
            parsed.values[PERMISSION_UPLOAD_ICONS] = legacy_icons;
        }
        if (cJSON_GetObjectItemCaseSensitive(object, "delete_icons") == NULL_POINTER)
        {
            parsed.values[PERMISSION_DELETE_ICONS] = legacy_icons;
        }
        known++;
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "flags");
    if (value != NULL_POINTER)
    {
        known++;
        if (!permissions__read_flags(value, &parsed, FALSE))
        {
            return FALSE;
        }
    }
    if (known != (uint64)cJSON_GetArraySize(object))
    {
        return FALSE;
    }
    *permissions = parsed;
    return TRUE;
}

/**
 * @brief Returns the permission that an upload of the declared attachment type needs.
 *
 * @param intent Attachment type announced before the first upload part.
 *
 * @return The permission, or PERMISSION_COUNT for an unknown type.
 */
permission_t permissions__upload_permission(cstring intent)
{
    // Check the argument.
    if (intent == NULL_POINTER)
    {
        return PERMISSION_COUNT;
    }

    // Match the intent.
    if (clib__strcmp(intent, "direct_chat_picture_file") == 0)
    {
        return PERMISSION_SEND_DIRECT_PICTURES;
    }
    if (clib__strcmp(intent, "direct_chat_file") == 0)
    {
        return PERMISSION_SEND_DIRECT_FILES;
    }
    if (clib__strcmp(intent, "channel_chat_picture_file") == 0)
    {
        return PERMISSION_SEND_CHANNEL_PICTURES;
    }
    if (clib__strcmp(intent, "channel_chat_file") == 0)
    {
        return PERMISSION_SEND_CHANNEL_FILES;
    }
    if (clib__strcmp(intent, "musicbot_file") == 0)
    {
        return PERMISSION_UPLOAD_MUSIC_TRACKS;
    }
    return PERMISSION_COUNT;
}

/**
 * @brief Checks the permission for the declared attachment type.
 *
 * @param sender Client uploading data with the clients lock held.
 * @param intent Attachment type announced before the first upload part.
 *
 * @return boole -> TRUE when the client may upload this kind of content
 */
boole permissions__can_upload(const client_t *sender, cstring intent)
{
    return permissions__is_allowed(sender, permissions__upload_permission(intent));
}

/**
 * @brief Writes the same permission fields for saved settings and client messages.
 * @param permissions -> group permissions to write
 */
cJSON *permissions__write(const group_permissions_t *permissions)
{
    cJSON *object = cJSON_CreateObject();
    const char *value = NULL_POINTER;
    uint64 i = 0;

    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        value = permissions->values[i] == PERMISSION_DENY ? "deny" : permissions->values[i] == PERMISSION_GRANT ? "grant" : "default";
        if (!cJSON_AddStringToObject(object, g_permission_names[i], value))
        {
            goto json_failure;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (!cJSON_AddNumberToObject(object, g_permission_power_names[i], permissions->powers[i]))
        {
            goto json_failure;
        }
    }
    if (!permissions__write_flags(object, permissions, FALSE) || !permissions__write_grants(object, permissions, FALSE))
    {
        goto json_failure;
    }
    return object;

json_failure:
    util__json_delete_secure(object);
    return NULL_POINTER;
}

/**
 * @brief Returns the wire name of a known permission.
 *
 * @param permission Action to name.
 */
cstring permissions__name(permission_t permission)
{
    return permission >= 0 && permission < PERMISSION_COUNT ? g_permission_names[permission] : "";
}

/**
 * @brief Returns the wire name of a known power or protection.
 *
 * @param power Power to name.
 *
 * @return cstring -> the name, or an empty text for an unknown power
 */
cstring permissions__power_name(permission_power_t power)
{
    return power >= 0 && power < PERMISSION_POWER_COUNT ? g_permission_power_names[power] : "";
}

/**
 * @brief Reads the highest server group power with the clients lock held.
 * @param client Authenticated client.
 * @param power Numerical permission being checked.
 */
uint64 permissions__power(const client_t *client, permission_power_t power)
{
    group_permissions_t effective;

    if (client == NULL_POINTER || !client->is_existing || !client->is_authenticated || power < 0 || power >= PERMISSION_POWER_COUNT)
    {
        return 0;
    }
    if (client->is_admin)
    {
        return PERMISSION_POWER_MAX;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(client, &effective);
    clib__unlock(&g_groups_global_rwlock_guard);
    return effective.powers[power];
}

/**
 * @brief Checks a permission when the caller already holds the clients and groups locks.
 * @param client -> client making the request
 * @param permission -> action to check
 */
boole permissions__is_allowed_in_groups(const client_t *client, permission_t permission)
{
    group_permissions_t result;

    if (client == NULL_POINTER || permission < 0 || permission >= PERMISSION_COUNT)
    {
        return FALSE;
    }
    _permissions_internal__resolve(client, &result);
    return _permissions_internal__is_allowed(client, &result, permission);
}

/**
 * @brief Finds a group in the client's current memberships.
 * @param client -> client whose memberships are read under the clients lock
 * @param group_id -> group to find
 */
static boole _permissions_internal__has_group(const client_t *client, uint64 group_id)
{
    uint64 i = 0;
    int64 id = 0;

    if (group_id == g_guest_group_id && g_guest_group_id < MAX_GROUPS)
    {
        for (i = 0; i < cvector_size(client->group_ids); i++)
        {
            id = client->group_ids[i];
            if (id >= 0 && id < MAX_GROUPS && (uint64)id != g_guest_group_id && g_groups_array[id].is_existing == TRUE)
            {
                return FALSE;
            }
        }
        return TRUE;
    }

    for (i = 0; i < cvector_size(client->group_ids); i++)
    {
        if ((uint64)client->group_ids[i] == group_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Checks an action against the group's protection with the clients and groups locks held.
 * @param sender -> client making the change
 * @param group_id -> existing group affected by the change
 * @param permission -> required management permission
 */
boole permissions__can_manage_group(const client_t *sender, uint64 group_id, permission_t permission)
{
    group_permissions_t actor;

    if (sender == NULL_POINTER || group_id >= MAX_GROUPS)
    {
        return FALSE;
    }
    if (g_groups_array[group_id].is_existing == FALSE || g_server_settings.is_permission_system_enabled == FALSE)
    {
        return FALSE;
    }

    // The guest group cannot be deleted, assigned or removed manually.
    if (group_id == g_guest_group_id)
    {
        if (permission == PERMISSION_DELETE_GROUPS || permission == PERMISSION_ASSIGN_GROUPS || permission == PERMISSION_REMOVE_GROUPS)
        {
            return FALSE;
        }
    }

    // Combine the sender's group permissions before checking this action.
    _permissions_internal__resolve(sender, &actor);
    if (_permissions_internal__is_allowed(sender, &actor, permission) == FALSE)
    {
        return FALSE;
    }
    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    if (permission != PERMISSION_EDIT_GROUPS && permissions__has_flags(&g_groups_array[group_id].permissions))
    {
        return FALSE;
    }
    if (group_id == ADMIN_GROUP_ID || actor.powers[PERMISSION_GROUP_MANAGEMENT_POWER] < g_groups_array[group_id].permissions.powers[PERMISSION_GROUP_PROTECTION])
    {
        return FALSE;
    }

    // Prevent non-admin users from editing permissions or deleting their own groups.
    if ((permission == PERMISSION_EDIT_GROUP_PERMISSIONS || permission == PERMISSION_DELETE_GROUPS) && _permissions_internal__has_group(sender, group_id) == TRUE)
    {
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Rejects changes that give a user rights or powers the sender does not have.
 * @param sender -> client requesting the change
 * @param target -> affected user with the clients and groups locks held
 * @param group_id -> group being added, removed or edited
 * @param replacement -> new permissions, or NULL when removing the group
 */
static boole _permissions_internal__allows_change(const client_t *sender, const client_t *target, uint64 group_id, const group_permissions_t *replacement)
{
    group_permissions_t actor;
    group_permissions_t before;
    group_permissions_t after;
    uint64 i = 0;

    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    if (target == sender || target->is_admin == TRUE)
    {
        return FALSE;
    }
    if (_permissions_internal__has_group(target, ADMIN_GROUP_ID) == TRUE)
    {
        return FALSE;
    }
    _permissions_internal__resolve(sender, &actor);
    _permissions_internal__resolve(target, &before);
    _permissions_internal__resolve_change(target, group_id, replacement, &after);
    if (actor.powers[PERMISSION_GROUP_MANAGEMENT_POWER] < before.powers[PERMISSION_GROUP_PROTECTION])
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (_permissions_internal__is_allowed(target, &after, (permission_t)i) == FALSE)
        {
            continue;
        }
        if (_permissions_internal__is_allowed(target, &before, (permission_t)i) == TRUE)
        {
            continue;
        }
        if (_permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (after.powers[i] > before.powers[i] && after.powers[i] > actor.powers[i == PERMISSION_GROUP_PROTECTION ? PERMISSION_GROUP_MANAGEMENT_POWER : i])
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Checks an assignment or removal without changing memberships.
 * @param sender -> client making the request
 * @param target -> affected user with the clients and groups locks held
 * @param group_id -> group being assigned or removed
 * @param add -> TRUE for assignment
 */
boole permissions__can_change_member(const client_t *sender, const client_t *target, uint64 group_id, boole add)
{
    group_permissions_t actor;
    const group_permissions_t *assigned = NULL_POINTER;
    uint64 i = 0;
    uint64 limit = 0;

    if (target == NULL_POINTER)
    {
        return FALSE;
    }
    if (target->is_existing == FALSE || target->is_authenticated == FALSE || target->is_music_bot == TRUE)
    {
        return FALSE;
    }
    if (permissions__can_manage_group(sender, group_id, add == TRUE ? PERMISSION_ASSIGN_GROUPS : PERMISSION_REMOVE_GROUPS) == FALSE)
    {
        return FALSE;
    }
    if (add == TRUE && sender->is_admin == FALSE)
    {
        assigned = &g_groups_array[group_id].permissions;
        _permissions_internal__resolve(sender, &actor);
        for (i = 0; i < PERMISSION_COUNT; i++)
        {
            if (assigned->values[i] == PERMISSION_GRANT && _permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
            {
                return FALSE;
            }
        }
        for (i = 0; i < PERMISSION_POWER_COUNT; i++)
        {
            limit = i == PERMISSION_GROUP_PROTECTION ? actor.powers[PERMISSION_GROUP_MANAGEMENT_POWER] : actor.powers[i];
            if (assigned->powers[i] > limit)
            {
                return FALSE;
            }
        }
    }
    return _permissions_internal__allows_change(sender, target, group_id, add == TRUE ? &g_groups_array[group_id].permissions : NULL_POINTER);
}

/**
 * @brief Checks every value before a group edit, including users who already hold the group.
 * @param sender -> client making the edit
 * @param group_id -> group being edited with the clients and groups locks held
 * @param proposed -> complete proposed permissions
 */
boole permissions__can_edit_group_permissions(const client_t *sender, uint64 group_id, const group_permissions_t *proposed)
{
    group_permissions_t actor;
    uint64 i = 0;
    uint64 limit = 0;

    if (sender == NULL_POINTER || proposed == NULL_POINTER || group_id == ADMIN_GROUP_ID)
    {
        return FALSE;
    }
    if (sender->is_admin == FALSE)
    {
        if (permissions__has_flags(proposed) == TRUE)
        {
            return FALSE;
        }
    }
    if (permissions__can_manage_group(sender, group_id, PERMISSION_EDIT_GROUP_PERMISSIONS) == FALSE)
    {
        return FALSE;
    }
    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    _permissions_internal__resolve(sender, &actor);
    if (permissions__can_edit_values(actor.powers[PERMISSION_EDIT_POWER], &g_groups_array[group_id].permissions, proposed, FALSE) == FALSE)
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (_permissions_internal__action_value((permission_t)i, proposed->values[i]) == _permissions_internal__action_value((permission_t)i, g_groups_array[group_id].permissions.values[i]))
        {
            continue;
        }
        if (_permissions_internal__is_allowed(sender, proposed, (permission_t)i) == FALSE)
        {
            continue;
        }
        if (_permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        limit = actor.powers[i];
        if (i == PERMISSION_GROUP_PROTECTION)
        {
            limit = actor.powers[PERMISSION_GROUP_MANAGEMENT_POWER];
        }
        if (proposed->powers[i] != g_groups_array[group_id].permissions.powers[i] && proposed->powers[i] > limit)
        {
            return FALSE;
        }
    }
    return _permissions_internal__allows_group_members_change(sender, group_id, proposed);
}

/**
 * @brief Checks deletion and any Guest permissions that members would receive afterwards.
 * @param sender -> client requesting deletion
 * @param group_id -> group to delete with the clients and groups locks held
 */
boole permissions__can_delete_group(const client_t *sender, uint64 group_id)
{
    if (group_id == ADMIN_GROUP_ID || group_id == g_guest_group_id)
    {
        return FALSE;
    }
    if (permissions__can_manage_group(sender, group_id, PERMISSION_DELETE_GROUPS) == FALSE)
    {
        return FALSE;
    }
    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    return _permissions_internal__allows_group_members_change(sender, group_id, NULL_POINTER);
}

/**
 * @brief Rejects group presets that grant permissions or powers the creator does not have.
 * @param sender Client creating the group with the clients and groups locks held.
 * @param proposed Initial permissions of the new group.
 */
boole permissions__can_create_group(const client_t *sender, const group_permissions_t *proposed)
{
    group_permissions_t actor;
    uint64 i = 0;
    uint64 limit = 0;

    if (sender == NULL_POINTER || proposed == NULL_POINTER || g_server_settings.is_permission_system_enabled == FALSE)
    {
        return FALSE;
    }
    _permissions_internal__resolve(sender, &actor);
    if (_permissions_internal__is_allowed(sender, &actor, PERMISSION_CREATE_GROUPS) == FALSE)
    {
        return FALSE;
    }
    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    if (permissions__can_edit_values(actor.powers[PERMISSION_EDIT_POWER], NULL_POINTER, proposed, FALSE) == FALSE)
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (_permissions_internal__is_allowed(sender, proposed, (permission_t)i) == TRUE && _permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        limit = actor.powers[i == PERMISSION_GROUP_PROTECTION ? PERMISSION_GROUP_MANAGEMENT_POWER : i];
        if (proposed->powers[i] > limit)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Checks group edits against connected users and saved memberships.
 * @param sender -> client making the change with the clients and groups locks held
 * @param group_id -> group being edited or deleted
 * @param proposed -> new permissions, or NULL for deletion
 */
static boole _permissions_internal__allows_group_members_change(const client_t *sender, uint64 group_id, const group_permissions_t *proposed)
{
    client_t stored;
    uint64 c = 0;
    uint64 i = 0;
    boole allowed = TRUE;

    for (c = 0; c < (uint64)g_server_settings.max_client_count; c++)
    {
        if (g_clients_array[c].is_existing == TRUE && g_clients_array[c].is_authenticated == TRUE && _permissions_internal__has_group(&g_clients_array[c], group_id) == TRUE && _permissions_internal__allows_change(sender, &g_clients_array[c], group_id, proposed) == FALSE)
        {
            return FALSE;
        }
    }
    clib__memset(&stored, 0, sizeof(stored));
    stored.is_existing = TRUE;
    stored.is_authenticated = TRUE;
    if (!clib__int_vector_reserve(&stored.group_ids, MAX_GROUPS_FOR_SINGLE_CLIENT))
    {
        return FALSE;
    }
    pthread_mutex_lock(&g_client_stored_data_mutex);
    for (c = 0; c < MAX_CLIENT_STORED_DATA; c++)
    {
        if (g_client_stored_data[c].public_key[0] == 0)
        {
            continue;
        }
        if (g_client_stored_data[c].group_id_count > MAX_GROUPS_FOR_SINGLE_CLIENT)
        {
            allowed = FALSE;
            break;
        }
        cvector_clear(stored.group_ids);
        for (i = 0; i < g_client_stored_data[c].group_id_count; i++)
        {
            cvector_push_back(stored.group_ids, (int)g_client_stored_data[c].group_ids[i]);
        }
        if (_permissions_internal__has_group(&stored, group_id) == TRUE && _permissions_internal__allows_change(sender, &stored, group_id, proposed) == FALSE)
        {
            allowed = FALSE;
            break;
        }
    }
    pthread_mutex_unlock(&g_client_stored_data_mutex);
    cvector_free(stored.group_ids);
    return allowed;
}

/**
 * @brief Writes the current user's rights without revealing other groups' permission values.
 * @param client -> user receiving the result with the clients and groups locks held
 */
cJSON *permissions__write_access(const client_t *client)
{
    cJSON *object = cJSON_CreateObject();
    cJSON *powers = cJSON_CreateObject();
    group_permissions_t effective;
    uint64 i = 0;

    if (object == NULL_POINTER || powers == NULL_POINTER || !cJSON_AddItemToObject(object, "powers", powers))
    {
        cJSON_Delete(powers);
        cJSON_Delete(object);
        return NULL_POINTER;
    }
    _permissions_internal__resolve(client, &effective);
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        cJSON_AddBoolToObject(object, g_permission_names[i], _permissions_internal__is_allowed(client, &effective, (permission_t)i));
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        cJSON_AddNumberToObject(powers, g_permission_power_names[i], client->is_admin == TRUE ? PERMISSION_POWER_MAX : effective.powers[i]);
    }
    cJSON_AddNumberToObject(object, "channel_id", (double)client->channel_id);
    cJSON_AddNumberToObject(object, "channel_group_role", client->channel_group_role);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(powers, "talk_power"), client->talk_power);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(powers, "channel_message_power"), client->channel_message_power);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(powers, "channel_picture_power"), client->channel_picture_power);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(powers, "stream_power"), client->stream_power);
    if (client->is_temp_admin_channel == TRUE)
    {
        cJSON_AddNumberToObject(object, "temp_channel_id", (double)client->temp_channel_id);
    }
    else
    {
        cJSON_AddNullToObject(object, "temp_channel_id");
    }
    return object;
}

/**
 * @brief Writes which group controls this user may use. Requests are checked again when received.
 * @param client -> user receiving the controls with the clients and groups locks held
 * @param group_id -> group displayed in the window
 */
cJSON *permissions__write_group_access(const client_t *client, uint64 group_id)
{
    cJSON *object = cJSON_CreateObject();
    permission_t actions[] = { PERMISSION_EDIT_GROUPS, PERMISSION_DELETE_GROUPS, PERMISSION_ASSIGN_GROUPS, PERMISSION_REMOVE_GROUPS, PERMISSION_EDIT_GROUP_PERMISSIONS };
    uint64 i = 0;

    for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
    {
        cJSON_AddBoolToObject(object, g_permission_names[actions[i]], permissions__can_manage_group(client, group_id, actions[i]));
    }
    return object;
}

/**
 * @brief Checks a saved user's groups while the clients, groups and identity store locks are held.
 * @param stored Saved identity whose permission is checked.
 * @param permission Action to check.
 */
boole permissions__is_stored_client_allowed(const client_stored_data_t *stored, permission_t permission)
{
    client_t client;
    uint64 i = 0;
    boole allowed = FALSE;

    clib__memset(&client, 0, sizeof(client));
    if (stored == NULL_POINTER || stored->public_key[0] == 0)
    {
        return FALSE;
    }
    if (stored->group_id_count > MAX_GROUPS_FOR_SINGLE_CLIENT || !clib__int_vector_reserve(&client.group_ids, MAX_GROUPS_FOR_SINGLE_CLIENT))
    {
        return FALSE;
    }
    client.is_existing = TRUE;
    client.is_authenticated = TRUE;
    for (i = 0; i < stored->group_id_count && i < MAX_GROUPS_FOR_SINGLE_CLIENT; i++)
    {
        cvector_push_back(client.group_ids, (int)stored->group_ids[i]);
        if (g_server_settings.is_permission_system_enabled == TRUE && stored->group_ids[i] == 0)
        {
            client.is_admin = TRUE;
        }
    }
    allowed = permissions__is_allowed_in_groups(&client, permission);
    cvector_free(client.group_ids);
    return allowed;
}

/**
 * @brief Checks a saved identity's permission while the caller holds the clients lock.
 * @param identity_hash Saved identity to find.
 * @param permission Action to check.
 */
boole permissions__is_identity_allowed(const char *identity_hash, permission_t permission)
{
    uint64 i = 0;
    boole allowed = FALSE;

    clib__read_lock(&g_groups_global_rwlock_guard);
    pthread_mutex_lock(&g_client_stored_data_mutex);
    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__strcmp(g_client_stored_data[i].public_key, identity_hash) == 0)
        {
            allowed = permissions__is_stored_client_allowed(&g_client_stored_data[i], permission);
            break;
        }
    }
    pthread_mutex_unlock(&g_client_stored_data_mutex);
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}
