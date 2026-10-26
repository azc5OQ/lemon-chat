// This file sends usernames, avatars, visible country codes and stored client information.
#include "../../core/permissions.h"

#include "server_msg.h"
#include "../../base.h"
#include "../../core/identity_store.h"
#include "../../../third-party/rxi-log/log.h"

/**
 * @brief the country code users may learn about a client: nothing unless flags are displayed, and
 *        nothing for an admin while admin flags are kept private. country blocking resolves the code
 *        for its own check, which must never leak it to users
 *
 * @param client_t* client -> the client being described
 *
 * @return cstring -> the code to send, possibly ""
 */
cstring server_msg__profile__country_code_to_send(client_t* client)
{
    if (g_server_settings.is_display_country_flags_active == FALSE)
    {
        return "";
    }

    if (g_server_settings.is_admin_country_flag_hidden == TRUE && client->is_admin == TRUE)
    {
        return "";
    }

    return client->country_iso_code;
}

/**
 * @brief This function sends client_rename_message to all authenticated clients
 *
 * @param client_t* subject -> the client that changed its username
 * @param char* new_username -> self explanatory
 *
 * @attention this function is used within acquired readlock within msg/client/ client_msg__profile__process_change_client_username function
 *
 * @return void
 */
void server_msg__profile__send_client_rename_message_to_all_clients(client_t* subject, char* new_username)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("client_rename", &message);

    cJSON_AddStringToObject(message, "new_username", new_username);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}


/**
 * @brief broadcasts the country code a client's row shows from now on, to all authenticated non-bot
 *        clients. the clients only ever see the code, never the setting that withholds it, so this
 *        is sent whenever the code the server is willing to show changed: the client became or
 *        stopped being an admin, or the admin toggled is_admin_country_flag_hidden
 *
 * @param client_t* subject -> the client whose shown code changed
 *
 * @note must be called with the clients lock already held (like the group-event broadcasts)
 *
 * @return void
 */
void server_msg__profile__send_client_country_code_changed_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("client_country_code_changed", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddStringToObject(message, "country_iso_code", server_msg__profile__country_code_to_send(subject));

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief re-sends every connected admin's shown country code to all clients, after the admin
 *        toggled is_admin_country_flag_hidden: the flags appear or vanish for everyone at once
 *
 * @note must be called with the clients lock already held
 *
 * @return void
 */
void server_msg__profile__send_admin_country_codes_to_all_clients(void)
{
    uint64 i = 0;
    client_t* client = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_admin == FALSE)
        {
            continue;
        }

        server_msg__profile__send_client_country_code_changed_to_all_clients(client);
    }
}

/**
 * @brief broadcasts a lightweight "this client's avatar changed" event (no image payload) to all
 *        authenticated non-bot clients, so they can re-request that client's avatar if they show it.
 *
 * @param client_t* subject -> the client whose avatar was set or deleted
 *
 * @note must be called with the clients lock already held (like the group-event broadcasts)
 *
 * @return void
 */
void server_msg__profile__send_avatar_changed_event_to_all_clients(client_t* subject)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("avatar_changed", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief sends one client's avatar (full base64 data-url, or empty string if none) to a single
 *        requesting client. used for both the profile-pane request and the chunked tree lazy-load,
 *        so avatars are only ever sent to clients that asked for them.
 *
 * @param client_t* receiver -> the client that asked for the avatar
 * @param uint64 avatar_owner_client_id -> whose avatar this is
 * @param char*          base64_avatar    -> the avatar data-url, or "" for none
 *
 * @return void
 */
void server_msg__profile__send_client_avatar_to_single_client(client_t* receiver, uint64 avatar_owner_client_id, char* base64_avatar)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    if (receiver == NULL_POINTER)
    {
        return;
    }

    root = server_msg__delivery__new_message("client_avatar", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)avatar_owner_client_id);
    cJSON_AddStringToObject(message, "base64_avatar", (base64_avatar != NULL_POINTER) ? base64_avatar : "");

    server_msg__delivery__send_json_to_client(receiver, root);

    cJSON_Delete(root);
}

/**
 * @brief Sends saved users' names, avatars and active group badges to a client with access to the list.
 *        Public keys and last seen times are included only when their server settings are enabled.
 * @param receiver -> client receiving the list while the caller holds the clients lock
 */
void server_msg__profile__send_stored_clients_to_single_client(client_t* receiver)
{
    uint64 i = 0;
    uint64 t = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_stored_clients = 0;
    cJSON* json_stored_client = 0;
    cJSON* json_group_ids = 0;
    uint64 group_id = 0;
    boole has_assigned_group = FALSE;

    if (receiver == NULL_POINTER)
    {
        return;
    }

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_stored_client_list_allowed == FALSE
        || permissions__is_allowed(receiver, PERMISSION_VIEW_STORED_USERS) == FALSE)
    {
        return;
    }

    root = server_msg__delivery__new_message("stored_clients_list", &message);
    json_stored_clients = cJSON_CreateArray();

    clib__read_lock(&g_groups_global_rwlock_guard);
    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0)
        {
            continue;
        }

        json_stored_client = cJSON_CreateObject();
        cJSON_AddStringToObject(json_stored_client, "identity_hash", g_client_stored_data[i].public_key);
        cJSON_AddStringToObject(json_stored_client, "username", g_client_stored_data[i].username);
        cJSON_AddStringToObject(json_stored_client, "base64_avatar", &g_client_stored_data[i].base64_avatar[0]);

        if (g_server_settings.is_offline_messaging_allowed == TRUE && g_client_stored_data[i].raw_public_key[0] != 0
            && permissions__is_allowed_in_groups(receiver, PERMISSION_SEND_OFFLINE_MESSAGES) == TRUE
            && permissions__is_stored_client_allowed(&g_client_stored_data[i], PERMISSION_RECEIVE_OFFLINE_MESSAGES) == TRUE)
        {
            cJSON_AddStringToObject(json_stored_client, "public_key", &g_client_stored_data[i].raw_public_key[0]);
        }

        // when the admin enabled last-seen, tell the receiver when this identity was last connected
        // (unix seconds). left out entirely while the setting is off, so nothing leaks
        if (g_server_settings.is_last_seen_enabled == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            cJSON_AddNumberToObject(json_stored_client, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds);
        }

        json_group_ids = cJSON_CreateArray();
        has_assigned_group = FALSE;
        for (t = 0; g_server_settings.is_permission_system_enabled == TRUE && t < g_client_stored_data[i].group_id_count; t++)
        {
            group_id = g_client_stored_data[i].group_ids[t];
            if (group_id == g_guest_group_id || group_id >= MAX_GROUPS || g_groups_array[group_id].is_existing == FALSE)
            {
                continue;
            }
            cJSON_AddItemToArray(json_group_ids, cJSON_CreateNumber((double)group_id));
            has_assigned_group = TRUE;
        }
        if (g_server_settings.is_permission_system_enabled == TRUE && has_assigned_group == FALSE
            && g_guest_group_id < MAX_GROUPS)
        {
            cJSON_AddItemToArray(json_group_ids, cJSON_CreateNumber((double)g_guest_group_id));
        }
        cJSON_AddItemToObject(json_stored_client, "group_ids", json_group_ids);

        cJSON_AddItemToArray(json_stored_clients, json_stored_client);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
    clib__unlock(&g_groups_global_rwlock_guard);

    cJSON_AddItemToObject(message, "stored_clients", json_stored_clients);

    server_msg__delivery__send_json_to_client(receiver, root);

    cJSON_Delete(root);
}
