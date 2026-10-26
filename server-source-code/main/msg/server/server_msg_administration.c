// This file sends admin replies, client details, server logs, icons and groups.
#include "../../core/permissions.h"

#include "server_msg.h"
#include "../../base.h"
#include "../../core/identity_store.h"
#include "../../util.h"
#include "../../clib/clib_string.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"
#include "../../core/server_logs.h"

/**
 * @brief sends one client's info (connected duration, ip, country, identity, last action) to a single
 *        admin client, in reply to a get_client_info request
 *
 * @param client_t* receiver -> the admin the reply is sent to (provides the websocket + shared secret)
 * @param client_t* subject -> the client the info is about
 *
 * @attention caller must hold at least the clients read lock
 *
 * @return void
 */
void server_msg__administration__send_client_info_to_single_client(client_t* receiver, client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    uint64 now_ms = 0;
    uint64 connected_seconds = 0;
    uint64 last_action_seconds_ago = 0;

    now_ms = util__get_timestamp_ms();
    if (now_ms >= subject->timestamp_connected)
    {
        connected_seconds = (now_ms - subject->timestamp_connected) / 1000;
    }
    if (now_ms >= subject->timestamp_last_action)
    {
        last_action_seconds_ago = (now_ms - subject->timestamp_last_action) / 1000;
    }

    root = server_msg__delivery__new_message("client_info", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddNumberToObject(message, "connected_seconds", (double)connected_seconds);
    cJSON_AddNumberToObject(message, "last_action_seconds_ago", (double)last_action_seconds_ago);
    cJSON_AddStringToObject(message, "ip_address", &subject->ip_address[0]);
    cJSON_AddStringToObject(message, "country_iso_code", &subject->country_iso_code[0]);
    cJSON_AddStringToObject(message, "identity", &subject->public_key[0]);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief This function sends icon list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @attention it doesnt need readlock, already called within writelock in msg/client/
 *
 * @return void
 */
void server_msg__administration__send_icon_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_icons_array = 0;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        return;
    }

    root = server_msg__delivery__new_message("icon_list", &message);
    json_icons_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_ICONS; x++)
    {
        cJSON* single_client = NULL_POINTER;

        if (g_icons_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_client = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_client, "icon_id", (double)g_icons_array[x].id);
        cJSON_AddStringToObject(single_client, "base64_icon", (g_icons_array[x].base64 != NULL_POINTER) ? g_icons_array[x].base64 : "");
        cJSON_AddItemToArray(json_icons_array, single_client);
    }

    cJSON_AddItemToObject(message, "icons", json_icons_array);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends the available groups to a client after login.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @attention it doesnt need readlock, already called within writelock in msg/client/
 *
 * @return void
 */
void server_msg__administration__send_group_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_groups_array = 0;
    cJSON* single_group_id_object = NULL_POINTER;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        return;
    }

    root = server_msg__delivery__new_message("group_list", &message);
    json_groups_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_GROUPS; x++)
    {
        if (g_groups_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_group_id_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_group_id_object, "group_id", (double)g_groups_array[x].id);
        cJSON_AddStringToObject(single_group_id_object, "group_name", g_groups_array[x].name);
        cJSON_AddStringToObject(single_group_id_object, "group_type", permissions__group_type_name(g_groups_array[x].group_type));
        cJSON_AddBoolToObject(single_group_id_object, "is_default_guest", g_groups_array[x].is_default_guest == TRUE);
        cJSON_AddNumberToObject(single_group_id_object, "group_linked_icon_id", (double)g_groups_array[x].icon_id);
        cJSON_AddItemToObject(single_group_id_object, "has_icon", cJSON_CreateBool(g_groups_array[x].has_icon == TRUE));
        if (permissions__is_allowed_in_groups(receiver, PERMISSION_VIEW_GROUP_PERMISSIONS) == TRUE)
        {
            cJSON_AddItemToObject(single_group_id_object, "permissions", permissions__write(&g_groups_array[x].permissions));
        }
        cJSON_AddItemToObject(single_group_id_object, "access", permissions__write_group_access(receiver, x));
        cJSON_AddItemToArray(json_groups_array, single_group_id_object);
    }

    cJSON_AddItemToObject(message, "groups", json_groups_array);
    cJSON_AddItemToObject(message, "access", permissions__write_access(receiver));

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends the admin identity-management list: every stored identity as { public_key_hash,
 *        group_ids[], is_online }. is_online is TRUE when a currently-connected authenticated client
 *        hashes to the same identity. admin-only; the caller checks that and holds the clients lock.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @note the caller must hold the clients read lock (g_clients_array is read for online status). the
 *       identity store is guarded by g_client_stored_data_mutex, taken here.
 *
 * @return void
 */
void server_msg__administration__send_identity_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_identities_array = 0;
    cJSON* single_identity_object = NULL_POINTER;
    cJSON* identity_group_ids_array = NULL_POINTER;
    client_t* connected_client = NULL_POINTER;
    boole is_online = FALSE;
    boole has_assigned_group = FALSE;
    uint64 group_id = 0;
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 c = 0;

    if (permissions__is_allowed(receiver, PERMISSION_VIEW_GROUP_MEMBERS) == FALSE)
    {
        return;
    }

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("identity_list", &message);
    json_identities_array = cJSON_CreateArray();

    clib__read_lock(&g_groups_global_rwlock_guard);
    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        is_online = FALSE;
        has_assigned_group = FALSE;

        if (g_client_stored_data[i].public_key[0] == 0 || (g_client_stored_data[i].group_id_count == 0 && g_client_stored_data[i].raw_public_key[0] == 0 && g_client_stored_data[i].base64_avatar[0] == 0))
        {
            continue;
        }

        single_identity_object = cJSON_CreateObject();
        cJSON_AddStringToObject(single_identity_object, "public_key_hash", &g_client_stored_data[i].public_key[0]);
        cJSON_AddStringToObject(single_identity_object, "username", &g_client_stored_data[i].username[0]);

        identity_group_ids_array = cJSON_CreateArray();
        for (t = 0; g_server_settings.is_permission_system_enabled == TRUE && t < g_client_stored_data[i].group_id_count; t++)
        {
            group_id = g_client_stored_data[i].group_ids[t];
            if (group_id == g_guest_group_id || group_id >= MAX_GROUPS || g_groups_array[group_id].is_existing == FALSE)
            {
                continue;
            }
            cJSON_AddItemToArray(identity_group_ids_array, cJSON_CreateNumber((double)group_id));
            has_assigned_group = TRUE;
        }
        if (g_server_settings.is_permission_system_enabled == TRUE && has_assigned_group == FALSE
            && g_guest_group_id < MAX_GROUPS)
        {
            cJSON_AddItemToArray(identity_group_ids_array, cJSON_CreateNumber((double)g_guest_group_id));
        }
        cJSON_AddItemToObject(single_identity_object, "group_ids", identity_group_ids_array);

        // mark online if any connected authenticated non-bot receiver hashes to this identity
        for (c = 0; c < g_server_settings.max_client_count; c++)
        {
            connected_client = &g_clients_array[c];

            if (connected_client->is_existing == FALSE || connected_client->is_authenticated == FALSE || connected_client->is_music_bot == TRUE)
            {
                continue;
            }
            if (connected_client->public_key[0] == 0)
            {
                continue;
            }

            util__hash_password_to_base64(connected_client->public_key, client_hash, sizeof(client_hash));
            if (clib__is_string_equal(client_hash, &g_client_stored_data[i].public_key[0]) == TRUE)
            {
                is_online = TRUE;
                break;
            }
        }
        cJSON_AddItemToObject(single_identity_object, "is_online", cJSON_CreateBool(is_online == TRUE));

        cJSON_AddItemToArray(json_identities_array, single_identity_object);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
    clib__unlock(&g_groups_global_rwlock_guard);

    cJSON_AddItemToObject(message, "identities", json_identities_array);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells a single client it should change the admin password (sent once after the first admin login,
 *        because the initial password was typed in cleartext at setup)
 *
 * @param client_t* receiver -> the client to notify
 *
 * @return void
 */
void server_msg__administration__send_force_admin_password_change_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("force_admin_password_change", &message);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends an access-denied message to a single client
 *
 * @param client_t* receiver -> client whose request was refused
 * @param char* reason -> explanation to display or NULL for insufficient permissions
 *
 * @attention this function is used within acquired readlock within msg/client/ , multiple functions
 *
 * @return void
 */
void server_msg__administration__send_access_denied_to_single_client(client_t* receiver, char* reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    if (receiver == NULL_POINTER || receiver->is_existing == FALSE || receiver->is_authenticated == FALSE)
    {
        return;
    }

    root = server_msg__delivery__new_message("access_denied", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddStringToObject(message, "reason", reason != NULL_POINTER && reason[0] != 0 ? reason : "Insufficient permissions.");

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Sends log entries after the caller checks permission to view server logs.
 *
 * @param client_t* receiver -> the client to reply to
 *
 * @return void
 */
void server_msg__administration__send_admin_log_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_lines = 0;

    json_lines = server_logs__build_json_array();
    if (json_lines == NULL_POINTER)
    {
        return;
    }

    root = server_msg__delivery__new_message("admin_log", &message);

    cJSON_AddItemToObject(message, "lines", json_lines);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts a group-added-to-client event to all clients
 *
 * @param client_t* subject -> the client that received the group
 * @param uint64 group_id -> id of the group added
 *
 * @return void
 */
void server_msg__administration__send_add_group_to_client_event_to_all_clients(client_t* subject, uint64 group_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        return;
    }

    root = server_msg__delivery__new_message("group_add_to_client", &message);

    cJSON_AddNumberToObject(message, "group_id", group_id);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a group-removed-from-client event to all clients
 *
 * @param client_t* subject -> the client that lost the group
 * @param uint64 group_id -> id of the group removed
 *
 * @return void
 */
void server_msg__administration__send_remove_group_from_client_event_to_all_clients(client_t* subject, uint64 group_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        return;
    }

    root = server_msg__delivery__new_message("remove_group_from_client", &message);

    cJSON_AddNumberToObject(message, "group_id", group_id);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a new-icon-added event to all clients
 *
 * @param uint64 new_icon_id -> id of the new icon
 * @param char* icon_base64_value -> base64-encoded icon image
 *
 * @return void
 */
void server_msg__administration__send_add_new_icon_event_to_all_clients(uint64 new_icon_id, char* icon_base64_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("icon_add", &message);

    cJSON_AddNumberToObject(message, "icon_id", new_icon_id);
    cJSON_AddStringToObject(message, "base64_icon", icon_base64_value);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a group-deleted-from-pool event to all clients
 *
 * @param uint64 group_id -> id of the group that was deleted
 *
 * @return void
 */
void server_msg__administration__send_remove_group_event_to_all_clients(uint64 group_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("group_delete", &message);

    cJSON_AddNumberToObject(message, "group_id", group_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts an icon-deleted-from-pool event to all clients
 *
 * @param uint64 icon_id -> id of the icon that was deleted
 *
 * @return void
 */
void server_msg__administration__send_remove_icon_event_to_all_clients(uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("icon_delete", &message);

    cJSON_AddNumberToObject(message, "icon_id", icon_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a group-icon-changed event to all clients
 *
 * @param uint64 group_id -> id of the group whose icon changed
 * @param boole has_icon -> whether the group now has an icon
 * @param uint64 icon_id -> the group's icon id (meaningful only when has_icon is TRUE)
 *
 * @return void
 */
void server_msg__administration__send_group_icon_changed_event_to_all_clients(uint64 group_id, boole has_icon, uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("group_icon_changed", &message);

    cJSON_AddNumberToObject(message, "group_id", group_id);
    cJSON_AddItemToObject(message, "has_icon", cJSON_CreateBool(has_icon == TRUE));
    cJSON_AddNumberToObject(message, "group_linked_icon_id", icon_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a new-group-created event to all clients
 *
 * @param uint64 group_id -> id of the new group
 * @param char* group_name -> name of the new group
 * @param uint64 group_linked_icon_id -> id of the icon linked to the group
 * @param boole has_icon -> TRUE when the group really has an icon, sent as a bool so the client knows
 *                          whether group_linked_icon_id is meaningful
 *
 * @return void
 */
void server_msg__administration__send_create_new_group_event_to_all_clients(uint64 group_id, char* group_name, uint64 group_linked_icon_id, boole has_icon)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("group_add", &message);

    cJSON_AddNumberToObject(message, "group_id", group_id);
    cJSON_AddStringToObject(message, "group_name", group_name);
    cJSON_AddStringToObject(message, "group_type", permissions__group_type_name(g_groups_array[group_id].group_type));
    cJSON_AddBoolToObject(message, "is_default_guest", g_groups_array[group_id].is_default_guest == TRUE);
    cJSON_AddNumberToObject(message, "group_linked_icon_id", group_linked_icon_id);
    cJSON_AddItemToObject(message, "has_icon", cJSON_CreateBool(has_icon == TRUE));

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief Tells connected clients that a group's permissions changed.
 * @param group_id -> changed group, with the clients and groups locks already held
 */
void server_msg__administration__send_group_permissions_changed(uint64 group_id)
{
    cJSON* message = NULL_POINTER;
    cJSON* root = server_msg__delivery__new_message("group_permissions_changed", &message);

    uint64 i = 0;

    cJSON_AddNumberToObject(message, "group_id", (double)group_id);
    cJSON_AddItemToObject(message, "permissions", permissions__write(&g_groups_array[group_id].permissions));
    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (permissions__is_allowed_in_groups(&g_clients_array[i], PERMISSION_VIEW_GROUP_PERMISSIONS) == TRUE)
        {
            server_msg__delivery__send_json_to_client(&g_clients_array[i], root);
        }
    }
    cJSON_Delete(root);
}

/**
 * @brief Tells a client whether it currently has admin access, even when groups are disabled.
 * @param receiver -> authenticated client receiving its own status
 */
void server_msg__administration__send_admin_status_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("admin_status", &message);
    cJSON_AddBoolToObject(message, "is_admin", receiver->is_admin);
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Refreshes group controls and current rights after a membership change.
 * @param receiver -> client being updated while the caller holds only the clients lock
 */
void server_msg__administration__send_permission_update_to_single_client(client_t* receiver)
{
    if (receiver == NULL_POINTER || receiver->is_existing == FALSE || receiver->is_authenticated == FALSE || receiver->is_music_bot == TRUE)
    {
        return;
    }
    clib__read_lock(&g_groups_global_rwlock_guard);
    identity_store__update_offline_public_key(receiver);
    server_msg__administration__send_group_list_to_single_client(receiver);
    clib__unlock(&g_groups_global_rwlock_guard);
}

/**
 * @brief Refreshes connected users after group permissions change. The caller holds only the clients lock.
 */
void server_msg__administration__send_permission_updates_to_all_clients(void)
{
    uint64 i = 0;

    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        server_msg__administration__send_permission_update_to_single_client(&g_clients_array[i]);
    }
}
