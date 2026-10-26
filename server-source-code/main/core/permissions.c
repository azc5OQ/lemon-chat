// This file checks what a client's groups let them do and reads and saves group permissions.
#include "permissions.h"
#include "identity_store.h"
#include "../base.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include <string.h>

uint64 g_guest_group_id = MAX_GROUPS;

static const char* g_permission_names[PERMISSION_COUNT] =
{
    "create_channel", "create_temp_channel", "kick", "ban", "view_stored_users",
    "view_group_permissions", "view_group_members", "create_groups", "edit_groups", "delete_groups", "assign_groups", "remove_groups", "edit_group_permissions", "manage_icons", "view_client_details", "view_server_logs", "view_server_info",
    "send_offline_messages", "receive_offline_messages", "message_idle_clients", "call_idle_clients"
};
static const char* g_permission_power_names[PERMISSION_POWER_COUNT] =
{
    "kick_power", "kick_protection", "ban_power", "ban_protection",
    "group_management_power", "group_protection",
    "talk_power", "channel_message_power", "channel_picture_power", "stream_power"
};

static void _permissions_internal__resolve(const client_t* client, group_permissions_t* result);
static void _permissions_internal__resolve_change(const client_t* client, uint64 group_id, const group_permissions_t* replacement, group_permissions_t* result);
static boole _permissions_internal__has_group(const client_t* client, uint64 group_id);
static boole _permissions_internal__allows_group_members_change(const client_t* sender, uint64 group_id, const group_permissions_t* proposed);
static boole _permissions_internal__allows_change(const client_t* sender, const client_t* target, uint64 group_id, const group_permissions_t* replacement);
static boole _permissions_internal__is_allowed(const client_t* client, const group_permissions_t* result, permission_t permission);
static void _permissions_internal__combine(group_permissions_t* result, const group_permissions_t* group);

/**
 * @brief Fills a new group's permissions from the selected preset.
 * @param type Preset selected when creating the group.
 * @param permissions Receives the initial permissions and powers.
 */
void permissions__set_preset(group_type_t type, group_permissions_t* permissions)
{
    uint i = 0;

    memset(permissions, 0, sizeof(*permissions));
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        permissions->values[i] = PERMISSION_DENY;
    }
    permissions->values[PERMISSION_CREATE_TEMP_CHANNEL] = PERMISSION_GRANT;
    permissions->values[PERMISSION_VIEW_SERVER_INFO] = PERMISSION_GRANT;
    permissions->values[PERMISSION_MESSAGE_IDLE_CLIENTS] = PERMISSION_GRANT;
    permissions->values[PERMISSION_CALL_IDLE_CLIENTS] = PERMISSION_GRANT;
    permissions->powers[PERMISSION_TALK_POWER] = type == GROUP_TYPE_MODERATOR ? 50 : 25;
    permissions->powers[PERMISSION_CHANNEL_MESSAGE_POWER] = type == GROUP_TYPE_MODERATOR ? 50 : 25;
    permissions->powers[PERMISSION_CHANNEL_PICTURE_POWER] = type == GROUP_TYPE_MODERATOR ? 50 : 25;
    permissions->powers[PERMISSION_STREAM_POWER] = type == GROUP_TYPE_MODERATOR ? 50 : 25;
    if (type != GROUP_TYPE_GUEST)
    {
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
    }
}

/**
 * @brief Reads the preset name used in saved groups and creation requests.
 * @param name Preset name to check.
 * @param type Receives the preset when the name is valid.
 */
boole permissions__parse_group_type(cstring name, group_type_t* type)
{
    uint i = 0;

    if (name == NULL_POINTER || type == NULL_POINTER)
    {
        return FALSE;
    }
    for (i = GROUP_TYPE_NORMAL; i <= GROUP_TYPE_ADMIN; i++)
    {
        if (strcmp(name, permissions__group_type_name((group_type_t)i)) == 0)
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
boole permissions__refresh_guest_membership(client_t* client)
{
    uint64 i = 0;
    int id = 0;
    boole has_other = FALSE;
    boole has_guest = FALSE;
    boole changed = FALSE;

    if (g_server_settings.is_permission_system_enabled == FALSE || g_guest_group_id >= MAX_GROUPS
        || client == NULL_POINTER || client->is_music_bot == TRUE)
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
        if (id < 0 || id >= MAX_GROUPS || g_groups_array[id].is_existing == FALSE
            || ((uint64)id == g_guest_group_id && (has_other == TRUE || has_guest == TRUE)))
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
        cvector_push_back(client->group_ids, (int)g_guest_group_id);
        changed = TRUE;
    }
    permissions__refresh_client_powers(client);
    return changed;
}

/**
 * @brief Combines each permission separately so an Allow and the highest powers win.
 * @param result Permissions collected so far.
 * @param group Permissions to include.
 */
static void _permissions_internal__combine(group_permissions_t* result, const group_permissions_t* group)
{
    uint p = 0;

    if (group == NULL_POINTER)
    {
        return;
    }
    for (p = 0; p < PERMISSION_COUNT; p++)
    {
        if (group->values[p] > result->values[p])
        {
            result->values[p] = group->values[p];
        }
    }
    for (p = 0; p < PERMISSION_POWER_COUNT; p++)
    {
        if (group->powers[p] > result->powers[p])
        {
            result->powers[p] = group->powers[p];
        }
    }
}

/**
 * @brief Combines group permissions, optionally replacing one group to check a proposed change.
 * @param client -> client whose groups are read with the clients and groups locks held
 * @param group_id -> group to replace, or MAX_GROUPS to leave all groups unchanged
 * @param replacement -> proposed permissions, or NULL to remove the group
 * @param result -> combined permissions
 */
static void _permissions_internal__resolve_change(const client_t* client, uint64 group_id, const group_permissions_t* replacement, group_permissions_t* result)
{
    uint64 i = 0;
    uint64 group_count = 0;
    int id = 0;
    const group_permissions_t* group = NULL_POINTER;

    memset(result, 0, sizeof(*result));
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
            if (id < 0 || id >= MAX_GROUPS || (uint64)id == group_id
                || (uint64)id == g_guest_group_id || g_groups_array[id].is_existing == FALSE)
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
static void _permissions_internal__resolve(const client_t* client, group_permissions_t* result)
{
    _permissions_internal__resolve_change(client, MAX_GROUPS, NULL_POINTER, result);
}


/**
 * @brief Saves current group powers so audio and video packets need no group lookup.
 * @param client Client to update with the clients write lock and groups lock held.
 */
void permissions__refresh_client_powers(client_t* client)
{
    group_permissions_t effective;

    if (client == NULL_POINTER)
    {
        return;
    }
    _permissions_internal__resolve(client, &effective);
    client->talk_power = effective.powers[PERMISSION_TALK_POWER];
    client->channel_message_power = effective.powers[PERMISSION_CHANNEL_MESSAGE_POWER];
    client->channel_picture_power = effective.powers[PERMISSION_CHANNEL_PICTURE_POWER];
    client->stream_power = effective.powers[PERMISSION_STREAM_POWER];
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
boole permissions__can_use_channel(const client_t* client, const channel_t* channel, permission_power_t power)
{
    uint available = 0;
    uint required = 0;

    if (client == NULL_POINTER || channel == NULL_POINTER || client->is_existing == FALSE
        || client->is_authenticated == FALSE || channel->is_existing == FALSE)
    {
        return FALSE;
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
        default:
            return FALSE;
    }
    if (g_server_settings.is_permission_system_enabled == FALSE || client->is_admin == TRUE)
    {
        return TRUE;
    }
    if (channel->is_temp_channel == TRUE && client->is_temp_admin_channel == TRUE
        && client->temp_channel_id == channel->channel_id)
    {
        available = PERMISSION_POWER_MAX;
    }
    return (boole)(available >= required);
}

/**
 * @brief Applies group choices or the existing guest rules when no group makes a choice.
 * @param client -> client making the request
 * @param result -> combined group permissions
 * @param permission -> action to check
 */
static boole _permissions_internal__is_allowed(const client_t* client, const group_permissions_t* result, permission_t permission)
{
    if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
    {
        return FALSE;
    }
    if (client->is_admin == TRUE)
    {
        return TRUE;
    }
    if (result->values[permission] != PERMISSION_DEFAULT)
    {
        return (boole)(result->values[permission] == PERMISSION_GRANT);
    }
    if (permission == PERMISSION_CREATE_TEMP_CHANNEL || permission == PERMISSION_VIEW_SERVER_INFO
        || permission == PERMISSION_MESSAGE_IDLE_CLIENTS || permission == PERMISSION_CALL_IDLE_CLIENTS)
    {
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Checks a permission using current groups. The caller holds the clients lock.
 * @param client -> client making the request
 * @param permission -> action to check
 */
boole permissions__is_allowed(const client_t* client, permission_t permission)
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
 * @brief Checks kick or ban rights and target protection. The caller holds the clients lock.
 * @param sender -> client requesting the kick or ban
 * @param target -> client affected by the request
 * @param permission -> kick or ban permission
 */
boole permissions__can_moderate(const client_t* sender, const client_t* target, permission_t permission)
{
    group_permissions_t actor;
    group_permissions_t protected_client;
    uint power = 0;
    boole allowed = FALSE;

    if (sender == NULL_POINTER || target == NULL_POINTER || target->is_existing == FALSE
        || target->is_authenticated == FALSE || (permission != PERMISSION_KICK && permission != PERMISSION_BAN))
    {
        return FALSE;
    }
    power = permission == PERMISSION_KICK ? PERMISSION_KICK_POWER : PERMISSION_BAN_POWER;
    clib__read_lock(&g_groups_global_rwlock_guard);
    _permissions_internal__resolve(sender, &actor);
    _permissions_internal__resolve(target, &protected_client);
    if (_permissions_internal__is_allowed(sender, &actor, permission) == TRUE)
    {
        allowed = (boole)(sender->is_admin == TRUE || (target->is_admin == FALSE
            && actor.powers[power] >= protected_client.powers[power + 1]));
    }
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}

/**
 * @brief Reads known permission fields and keeps omitted fields at their defaults.
 * @param object -> saved settings or a proposed group edit
 * @param permissions -> receives the permissions only if all fields are valid
 */
boole permissions__read(cJSON* object, group_permissions_t* permissions)
{
    group_permissions_t parsed;
    cJSON* value = NULL_POINTER;
    uint i = 0;
    uint known = 0;

    memset(&parsed, 0, sizeof(parsed));
    parsed.powers[PERMISSION_TALK_POWER] = 25;
    parsed.powers[PERMISSION_CHANNEL_MESSAGE_POWER] = 25;
    parsed.powers[PERMISSION_CHANNEL_PICTURE_POWER] = 25;
    parsed.powers[PERMISSION_STREAM_POWER] = 25;
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
        if (strcmp(value->valuestring, "grant") == 0)
        {
            parsed.values[i] = PERMISSION_GRANT;
        }
        else if (strcmp(value->valuestring, "deny") == 0)
        {
            parsed.values[i] = PERMISSION_DENY;
        }
        else if (strcmp(value->valuestring, "default") != 0)
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
        if (cJSON_IsNumber(value) == FALSE || !(value->valuedouble >= 0 && value->valuedouble <= PERMISSION_POWER_MAX)
            || value->valuedouble != (double)value->valueint)
        {
            return FALSE;
        }
        parsed.powers[i] = (uint)value->valueint;
    }
    if (known != (uint)cJSON_GetArraySize(object))
    {
        return FALSE;
    }
    *permissions = parsed;
    return TRUE;
}

/**
 * @brief Writes the same permission fields for saved settings and client messages.
 * @param permissions -> group permissions to write
 */
cJSON* permissions__write(const group_permissions_t* permissions)
{
    cJSON* object = cJSON_CreateObject();
    const char* value = NULL_POINTER;
    uint i = 0;

    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        value = permissions->values[i] == PERMISSION_DENY ? "deny"
            : permissions->values[i] == PERMISSION_GRANT ? "grant" : "default";
        cJSON_AddStringToObject(object, g_permission_names[i], value);
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        cJSON_AddNumberToObject(object, g_permission_power_names[i], permissions->powers[i]);
    }
    return object;
}

/**
 * @brief Checks a permission when the caller already holds the clients and groups locks.
 * @param client -> client making the request
 * @param permission -> action to check
 */
boole permissions__is_allowed_in_groups(const client_t* client, permission_t permission)
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
static boole _permissions_internal__has_group(const client_t* client, uint64 group_id)
{
    uint64 i = 0;
    int id = 0;

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
boole permissions__can_manage_group(const client_t* sender, uint64 group_id, permission_t permission)
{
    group_permissions_t actor;

    if (sender == NULL_POINTER || group_id >= MAX_GROUPS || g_groups_array[group_id].is_existing == FALSE
        || g_server_settings.is_permission_system_enabled == FALSE)
    {
        return FALSE;
    }
    if (group_id == g_guest_group_id && (permission == PERMISSION_DELETE_GROUPS
        || permission == PERMISSION_ASSIGN_GROUPS || permission == PERMISSION_REMOVE_GROUPS))
    {
        return FALSE;
    }
    _permissions_internal__resolve(sender, &actor);
    if (_permissions_internal__is_allowed(sender, &actor, permission) == FALSE)
    {
        return FALSE;
    }
    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    if (group_id == ADMIN_GROUP_ID || actor.powers[PERMISSION_GROUP_MANAGEMENT_POWER]
        < g_groups_array[group_id].permissions.powers[PERMISSION_GROUP_PROTECTION])
    {
        return FALSE;
    }
    if ((permission == PERMISSION_EDIT_GROUP_PERMISSIONS || permission == PERMISSION_DELETE_GROUPS)
        && _permissions_internal__has_group(sender, group_id) == TRUE)
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
static boole _permissions_internal__allows_change(const client_t* sender, const client_t* target, uint64 group_id, const group_permissions_t* replacement)
{
    group_permissions_t actor;
    group_permissions_t before;
    group_permissions_t after;
    uint i = 0;

    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    if (target == sender || target->is_admin == TRUE || _permissions_internal__has_group(target, ADMIN_GROUP_ID) == TRUE)
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
        if (_permissions_internal__is_allowed(target, &after, (permission_t)i) == TRUE
            && _permissions_internal__is_allowed(target, &before, (permission_t)i) == FALSE
            && _permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
        {
            return FALSE;
        }
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        if (after.powers[i] > before.powers[i]
            && after.powers[i] > actor.powers[i == PERMISSION_GROUP_PROTECTION ? PERMISSION_GROUP_MANAGEMENT_POWER : i])
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
boole permissions__can_change_member(const client_t* sender, const client_t* target, uint64 group_id, boole add)
{
    group_permissions_t actor;
    const group_permissions_t* assigned = NULL_POINTER;
    uint i = 0;
    uint limit = 0;

    if (target == NULL_POINTER || target->is_existing == FALSE || target->is_authenticated == FALSE
        || target->is_music_bot == TRUE || permissions__can_manage_group(sender, group_id,
            add == TRUE ? PERMISSION_ASSIGN_GROUPS : PERMISSION_REMOVE_GROUPS) == FALSE)
    {
        return FALSE;
    }
    if (add == TRUE && sender->is_admin == FALSE)
    {
        assigned = &g_groups_array[group_id].permissions;
        _permissions_internal__resolve(sender, &actor);
        for (i = 0; i < PERMISSION_COUNT; i++)
        {
            if (assigned->values[i] == PERMISSION_GRANT
                && _permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
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
boole permissions__can_edit_group_permissions(const client_t* sender, uint64 group_id, const group_permissions_t* proposed)
{
    group_permissions_t actor;
    uint i = 0;
    uint limit = 0;

    if (group_id == ADMIN_GROUP_ID || permissions__can_manage_group(sender, group_id, PERMISSION_EDIT_GROUP_PERMISSIONS) == FALSE)
    {
        return FALSE;
    }
    if (sender->is_admin == TRUE)
    {
        return TRUE;
    }
    _permissions_internal__resolve(sender, &actor);
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (_permissions_internal__is_allowed(sender, proposed, (permission_t)i) == TRUE
            && _permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
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
        if (proposed->powers[i] > limit)
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
boole permissions__can_delete_group(const client_t* sender, uint64 group_id)
{
    if (group_id == ADMIN_GROUP_ID || group_id == g_guest_group_id
        || permissions__can_manage_group(sender, group_id, PERMISSION_DELETE_GROUPS) == FALSE)
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
boole permissions__can_create_group(const client_t* sender, const group_permissions_t* proposed)
{
    group_permissions_t actor;
    uint i = 0;
    uint limit = 0;

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
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        if (_permissions_internal__is_allowed(sender, proposed, (permission_t)i) == TRUE
            && _permissions_internal__is_allowed(sender, &actor, (permission_t)i) == FALSE)
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
static boole _permissions_internal__allows_group_members_change(const client_t* sender, uint64 group_id, const group_permissions_t* proposed)
{
    client_t stored;
    uint64 c = 0;
    uint64 i = 0;
    boole allowed = TRUE;

    for (c = 0; c < (uint64)g_server_settings.max_client_count; c++)
    {
        if (g_clients_array[c].is_existing == TRUE && g_clients_array[c].is_authenticated == TRUE
            && _permissions_internal__has_group(&g_clients_array[c], group_id) == TRUE
            && _permissions_internal__allows_change(sender, &g_clients_array[c], group_id, proposed) == FALSE)
        {
            return FALSE;
        }
    }
    memset(&stored, 0, sizeof(stored));
    stored.is_existing = TRUE;
    stored.is_authenticated = TRUE;
    pthread_mutex_lock(&g_client_stored_data_mutex);
    for (c = 0; c < MAX_CLIENT_STORED_DATA; c++)
    {
        if (g_client_stored_data[c].public_key[0] == 0)
        {
            continue;
        }
        cvector_clear(stored.group_ids);
        for (i = 0; i < g_client_stored_data[c].group_id_count; i++)
        {
            cvector_push_back(stored.group_ids, (int)g_client_stored_data[c].group_ids[i]);
        }
        if (_permissions_internal__has_group(&stored, group_id) == TRUE
            && _permissions_internal__allows_change(sender, &stored, group_id, proposed) == FALSE)
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
cJSON* permissions__write_access(const client_t* client)
{
    cJSON* object = cJSON_CreateObject();
    cJSON* powers = cJSON_CreateObject();
    group_permissions_t effective;
    uint i = 0;

    _permissions_internal__resolve(client, &effective);
    for (i = 0; i < PERMISSION_COUNT; i++)
    {
        cJSON_AddBoolToObject(object, g_permission_names[i], _permissions_internal__is_allowed(client, &effective, (permission_t)i));
    }
    for (i = 0; i < PERMISSION_POWER_COUNT; i++)
    {
        cJSON_AddNumberToObject(powers, g_permission_power_names[i], client->is_admin == TRUE ? PERMISSION_POWER_MAX : effective.powers[i]);
    }
    cJSON_AddItemToObject(object, "powers", powers);
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
cJSON* permissions__write_group_access(const client_t* client, uint64 group_id)
{
    cJSON* object = cJSON_CreateObject();
    permission_t actions[] = { PERMISSION_EDIT_GROUPS, PERMISSION_DELETE_GROUPS, PERMISSION_ASSIGN_GROUPS, PERMISSION_REMOVE_GROUPS, PERMISSION_EDIT_GROUP_PERMISSIONS };
    uint i = 0;

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
boole permissions__is_stored_client_allowed(const client_stored_data_t* stored, permission_t permission)
{
    client_t client;
    uint64 i = 0;
    boole allowed = FALSE;

    memset(&client, 0, sizeof(client));
    if (stored == NULL_POINTER || stored->public_key[0] == 0)
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
boole permissions__is_identity_allowed(const char* identity_hash, permission_t permission)
{
    uint64 i = 0;
    boole allowed = FALSE;

    clib__read_lock(&g_groups_global_rwlock_guard);
    pthread_mutex_lock(&g_client_stored_data_mutex);
    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (strcmp(g_client_stored_data[i].public_key, identity_hash) == 0)
        {
            allowed = permissions__is_stored_client_allowed(&g_client_stored_data[i], permission);
            break;
        }
    }
    pthread_mutex_unlock(&g_client_stored_data_mutex);
    clib__unlock(&g_groups_global_rwlock_guard);
    return allowed;
}
