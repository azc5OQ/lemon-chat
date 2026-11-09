#include "../clib/clib.h"
// This file stores channel group assignments and checks their rights within one channel.
#include "channel_groups.h"
#include "permissions.h"
#include "../base.h"
#include "../util.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include <string.h>

channel_group_t g_channel_groups[CHANNEL_GROUP_LIMIT];

static boole _channel_groups_internal__append_member(channel_t *channel, const channel_group_member_t *member);

static cstring g_channel_group_power_names[PERMISSION_POWER_COUNT] = { NULL_POINTER, NULL_POINTER, NULL_POINTER, NULL_POINTER, "channel_group_assignment_power", "channel_group_protection", "talk_power", "channel_message_power", "channel_picture_power", "stream_power", "channel_kick_power", "channel_kick_protection", "channel_moderation_power", "channel_moderation_protection", NULL_POINTER };

/**
 * @brief Creates the default Channel Admin group before saved groups are loaded.
 */
void channel_groups__init(void)
{
    uint64 i = 0;

    clib__memset(g_channel_groups, 0, sizeof(g_channel_groups));
    for (i = 0; i < CHANNEL_GROUP_LIMIT; i++)
    {
        permissions__init_grants(&g_channel_groups[i].permissions);
    }
    g_channel_groups[CHANNEL_GROUP_ADMIN].is_existing = TRUE;
    g_channel_groups[CHANNEL_GROUP_ADMIN].icon_id = -2;
    clib__string_copy(g_channel_groups[CHANNEL_GROUP_ADMIN].name, sizeof(g_channel_groups[CHANNEL_GROUP_ADMIN].name), "Channel Admin");
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (channel_groups__supports((permission_t)i))
        {
            g_channel_groups[CHANNEL_GROUP_ADMIN].permissions.values[i] = PERMISSION_GRANT;
        }
    }
    for (i = PERMISSION_GROUP_MANAGEMENT_POWER; i < PERMISSION_POWER_COUNT; i++)
    {
        if (g_channel_group_power_names[i] != NULL_POINTER)
        {
            g_channel_groups[CHANNEL_GROUP_ADMIN].permissions.powers[i] = PERMISSION_POWER_MAX;
        }
    }
}

/**
 * @brief Checks management rights and prevents granting stronger channel permissions.
 * @param actor Requesting client with the clients lock held.
 * @param permission Create, edit or delete channel groups.
 * @param previous Existing definition, or null when creating.
 * @param proposed New definition, or null when deleting.
 */
boole channel_groups__can_edit_definition(const client_t *actor, permission_t permission, const channel_group_t *previous, const channel_group_t *proposed)
{
    uint64 i = 0;
    uint64 management = 0;
    uint64 limit = 0;

    if (permissions__is_allowed(actor, permission) == FALSE)
    {
        return FALSE;
    }
    if (actor->is_admin)
    {
        return TRUE;
    }
    management = permissions__power(actor, PERMISSION_GROUP_MANAGEMENT_POWER);
    if ((previous && permissions__has_flags(&previous->permissions)) || (proposed && permissions__has_flags(&proposed->permissions)))
    {
        return FALSE;
    }
    if (previous != NULL_POINTER && previous->permissions.powers[PERMISSION_GROUP_PROTECTION] > management)
    {
        return FALSE;
    }
    if (proposed == NULL_POINTER)
    {
        return TRUE;
    }
    if (!permissions__can_edit_values(permissions__power(actor, PERMISSION_EDIT_POWER), previous ? &previous->permissions : NULL_POINTER, &proposed->permissions, TRUE))
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if ((!previous || previous->permissions.values[i] != proposed->permissions.values[i]) && proposed->permissions.values[i] == PERMISSION_GRANT && !permissions__is_allowed(actor, (permission_t)i))
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        limit = i == PERMISSION_GROUP_PROTECTION ? management : permissions__power(actor, (permission_power_t)i);
        if ((!previous || previous->permissions.powers[i] != proposed->permissions.powers[i]) && proposed->permissions.powers[i] > limit)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Returns this identity's group in the selected channel.
 * @param client Client whose identity is checked with the clients lock held.
 * @param channel Channel being read with the channels lock held.
 */
uint64 channel_groups__role(const client_t *client, const channel_t *channel)
{
    if (client == NULL_POINTER || channel == NULL_POINTER || client->is_idle || client->channel_id != channel->channel_id)
    {
        return CHANNEL_GROUP_MEMBER;
    }
    return channel_groups__assigned_role(client, channel);
}

/** Looks up saved identity membership solely for entry checks; it grants no rights outside that channel. */
uint64 channel_groups__assigned_role(const client_t *client, const channel_t *channel)
{
    uint64 i = 0;
    char identity[IDENTITY_HASH_MAX_LENGTH] = { 0 };

    if (!g_server_settings.is_permission_system_enabled || channel == NULL_POINTER || client == NULL_POINTER)
    {
        return CHANNEL_GROUP_MEMBER;
    }

    if (client->is_temp_admin_channel == TRUE && client->temp_channel_id == channel->channel_id && channel->is_temp_channel == TRUE)
    {
        return CHANNEL_GROUP_ADMIN;
    }
    util__hash_password_to_base64(client->public_key, identity, sizeof(identity));
    for (i = 0; i < cvector_size(channel->group_members); i++)
    {
        if (clib__strcmp(channel->group_members[i].identity, identity) == 0)
        {
            return channel->group_members[i].role < CHANNEL_GROUP_LIMIT && g_channel_groups[channel->group_members[i].role].is_existing ? channel->group_members[i].role : CHANNEL_GROUP_MEMBER;
        }
    }
    return CHANNEL_GROUP_MEMBER;
}

/**
 * @brief Identifies actions that can be granted within one channel.
 * @param permission Action being checked.
 */
boole channel_groups__supports(permission_t permission)
{
    switch (permission)
    {
    case PERMISSION_KICK_FROM_CHANNEL:
    case PERMISSION_MUTE_CLIENTS:
    case PERMISSION_STOP_CLIENT_STREAMS:
    case PERMISSION_EDIT_CHANNEL_DESCRIPTION:
    case PERMISSION_EDIT_CHANNEL_NAME:
    case PERMISSION_EDIT_CHANNEL_PASSWORD:
    case PERMISSION_EDIT_CHANNEL_POWERS:
    case PERMISSION_EDIT_CHANNEL_OPTIONS:
    case PERMISSION_DELETE_CHANNEL:
    case PERMISSION_ASSIGN_CHANNEL_GROUPS:
    case PERMISSION_BYPASS_CHANNEL_PASSWORD:
        return TRUE;
    default:
        return FALSE;
    }
}

/**
 * @brief Reads only actions and powers that can apply inside a channel.
 * @param object Proposed or saved channel permissions.
 * @param permissions Receives the validated values.
 */
boole channel_groups__read_permissions(cJSON *object, group_permissions_t *permissions)
{
    group_permissions_t parsed = { 0 };
    cJSON *value = NULL_POINTER;
    uint64 i = 0;
    uint64 known = 0;

    permissions__init_grants(&parsed);
    if (!cJSON_IsObject(object))
    {
        return FALSE;
    }
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (!channel_groups__supports((permission_t)i))
        {
            continue;
        }
        parsed.values[i] = PERMISSION_DENY;
        value = cJSON_GetObjectItemCaseSensitive(object, permissions__name((permission_t)i));
        if (value == NULL_POINTER)
        {
            continue;
        }
        known++;
        if (!cJSON_IsString(value) || (clib__strcmp(value->valuestring, "grant") && clib__strcmp(value->valuestring, "deny") && clib__strcmp(value->valuestring, "default")))
        {
            return FALSE;
        }
        parsed.values[i] = !clib__strcmp(value->valuestring, "grant") ? PERMISSION_GRANT : PERMISSION_DENY;
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (g_channel_group_power_names[i] == NULL_POINTER)
        {
            continue;
        }
        value = cJSON_GetObjectItemCaseSensitive(object, g_channel_group_power_names[i]);
        if (value == NULL_POINTER)
        {
            continue;
        }
        known++;
        if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0 && value->valuedouble <= PERMISSION_POWER_MAX) || value->valuedouble != value->valueint)
        {
            return FALSE;
        }
        parsed.powers[i] = (uint64)value->valueint;
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "grants");
    if (value != NULL_POINTER)
    {
        known++;
        if (!permissions__read_grants(value, &parsed, TRUE))
        {
            return FALSE;
        }
    }
    value = cJSON_GetObjectItemCaseSensitive(object, "flags");
    if (value != NULL_POINTER)
    {
        known++;
        if (!permissions__read_flags(value, &parsed, TRUE))
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
 * @brief Writes channel permissions without unrelated server permissions.
 * @param permissions Channel group values to save or send.
 */
cJSON *channel_groups__write_permissions(const group_permissions_t *permissions)
{
    cJSON *object = cJSON_CreateObject();
    uint64 i = 0;

    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (channel_groups__supports((permission_t)i))
        {
            if (!cJSON_AddStringToObject(object, permissions__name((permission_t)i), permissions->values[i] == PERMISSION_GRANT ? "grant" : "deny"))
            {
                goto json_failure;
            }
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (g_channel_group_power_names[i] != NULL_POINTER)
        {
            if (!cJSON_AddNumberToObject(object, g_channel_group_power_names[i], permissions->powers[i]))
            {
                goto json_failure;
            }
        }
    }
    if (!permissions__write_flags(object, permissions, TRUE) || !permissions__write_grants(object, permissions, TRUE))
    {
        goto json_failure;
    }
    return object;

json_failure:
    util__json_delete_secure(object);
    return NULL_POINTER;
}

/**
 * @brief Checks a channel group's grant for an action that can be limited to a channel.
 * @param role Saved channel group ID.
 * @param permission Action being checked.
 */
boole channel_groups__allows(uint64 role, permission_t permission)
{
    return role < CHANNEL_GROUP_LIMIT && g_channel_groups[role].is_existing && channel_groups__supports(permission) && g_channel_groups[role].permissions.values[permission] == PERMISSION_GRANT;
}

/**
 * @brief Reads a channel group's power without searching memberships per media packet.
 * @param group_id Assigned channel group.
 * @param power Numerical permission.
 */
uint64 channel_groups__power(uint64 group_id, permission_power_t power)
{
    return g_server_settings.is_permission_system_enabled && group_id < CHANNEL_GROUP_LIMIT && g_channel_groups[group_id].is_existing && power >= 0 && power < PERMISSION_POWER_COUNT && g_channel_group_power_names[power] != NULL_POINTER ? g_channel_groups[group_id].permissions.powers[power] : 0;
}

/**
 * @brief Returns the channel-scoped wire name, or null for server-only powers.
 */
cstring channel_groups__power_name(permission_power_t power)
{
    return power >= 0 && power < PERMISSION_POWER_COUNT ? g_channel_group_power_names[power] : NULL_POINTER;
}

/**
 * @brief Prevents a channel assignment from granting rights the actor does not hold.
 * @param actor Client making the change with the clients lock held.
 * @param target Client receiving the assignment.
 * @param channel Affected channel with the channels lock held.
 * @param group_id New group, or zero to remove the assignment.
 */
boole channel_groups__can_assign(const client_t *actor, const client_t *target, const channel_t *channel, uint64 group_id)
{
    uint64 target_group = channel_groups__role(target, channel);
    uint64 available = 0;
    uint64 i = 0;

    // Check the group index; zero means removing the channel assignment.
    if (group_id >= CHANNEL_GROUP_LIMIT)
    {
        return FALSE;
    }
    if (group_id != 0)
    {
        if (g_channel_groups[group_id].is_existing == FALSE)
        {
            return FALSE;
        }
    }
    if (channel_groups__is_allowed(actor, channel, PERMISSION_ASSIGN_CHANNEL_GROUPS) == FALSE)
    {
        return FALSE;
    }
    if (actor->is_admin)
    {
        return TRUE;
    }
    if (permissions__has_flags(&g_channel_groups[group_id].permissions) || permissions__has_flags(&g_channel_groups[target_group].permissions))
    {
        return FALSE;
    }
    if (actor == target || target->is_admin)
    {
        return FALSE;
    }

    // Require enough management power to change this user.
    available = permissions__power_in_channel(actor, channel, PERMISSION_GROUP_MANAGEMENT_POWER);
    if (available < permissions__power_in_channel(target, channel, PERMISSION_GROUP_PROTECTION))
    {
        return FALSE;
    }

    // Do not grant an action the sender cannot perform.
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (channel_groups__allows(group_id, (permission_t)i) && !channel_groups__is_allowed(actor, channel, (permission_t)i))
        {
            return FALSE;
        }
    }

    // Do not grant power levels above the sender's own levels.
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        available = permissions__power_in_channel(actor, channel, (permission_power_t)i);
        if (available < channel_groups__power(group_id, (permission_power_t)i))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Combines server rights with rights assigned in this channel.
 * @param client Client making the request with the clients lock held.
 * @param channel Channel being changed with the channels lock held.
 * @param permission Requested action.
 */
boole channel_groups__is_allowed(const client_t *client, const channel_t *channel, permission_t permission)
{
    return permissions__is_allowed_in_channel(client, channel, permission);
}

/**
 * @brief Checks moderation rights and protection in the affected channel.
 * @param actor Client requesting the action with the clients lock held.
 * @param target Client affected by the action.
 * @param channel Target's channel with the channels lock held.
 * @param permission Move, channel kick, mute or stream stop.
 */
boole channel_groups__can_moderate(const client_t *actor, const client_t *target, const channel_t *channel, permission_t permission)
{
    if (actor == NULL_POINTER || target == NULL_POINTER)
    {
        return FALSE;
    }
    if (actor->is_authenticated == FALSE || target->is_authenticated == FALSE || actor->is_existing == FALSE || target->is_existing == FALSE || target->is_music_bot != FALSE || actor == target)
    {
        return FALSE;
    }
    if (actor->is_admin)
    {
        return TRUE;
    }
    if (permission == PERMISSION_MOVE_CLIENTS)
    {
        return permissions__can_moderate(actor, target, permission);
    }
    return permissions__can_moderate_in_channel(actor, target, channel, permission);
}

/**
 * @brief Adds, changes or removes one saved channel assignment.
 * @param channel Channel being changed with the channels write lock held.
 * @param identity Authenticated identity hash.
 * @param role New group, with Member removing the stored assignment.
 */
boole channel_groups__set(channel_t *channel, cstring identity, uint64 role)
{
    uint64 i = 0;
    channel_group_member_t member;

    if (role >= CHANNEL_GROUP_LIMIT || identity == NULL_POINTER)
    {
        return FALSE;
    }
    if (clib__strlen(identity) == 0)
    {
        return FALSE;
    }
    if (clib__strlen(identity) >= sizeof(member.identity))
    {
        return FALSE;
    }
    for (i = 0; i < cvector_size(channel->group_members); i++)
    {
        if (clib__strcmp(channel->group_members[i].identity, identity) == 0)
        {
            if (role == CHANNEL_GROUP_MEMBER && channel->group_members[i].muted_until_ms == 0)
            {
                cvector_erase(channel->group_members, i);
            }
            else
            {
                channel->group_members[i].role = role;
            }
            return TRUE;
        }
    }
    if (role == CHANNEL_GROUP_MEMBER)
    {
        return TRUE;
    }
    if (cvector_size(channel->group_members) >= MAX_SERVER_SLOTS)
    {
        return FALSE;
    }
    clib__memset(&member, 0, sizeof(member));
    clib__memcpy(member.identity, identity, clib__strlen(identity));
    member.role = role;
    if (_channel_groups_internal__append_member(channel, &member) == FALSE)
    {
        clib__null_memory(&member, sizeof(member));
        return FALSE;
    }
    clib__null_memory(&member, sizeof(member));
    return TRUE;
}

/**
 * @brief Updates saved powers when a client enters a channel or changes channel group.
 * @param client Client to update with the clients write lock and channels lock held.
 */
void channel_groups__refresh(client_t *client)
{
    clib__read_lock(&g_groups_global_rwlock_guard);
    channel_groups__refresh_locked(client);
    clib__unlock(&g_groups_global_rwlock_guard);
}

/**
 * @brief Loads channel rights and an unexpired mute into the connected client.
 * @param client Client to update with clients, channels and groups locks held.
 */
void channel_groups__refresh_locked(client_t *client)
{
    uint64 i = 0;
    uint64 now = 0;
    char identity[IDENTITY_HASH_MAX_LENGTH] = { 0 };

    if (client == NULL_POINTER)
    {
        return;
    }
    if (client->channel_id >= g_server_settings.max_channel_count || client->is_idle)
    {
        client->channel_group_role = CHANNEL_GROUP_MEMBER;
        client->muted_until_ms = 0;
        permissions__refresh_client_powers(client);
        return;
    }
    client->channel_group_role = channel_groups__role(client, &g_channel_array[client->channel_id]);
    client->muted_until_ms = 0;
    util__hash_password_to_base64(client->public_key, identity, sizeof(identity));
    util__get_monotonic_ms(&now);
    for (i = 0; i < cvector_size(g_channel_array[client->channel_id].group_members); i++)
    {
        if (clib__strcmp(g_channel_array[client->channel_id].group_members[i].identity, identity) == 0 && g_channel_array[client->channel_id].group_members[i].muted_until_ms > now)
        {
            client->muted_until_ms = g_channel_array[client->channel_id].group_members[i].muted_until_ms;
        }
    }
    permissions__refresh_client_powers(client);
}

/**
 * @brief Stores a temporary mute by identity so reconnecting does not remove it.
 * @param channel Affected channel with the channels write lock held.
 * @param identity Authenticated identity hash.
 * @param until_ms Monotonic expiry, or zero to unmute.
 */
boole channel_groups__mute(channel_t *channel, cstring identity, uint64 until_ms)
{
    uint64 i = 0;
    channel_group_member_t member;

    if (identity == NULL_POINTER)
    {
        return FALSE;
    }
    if (identity[0] == 0)
    {
        return FALSE;
    }
    if (clib__strlen(identity) >= sizeof(member.identity))
    {
        return FALSE;
    }
    for (i = 0; i < cvector_size(channel->group_members); i++)
    {
        if (clib__strcmp(channel->group_members[i].identity, identity) == 0)
        {
            channel->group_members[i].muted_until_ms = until_ms;
            return TRUE;
        }
    }
    if (until_ms == 0)
    {
        return TRUE;
    }
    if (cvector_size(channel->group_members) >= MAX_SERVER_SLOTS)
    {
        return FALSE;
    }
    clib__memset(&member, 0, sizeof(member));
    clib__memcpy(member.identity, identity, clib__strlen(identity));
    member.muted_until_ms = until_ms;
    if (_channel_groups_internal__append_member(channel, &member) == FALSE)
    {
        clib__null_memory(&member, sizeof(member));
        return FALSE;
    }
    clib__null_memory(&member, sizeof(member));
    return TRUE;
}

/**
 * @brief Frees assignments before a channel slot is cleared.
 * @param channel Channel being removed with the channels write lock held.
 */
void channel_groups__clear(channel_t *channel)
{
    cvector_free(channel->group_members);
    channel->group_members = NULL_POINTER;
}

/**
 * @brief Loads channel assignments from the settings file during startup.
 * @param channel Channel receiving the assignments.
 * @param members Saved array of identity hashes and channel roles.
 */
void channel_groups__read(channel_t *channel, cJSON *members)
{
    cJSON *member = NULL_POINTER;
    cJSON *identity = NULL_POINTER;
    cJSON *role = NULL_POINTER;

    if (!cJSON_IsArray(members))
    {
        return;
    }
    cJSON_ArrayForEach(member, members)
    {
        identity = cJSON_GetObjectItemCaseSensitive(member, "identity");
        role = cJSON_GetObjectItemCaseSensitive(member, "role");
        if (cJSON_IsString(identity) && cJSON_IsNumber(role) && role->valuedouble >= 0 && role->valuedouble < CHANNEL_GROUP_LIMIT && role->valuedouble == role->valueint)
        {
            channel_groups__set(channel, identity->valuestring, (uint64)role->valueint);
        }
    }
}

/**
 * @brief Writes channel assignments for persistence.
 * @param channel Channel being read with the channels lock held.
 */
cJSON *channel_groups__write(const channel_t *channel)
{
    cJSON *members = cJSON_CreateArray();
    cJSON *member = NULL_POINTER;
    uint64 i = 0;

    for (i = 0; i < cvector_size(channel->group_members); i++)
    {
        if (channel->group_members[i].role == CHANNEL_GROUP_MEMBER || !g_channel_groups[channel->group_members[i].role].is_existing)
        {
            continue;
        }
        member = cJSON_CreateObject();
        if (!util__json_add_owned_array(members, member))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(member, "identity", channel->group_members[i].identity))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(member, "role", channel->group_members[i].role))
        {
            goto json_failure;
        }
    }
    return members;

json_failure:
    util__json_delete_secure(members);
    return NULL_POINTER;
}

/**
 * @brief Loads custom channel group definitions at startup.
 * @param catalog Saved definitions, separate from server groups.
 */
boole channel_groups__load_catalog(cJSON *catalog)
{
    cJSON *entry = NULL_POINTER;
    cJSON *id = NULL_POINTER;
    cJSON *name = NULL_POINTER;
    cJSON *icon = NULL_POINTER;
    group_permissions_t parsed;
    uint64 index = 0;

    if (catalog == NULL_POINTER)
    {
        return TRUE;
    }
    if (!cJSON_IsArray(catalog))
    {
        return FALSE;
    }
    cJSON_ArrayForEach(entry, catalog)
    {
        id = cJSON_GetObjectItemCaseSensitive(entry, "group_id");
        name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        icon = cJSON_GetObjectItemCaseSensitive(entry, "icon_id");
        if (!cJSON_IsNumber(id) || !(id->valuedouble >= 1 && id->valuedouble < CHANNEL_GROUP_LIMIT) || id->valuedouble != id->valueint || !cJSON_IsString(name) || !name->valuestring[0] || clib__strlen(name->valuestring) >= GROUP_MAX_NAME_LENGTH)
        {
            return FALSE;
        }
        index = (uint64)id->valueint;
        if (cJSON_GetObjectItemCaseSensitive(entry, "permissions") != NULL_POINTER)
        {
            if (!channel_groups__read_permissions(cJSON_GetObjectItemCaseSensitive(entry, "permissions"), &parsed))
            {
                return FALSE;
            }
            if (index == CHANNEL_GROUP_ADMIN && !cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(entry, "permissions"), "bypass_channel_password"))
            {
                parsed.values[PERMISSION_BYPASS_CHANNEL_PASSWORD] = PERMISSION_GRANT;
            }
            g_channel_groups[index].permissions = parsed;
        }
        g_channel_groups[index].is_existing = TRUE;
        clib__string_copy(g_channel_groups[index].name, sizeof(g_channel_groups[index].name), name->valuestring);
        g_channel_groups[index].icon_id = index == CHANNEL_GROUP_ADMIN ? -2 : -1;
        if (cJSON_IsNumber(icon) && icon->valuedouble >= -2 && icon->valuedouble < MAX_ICONS && icon->valuedouble == icon->valueint)
        {
            g_channel_groups[index].icon_id = icon->valueint;
        }
    }
    return TRUE;
}

/**
 * @brief Writes channel group names and icons, optionally including their permissions.
 * @param include_permissions Whether the recipient may read permission values.
 */
cJSON *channel_groups__write_catalog(boole include_permissions)
{
    cJSON *catalog = cJSON_CreateArray();
    cJSON *entry = NULL_POINTER;
    uint64 i = 0;

    for (i = 1; i < CHANNEL_GROUP_LIMIT; i++)
    {
        if (!g_channel_groups[i].is_existing)
        {
            continue;
        }
        entry = cJSON_CreateObject();
        if (!util__json_add_owned_array(catalog, entry))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(entry, "group_id", i))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(entry, "name", g_channel_groups[i].name))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(entry, "icon_id", (double)g_channel_groups[i].icon_id))
        {
            goto json_failure;
        }
        if (include_permissions)
        {
            if (!util__json_add_owned_object(entry, "permissions", channel_groups__write_permissions(&g_channel_groups[i].permissions)))
            {
                goto json_failure;
            }
        }
    }
    return catalog;

json_failure:
    util__json_delete_secure(catalog);
    return NULL_POINTER;
}

/**
 * @brief Grows the vendored vector without its allocation-asserting append macro.
 * @attention The old vector remains intact on failure; callers hold the channels write lock.
 */
static boole _channel_groups_internal__append_member(channel_t *channel, const channel_group_member_t *member)
{
    uint64 count = cvector_size(channel->group_members);
    uint64 capacity = cvector_capacity(channel->group_members);
    uint64 new_capacity = 0;
    uint64 bytes = 0;
    cvector_metadata_t *old_storage = NULL_POINTER;
    cvector_metadata_t *new_storage = NULL_POINTER;

    if (count >= MAX_SERVER_SLOTS)
    {
        return FALSE;
    }
    if (count == capacity)
    {
        new_capacity = capacity == 0 ? 8 : capacity * 2;
        if (new_capacity > MAX_SERVER_SLOTS)
        {
            new_capacity = MAX_SERVER_SLOTS;
        }
        if (new_capacity > (SIZE_MAX - sizeof(cvector_metadata_t)) / sizeof(channel_group_member_t))
        {
            return FALSE;
        }
        bytes = sizeof(cvector_metadata_t) + new_capacity * sizeof(channel_group_member_t);
        new_storage = (cvector_metadata_t *)calloc(1, bytes);
        if (new_storage == NULL_POINTER)
        {
            return FALSE;
        }
        if (channel->group_members != NULL_POINTER)
        {
            old_storage = cvector_vec_to_base(channel->group_members);
            memcpy(new_storage, old_storage, sizeof(cvector_metadata_t) + count * sizeof(channel_group_member_t));
            clib__null_memory(old_storage, sizeof(cvector_metadata_t) + capacity * sizeof(channel_group_member_t));
            free(old_storage);
        }
        new_storage->capacity = new_capacity;
        channel->group_members = cvector_base_to_vec(new_storage);
    }
    channel->group_members[count] = *member;
    cvector_set_size(channel->group_members, count + 1);
    return TRUE;
}
