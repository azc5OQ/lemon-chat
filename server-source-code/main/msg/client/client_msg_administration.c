// This file checks administrator requests for passwords, bans, groups, icons, identities and logs.
#include "client_msg.h"
#include "../../core/permissions.h"
#include "../../core/audio_channel.h"
#include "../../core/video_channel.h"
#include "../../core/identity_store.h"
#include "../../core/administration.h"
#include "../../core/server_settings.h"
#include "../server/server_msg.h"
#include "../../core/server_logs.h"
#include "../../base.h"
#include "../../util.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/eteran-cvector/cvector.h"
#include "../../../third-party/zhicheng/base64.h"

// declarations
static boole _client_msg_administration_internal__is_admin_password_message_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_add_group_to_client_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_remove_group_from_client_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_process_server_settings_icon_upload_message_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_process_server_settings_add_new_group_message_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_kick_ban_request_valid(cJSON* json_root);
static void _client_msg_administration_internal__refresh_guest_membership(client_t* client);

/**
 * @brief Updates Guest membership and tells connected clients when it changes.
 * @param client -> client being updated with the clients and groups locks held
 */
static void _client_msg_administration_internal__refresh_guest_membership(client_t* client)
{
    uint64 i = 0;
    boole had_guest = FALSE;
    boole has_guest = FALSE;

    for (i = 0; i < cvector_size(client->group_ids); i++)
    {
        if ((uint64)client->group_ids[i] == g_guest_group_id)
        {
            had_guest = TRUE;
        }
    }
    permissions__refresh_guest_membership(client);
    for (i = 0; i < cvector_size(client->group_ids); i++)
    {
        if ((uint64)client->group_ids[i] == g_guest_group_id)
        {
            has_guest = TRUE;
        }
    }
    if (had_guest == FALSE && has_guest == TRUE)
    {
        server_msg__administration__send_add_group_to_client_event_to_all_clients(client, g_guest_group_id);
    }
    else if (had_guest == TRUE && has_guest == FALSE)
    {
        server_msg__administration__send_remove_group_from_client_event_to_all_clients(client, g_guest_group_id);
    }
}

/**
 * @brief Checks that an admin-password request contains a nonempty password within the size limit.
 * @param cJSON* json_root -> parsed client request
 * @return boole -> TRUE when the password field is valid; this does not check the password itself
 */
static boole _client_msg_administration_internal__is_admin_password_message_valid(cJSON* json_root)
{
    cJSON* json_admin_password = 0;
    cJSON* json_message_object = 0;
    int64 password_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_admin_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsString(json_admin_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_admin_password_message_valid cJSON_IsString(json_admin_password) \n");
        return FALSE;
    }

    if (json_admin_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_admin_password_message_valid json_admin_password->valuestring == NULL_POINTER \n");
        return FALSE;
    }

    password_length = clib__utf8_string_length_check_max_length(json_admin_password->valuestring, ADMIN_PASSWORD_MAX_LENGTH);
    if (password_length == -1 || password_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_admin_password_message_valid json_admin_password length is wrong \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin-password message, granting the client admin rights if the password matches
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_admin_password_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* admin_password = 0;
    cJSON* json_message_object = 0;
    uint64 cvector_loop_index = 0;
    boole is_group_found = FALSE;

    status = _client_msg_administration_internal__is_admin_password_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_admin_password_message _client_msg_administration_internal__is_admin_password_message_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_admin_password_message_end;
    }

    if (client->is_admin == TRUE || server_admin__allow_admin_password_attempt(client->ip_address, util__get_timestamp_ms()) == FALSE)
    {
        goto label_client_msg__process_admin_password_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    admin_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    status = util__password_matches(admin_password->valuestring, g_server_settings.admin_password);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", sender_client_id, "admin password is not correct \n");
        server_logs__admin_password_failed(client->username, client->ip_address, admin_password->valuestring);
        goto label_client_msg__process_admin_password_message_end;
    }

    if (client->is_admin == FALSE)
    {
        client->is_admin = TRUE;
        server_msg__administration__send_admin_status_to_single_client(client);
        clib__read_lock(&g_groups_global_rwlock_guard);

        if (g_server_settings.is_permission_system_enabled == TRUE && g_server_settings.is_display_admin_group_active == TRUE)
        {
            // only add the admin group to the client if the client does not have it
            if (client->group_ids != NULL_POINTER)
            {
                for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->group_ids); ++cvector_loop_index)
                {
                    if (client->group_ids[cvector_loop_index] == ADMIN_GROUP_ID)
                    {
                        is_group_found = TRUE;
                        break;
                    }
                }
            }

            if (is_group_found == FALSE)
            {
                cvector_push_back(client->group_ids, ADMIN_GROUP_ID);
            }

            server_msg__administration__send_add_group_to_client_event_to_all_clients(client, ADMIN_GROUP_ID);
        }

        // an admin's flag may be withheld, so the rows showing the client learn the code they show from now on
        if (g_server_settings.is_admin_country_flag_hidden == TRUE)
        {
            server_msg__profile__send_client_country_code_changed_to_all_clients(client);
        }

        // tie this identity to its groups in ram right away, so a reconnect restores admin within this
        // server run even before any disk save. persistence to disk still needs a settings save
        _client_msg_administration_internal__refresh_guest_membership(client);
        identity_store__sync_client_identity_in_store(client);
        clib__unlock(&g_groups_global_rwlock_guard);

        // the setup admin password was typed in cleartext; on the first admin login, ask for a one-time change
        if (g_server_settings.is_admin_password_initial == TRUE)
        {
            server_msg__administration__send_force_admin_password_change_to_single_client(client);
            g_server_settings.is_admin_password_initial = FALSE;
            settings__save_server_settings_to_file();
        }
    }

    // send admin status to other clients possibly, or not, do they have to know you are an admin
label_client_msg__process_admin_password_message_end:
    server_msg__administration__send_permission_update_to_single_client(client);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a change-admin-password request from an authenticated admin: hashes and stores the new password
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_change_admin_password_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* new_password = 0;
    cJSON* json_message_object = 0;

    status = _client_msg_administration_internal__is_admin_password_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_change_admin_password_message invalid message \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_admin == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_change_admin_password_message sender is not an admin \n");
        goto label_client_msg__process_change_admin_password_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    new_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    util__hash_password_to_base64(new_password->valuestring, &g_server_settings.admin_password[0], ADMIN_PASSWORD_MAX_LENGTH);
    g_server_settings.is_admin_password_initial = FALSE;
    settings__save_server_settings_to_file();

    DBG_CLIENT_MESSAGE log_info("%s", "admin password changed by admin \n");

label_client_msg__process_change_admin_password_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates an add-group-to-client request: client id and group id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_add_group_to_client_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_group_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    if (cJSON_IsNumber(json_client_id) == FALSE
        || json_client_id->valuedouble != (double)json_client_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_add_group_to_client_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_add_group_to_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_group_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_id");
    if (cJSON_IsNumber(json_group_id) == FALSE
        || json_group_id->valuedouble != (double)json_group_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_add_group_to_client_valid cJSON_IsNumber(group_id) \n");
        return FALSE;
    }

    if (json_group_id->valueint < 0 || json_group_id->valueint >= MAX_GROUPS)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_add_group_to_client_valid json_group_id->valueint is not valid ", json_group_id->valueint, "\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Checks permission and adds a group to a connected client.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_add_group_to_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole powers_changed = FALSE;
    boole status = FALSE;
    boole does_sender_have_permissions_to_add_group = FALSE;
    client_t* client = 0;
    client_t* client_to_add_group_to = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id_to_add_group_to = 0;
    cJSON* json_group_id = 0;

    status = _client_msg_administration_internal__is_add_group_to_client_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_add_group_to_client_message _client_msg_administration_internal__is_add_group_to_client_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }


    clib__read_lock(&g_groups_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    // check if client that is sending request has permission to add groups
    does_sender_have_permissions_to_add_group = permissions__is_allowed_in_groups(client, PERMISSION_ASSIGN_GROUPS);

    if (does_sender_have_permissions_to_add_group == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to add group \n");
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id_to_add_group_to = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_group_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_id");

    if (g_server_settings.is_display_admin_group_active == FALSE && json_group_id->valueint == ADMIN_GROUP_ID)
    {
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    // check if client whose group is about to be added exists
    client_to_add_group_to = &g_clients_array[json_client_id_to_add_group_to->valueint];

    if (client_to_add_group_to->is_authenticated == FALSE || client_to_add_group_to->is_existing == FALSE)
    {
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    if ((uint64)json_group_id->valueint == g_guest_group_id
        || permissions__can_change_member(client, client_to_add_group_to, (uint64)json_group_id->valueint, TRUE) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    // check if the group itself that is about to be added exists
    status = server_admin__is_group_id_real(json_group_id->valueint);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "group id : ", json_group_id->valueint, "does not exist \n");
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    // now check if the client already has the group id about to be added
    status = server_admin__is_client_already_assigned_this_group_id(json_client_id_to_add_group_to->valueint, json_group_id->valueint);
    if (status == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "group id : ", json_group_id->valueint, " already is assigned to client ", json_client_id_to_add_group_to->valueint, "\n");
        goto label_client_msg__process_add_group_to_client_message_end;
    }

    // at this point following is clear
    // - client has a permission to add this group id
    // - receiving client exists and doesn't have that group id yet
    // - group id is valid (exists)
    // - all that is left is to add that group id to the client, and make the client admin if the group id happens to be admin
    cvector_push_back(client_to_add_group_to->group_ids, json_group_id->valueint);

    if (json_group_id->valueint == ADMIN_GROUP_ID)
    {
        client_to_add_group_to->is_admin = TRUE;
        server_msg__administration__send_admin_status_to_single_client(client_to_add_group_to);
    }

    server_msg__administration__send_add_group_to_client_event_to_all_clients(client_to_add_group_to, json_group_id->valueint);
    _client_msg_administration_internal__refresh_guest_membership(client_to_add_group_to);
    powers_changed = TRUE;

    // an admin's flag may be withheld, so the rows showing the client learn the code they show from now on
    if (json_group_id->valueint == ADMIN_GROUP_ID && g_server_settings.is_admin_country_flag_hidden == TRUE)
    {
        server_msg__profile__send_client_country_code_changed_to_all_clients(client_to_add_group_to);
    }

    // mirror the change into the ram identity store immediately, so the group survives a reconnect
    // within this server run without needing an admin disk save
    identity_store__sync_client_identity_in_store(client_to_add_group_to);

    server_logs__group_added(json_group_id->valueint, client_to_add_group_to->username, client->username);

    // send admin status to other clients possibly, or not, do they have to know you are an admin
label_client_msg__process_add_group_to_client_message_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    if (client_to_add_group_to != NULL_POINTER && client_to_add_group_to->is_existing == TRUE && client_to_add_group_to->is_authenticated == TRUE)
    {
        if (powers_changed == TRUE)
        {
            audio_channel__apply_permission_changes();
            video_channel__apply_permission_changes();
        }
        server_msg__administration__send_permission_update_to_single_client(client_to_add_group_to);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a remove-group-from-client request: client id and group id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_remove_group_from_client_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_group_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE
        || json_client_id->valuedouble != (double)json_client_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_remove_group_from_client_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_remove_group_from_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_group_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_id");
    if (cJSON_IsNumber(json_group_id) == FALSE
        || json_group_id->valuedouble != (double)json_group_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_remove_group_from_client_valid cJSON_IsNumber(group_id) \n");
        return FALSE;
    }

    if (json_group_id->valueint < 0 || json_group_id->valueint >= MAX_GROUPS)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_remove_group_from_client_valid json_group_id->valueint is not valid ", json_group_id->valueint, "\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Checks permission and removes a group from a connected client.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_remove_group_from_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole powers_changed = FALSE;
    boole status = FALSE;
    boole does_sender_have_permissions_to_remove_group = FALSE;
    client_t* client = 0;
    client_t* client_to_remove_group_id_from = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id_to_remove_group_id_from = 0;
    cJSON* json_group_id = 0;
    int64 group_id_index = 0;

    status = _client_msg_administration_internal__is_remove_group_from_client_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_remove_group_from_client_message _client_msg_administration_internal__is_remove_group_from_client_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }


    clib__read_lock(&g_groups_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    // check if client that is sending request has permission to remove groups
    does_sender_have_permissions_to_remove_group = permissions__is_allowed_in_groups(client, PERMISSION_REMOVE_GROUPS);

    if (does_sender_have_permissions_to_remove_group == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to remove group \n");
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id_to_remove_group_id_from = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_group_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_id");

    // check if group happens to be admin group
    if (g_server_settings.is_display_admin_group_active == FALSE && json_group_id->valueint == ADMIN_GROUP_ID)
    {
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    // check if client whose group is about to be removed exists
    client_to_remove_group_id_from = &g_clients_array[json_client_id_to_remove_group_id_from->valueint];

    if (client_to_remove_group_id_from->is_authenticated == FALSE || client_to_remove_group_id_from->is_existing == FALSE)
    {
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    if ((uint64)json_group_id->valueint == g_guest_group_id
        || permissions__can_change_member(client, client_to_remove_group_id_from, (uint64)json_group_id->valueint, FALSE) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    // check if the group itself that is about to be removed exists
    status = server_admin__is_group_id_real(json_group_id->valueint);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "group id : ", json_group_id->valueint, "does not exist \n");
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    // now check if the client actually has the group id about to be removed
    status = server_admin__is_client_already_assigned_this_group_id(json_client_id_to_remove_group_id_from->valueint, json_group_id->valueint);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "group id : ", json_group_id->valueint, " already is not assigned to client ", json_client_id_to_remove_group_id_from->valueint, "\n");
        goto label_client_msg__process_remove_group_from_client_message_end;
    }

    // at this point following is clear
    // - client has a permission to remove this group id
    // - receiving client exists and has that group id
    // - group id is valid (exists)
    // - all that is left is to remove that group id from client, and revoke admin if group id happens to be admin
    group_id_index = server_admin__get_index_of_group_id_of_client(json_client_id_to_remove_group_id_from->valueint, json_group_id->valueint);

    if (group_id_index != -1)
    {
        cvector_erase(client_to_remove_group_id_from->group_ids, group_id_index);

        if (json_group_id->valueint == ADMIN_GROUP_ID)
        {
            client_to_remove_group_id_from->is_admin = FALSE;
            server_msg__administration__send_admin_status_to_single_client(client_to_remove_group_id_from);
        }

        server_msg__administration__send_remove_group_from_client_event_to_all_clients(client_to_remove_group_id_from, json_group_id->valueint);
        _client_msg_administration_internal__refresh_guest_membership(client_to_remove_group_id_from);
        powers_changed = TRUE;

        // the client's flag was withheld as an admin's; the rows showing the client get the real code back
        if (json_group_id->valueint == ADMIN_GROUP_ID && g_server_settings.is_admin_country_flag_hidden == TRUE)
        {
            server_msg__profile__send_client_country_code_changed_to_all_clients(client_to_remove_group_id_from);
        }

        // mirror the removal into the ram identity store immediately (drops the identity entirely
        // if it now wears no groups), so a reconnect does not resurrect a group that was just removed
        identity_store__sync_client_identity_in_store(client_to_remove_group_id_from);
    }

label_client_msg__process_remove_group_from_client_message_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    if (client_to_remove_group_id_from != NULL_POINTER && client_to_remove_group_id_from->is_existing == TRUE && client_to_remove_group_id_from->is_authenticated == TRUE)
    {
        if (powers_changed == TRUE)
        {
            audio_channel__apply_permission_changes();
            video_channel__apply_permission_changes();
        }
        server_msg__administration__send_permission_update_to_single_client(client_to_remove_group_id_from);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a server-settings icon upload: the base64 icon string is present and within size limits
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_process_server_settings_icon_upload_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* base64_icon_value = 0;
    uint64 base64_icon_length = 0;
    uint64 max_base64_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    base64_icon_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_icon_value");
    if (cJSON_IsString(base64_icon_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_icon_upload_message_valid cJSON_IsString(base64_icon_value)");
        return FALSE;
    }

    base64_icon_length = clib__utf8_string_length(base64_icon_value->valuestring);

    // the raw-bytes setting caps the upload: base64 is ~4/3 of raw plus a small data-url prefix.
    // the store buffer ceiling stays as the hard limit behind it
    max_base64_length = (uint64)((g_server_settings.icon_max_size_bytes * 4) / 3) + 64;
    if (base64_icon_length >= ICON_MAX_LENGTH || base64_icon_length > max_base64_length || base64_icon_length < 64)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "icon upload dropped: base64 length", base64_icon_length, "is outside the allowed range \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Checks permission and stores a new server icon.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_icon_upload(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_upload_icon = FALSE;
    uint64 icon_index = 0;
    icon_t* found_icon = 0;
    cJSON* base64_icon_value = 0;
    cJSON* json_message_object = 0;

    status = _client_msg_administration_internal__is_process_server_settings_icon_upload_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_icon_upload_message_valid is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    base64_icon_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_icon_value");

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_icons_global_rwlock_guard);

    does_sender_have_permissions_to_upload_icon = permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_MANAGE_ICONS);

    if (does_sender_have_permissions_to_upload_icon == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to upload icon \n");
        goto label_client_msg__process_server_settings_icon_upload_end;
    }

    if (base__is_server_ram_budget_reached())
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "The server has reached its RAM budget. Try uploading again later.");
        goto label_client_msg__process_server_settings_icon_upload_end;
    }

    // sender has necessary permission to upload new icon.
    // find out if there is free slot for icon in the array
    for (icon_index = 0; icon_index < MAX_ICONS; icon_index++)
    {
        icon_t* icon = &g_icons_array[icon_index];
        if (icon->is_existing == FALSE)
        {
            if (server_admin__set_icon_base64(icon, base64_icon_value->valuestring) == FALSE)
            {
                break; // out of memory: the slot stays free and nothing is announced
            }
            found_icon = icon;
            found_icon->is_existing = TRUE;
            found_icon->id = icon_index;

            break;
        }
    }

    if (found_icon == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "found icon is null");
        goto label_client_msg__process_server_settings_icon_upload_end;
    }

    // everything is checked
    // write new icon and notify users
    server_msg__administration__send_add_new_icon_event_to_all_clients(found_icon->id, found_icon->base64);

label_client_msg__process_server_settings_icon_upload_end:
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates an add-new-group request: the group name and linked icon id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check, it doesnt access any of array structures array_icons or array_groups
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_process_server_settings_add_new_group_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_group_name = 0;
    cJSON* json_linked_icon_id = 0;
    uint64 new_group_name_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_group_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_name");

    if (cJSON_IsString(json_group_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_add_new_group_message_valid cJSON_IsString(json_group_name)");
        return FALSE;
    }

    new_group_name_length = clib__utf8_string_length(json_group_name->valuestring);

    if (new_group_name_length >= GROUP_MAX_NAME_LENGTH || new_group_name_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_add_new_group_message_valid (clib__utf8_string_length(json_group_name->valuestring) >= GROUP_MAX_NAME_LENGTH)");
        return FALSE;
    }

    // the linked icon is optional; a group may be created without one. only validate it when present
    json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
    if (json_linked_icon_id != NULL_POINTER)
    {
        if (cJSON_IsNumber(json_linked_icon_id) == FALSE
            || json_linked_icon_id->valuedouble != (double)json_linked_icon_id->valueint)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_add_new_group_message_valid cJSON_IsNumber(json_linked_icon_id)");
            return FALSE;
        }

        if (json_linked_icon_id->valueint < 0 || json_linked_icon_id->valueint >= MAX_ICONS)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "icon id is invalid");
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief Creates a group using the selected permission preset.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_add_group(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_add_new_group_to_server = FALSE;
    cJSON* json_linked_icon_id = 0;
    cJSON* json_group_name = 0;
    cJSON* json_group_type = NULL_POINTER;
    group_type_t group_type = GROUP_TYPE_NORMAL;
    group_permissions_t preset_permissions;
    cJSON* json_message_object = 0;
    group_t* newly_added_group = 0;
    group_t* group_in_loop = 0;
    uint64 group_index = 0;
    boole is_same_group_name_found = FALSE;

    status = _client_msg_administration_internal__is_process_server_settings_add_new_group_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_add_group is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
    json_group_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_name");
    json_group_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_type");
    if (json_group_type != NULL_POINTER
        && (cJSON_IsString(json_group_type) == FALSE
            || permissions__parse_group_type(json_group_type->valuestring, &group_type) == FALSE))
    {
        return;
    }
    permissions__set_preset(group_type, &preset_permissions);

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_groups_global_rwlock_guard);

    // the linked icon is optional. when one is given it must reference an existing icon (id equals icon index,
    // range already checked in the validator); when none is given the group is created without an icon
    if (json_linked_icon_id != NULL_POINTER)
    {
        status = g_icons_array[json_linked_icon_id->valueint].is_existing;

        if (status == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_add_group linked icon id does not exist \n");
            goto label_client_msg__process_server_settings_add_new_group_end;
        }
    }

    // check if client that is sending request has permission to create new server groups
    does_sender_have_permissions_to_add_new_group_to_server = permissions__can_create_group(&g_clients_array[sender_client_id], &preset_permissions);

    if (does_sender_have_permissions_to_add_new_group_to_server == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__administration__process_add_group sender with sender_client_id", sender_client_id, "does not have permission to add newly_added_group \n");
        goto label_client_msg__process_server_settings_add_new_group_end;
    }

    // do not allow creation of new group if group with same name already exists
    for (group_index = 0; group_index < MAX_GROUPS; group_index++)
    {
        group_in_loop = &g_groups_array[group_index];
        if (group_in_loop->is_existing == TRUE)
        {
            status = clib__is_string_equal(json_group_name->valuestring, group_in_loop->name);

            if (status == TRUE)
            {
                is_same_group_name_found = TRUE;
                break;
            }
        }
    }

    if (is_same_group_name_found == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", " is_same_group_name_found == TRUE", json_group_name->valuestring, "\n");
        goto label_client_msg__process_server_settings_add_new_group_end;
    }

    // at this point client is verified, icon id is correct, newly_added_group length is correct
    // start at index 1: id 0 is ADMIN_GROUP_ID (reserved for the admin group). a user group at id 0 would collide
    // with it - adding it would grant admin, and adding/removing it would add/remove the admin group.
    for (group_index = 1; group_index < MAX_GROUPS; group_index++)
    {
        group_in_loop = &g_groups_array[group_index];

        if (group_in_loop->is_existing == FALSE)
        {
            clib__null_memory(group_in_loop, sizeof(*group_in_loop));
            group_in_loop->is_existing = TRUE;
            group_in_loop->group_type = group_type;
            group_in_loop->permissions = preset_permissions;
            if (json_linked_icon_id != NULL_POINTER)
            {
                group_in_loop->has_icon = TRUE;
                group_in_loop->icon_id = (uint64)json_linked_icon_id->valueint;
            }
            else
            {
                group_in_loop->has_icon = FALSE;
                group_in_loop->icon_id = 0;
            }
            clib__copy_memory((void*)&json_group_name->valuestring[0], (void*)&group_in_loop->name[0], clib__utf8_string_length(json_group_name->valuestring), GROUP_MAX_NAME_LENGTH);
            group_in_loop->id = group_index;

            newly_added_group = group_in_loop;
            break;
        }
    }

    if (newly_added_group == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "newly_added_group == NULL_POINTER");
        goto label_client_msg__process_server_settings_add_new_group_end;
    }

    server_msg__administration__send_create_new_group_event_to_all_clients(group_in_loop->id, group_in_loop->name, group_in_loop->icon_id, group_in_loop->has_icon);

label_client_msg__process_server_settings_add_new_group_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    server_msg__administration__send_permission_updates_to_all_clients();
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks permission to delete a group from the server's group pool. the group is stripped from every
 *        client that currently holds it, its pool slot is freed, and all clients are told to remove it. the
 *        admin group (ADMIN_GROUP_ID) can never be deleted this way.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_delete_group(cJSON* json_root, uint64 sender_client_id)
{
    boole powers_changed = FALSE;
    client_t* sender_client = 0;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_group_id = 0;
    uint64 group_id = 0;
    uint64 i = 0;
    int64 group_id_index = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_group_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_id");

    if (cJSON_IsNumber(json_group_id) == FALSE
        || json_group_id->valuedouble != (double)json_group_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_delete_group: group_id missing or not a number");
        return;
    }

    group_id = (uint64)json_group_id->valueint;

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_groups_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (permissions__can_delete_group(sender_client, group_id) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_set_server_settings_delete_group_end;
    }

    // Admin and the automatic Guest group must remain available.
    if (group_id == ADMIN_GROUP_ID || group_id == g_guest_group_id)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_set_server_settings_delete_group_end;
    }

    if (server_admin__is_group_id_real(group_id) == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_group_end;
    }

    // remove the group from every client that currently holds it, so server state stays consistent
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE)
        {
            continue;
        }

        group_id_index = server_admin__get_index_of_group_id_of_client(client->client_id, group_id);
        if (group_id_index != -1)
        {
            cvector_erase(client->group_ids, group_id_index);
            _client_msg_administration_internal__refresh_guest_membership(client);
        }
    }

    identity_store__remove_group_from_all_identities(group_id);

    // free the group's pool slot (a group id equals its index in g_groups_array)
    g_groups_array[group_id].is_existing = FALSE;
    powers_changed = TRUE;
    g_groups_array[group_id].icon_id = 0;
    clib__null_memory(&g_groups_array[group_id].name[0], GROUP_MAX_NAME_LENGTH);

    server_msg__administration__send_remove_group_event_to_all_clients(group_id);

label_client_msg__process_set_server_settings_delete_group_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    if (powers_changed == TRUE)
    {
        audio_channel__apply_permission_changes();
        video_channel__apply_permission_changes();
    }
    server_msg__administration__send_permission_updates_to_all_clients();
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin asks for the identity-management list; replies with every stored identity (hash, its
 *        groups, and whether it is currently online). Requires permission to view group members.
 *
 * @param cJSON* json_root -> the parsed request (no fields needed beyond type)
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__administration__process_request_identity_list(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (permissions__is_allowed(sender_client, PERMISSION_VIEW_GROUP_MEMBERS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_request_identity_list_end;
    }

    server_msg__administration__send_identity_list_to_single_client(sender_client);

label_client_msg__process_request_identity_list_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin-only, deletes one stored identity addressed by its public-key hash
 *
 *        the identity leaves the RAM store. if its holder is connected right now, their groups are
 *        stripped and admin is revoked on the live client, and each removal is broadcast one event
 *        at a time because there is no multi-group-removal event. the on-disk copy is only dropped on
 *        the next "save server settings". the requesting admin then gets a refreshed identity list.
 *
 * @param cJSON* json_root -> the parsed request; message.public_key_hash identifies the identity
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__administration__process_delete_identity(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    client_t* holder = 0;
    cJSON* json_message_object = 0;
    cJSON* json_hash = 0;
    char target_hash[BASE64_ENCODE_OUT_SIZE(32)];
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 groups_to_remove[MAX_GROUPS_FOR_SINGLE_CLIENT];
    uint64 groups_to_remove_count = 0;
    uint64 holder_client_id = 0;
    boole holder_was_admin = FALSE;
    boole holder_found = FALSE;
    uint64 c = 0;
    uint64 t = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");

    if (cJSON_IsString(json_hash) == FALSE || json_hash->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_delete_identity: public_key_hash missing or not a string \n");
        return;
    }

    clib__null_memory(target_hash, sizeof(target_hash));
    clib__copy_memory(json_hash->valuestring, target_hash, clib__utf8_string_length(json_hash->valuestring), sizeof(target_hash) - 1);

    clib__write_lock(&g_clients_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_delete_identity_end;
    }

    // if this identity is used by a connected client right now, remove every group from them
    for (c = 0; c < g_server_settings.max_client_count; c++)
    {
        holder = &g_clients_array[c];

        if (holder->is_existing == FALSE || holder->is_authenticated == FALSE || holder->is_music_bot == TRUE)
        {
            continue;
        }
        if (holder->public_key[0] == 0)
        {
            continue;
        }

        util__hash_password_to_base64(holder->public_key, client_hash, sizeof(client_hash));
        if (clib__is_string_equal(client_hash, target_hash) == FALSE)
        {
            continue;
        }

        holder_client_id = holder->client_id;
        holder_found = TRUE;

        // snapshot the group ids first, then clear them off the client and revoke admin
        groups_to_remove_count = 0;
        for (t = 0; t < cvector_size(holder->group_ids) && groups_to_remove_count < MAX_GROUPS_FOR_SINGLE_CLIENT; t++)
        {
            groups_to_remove[groups_to_remove_count] = (uint64)holder->group_ids[t];
            groups_to_remove_count++;
        }

        while (cvector_size(holder->group_ids) > 0)
        {
            cvector_erase(holder->group_ids, 0);
        }
        holder_was_admin = holder->is_admin;
        holder->is_admin = FALSE;
        server_msg__administration__send_admin_status_to_single_client(holder);

        break;
    }

    // drop the identity from the ram store (disk copy stays until the next settings save)
    identity_store__delete_identity_from_store_by_hash(target_hash);

    // tell everyone about each stripped group (one event per group; there is no bulk-removal event)
    for (t = 0; t < groups_to_remove_count; t++)
    {
        server_msg__administration__send_remove_group_from_client_event_to_all_clients(&g_clients_array[holder_client_id], groups_to_remove[t]);
    }

    if (holder_found == TRUE)
    {
        clib__read_lock(&g_groups_global_rwlock_guard);
        _client_msg_administration_internal__refresh_guest_membership(&g_clients_array[holder_client_id]);
        clib__unlock(&g_groups_global_rwlock_guard);
    }

    // the client's flag was withheld as an admin's; the rows showing the client get the real code back
    if (holder_was_admin == TRUE && g_server_settings.is_admin_country_flag_hidden == TRUE)
    {
        server_msg__profile__send_client_country_code_changed_to_all_clients(holder);
    }

    // refresh the requesting admin's identity list so the deleted row disappears
    server_msg__administration__send_identity_list_to_single_client(sender_client);

label_client_msg__process_delete_identity_end:
    if (holder_found == TRUE)
    {
        audio_channel__apply_permission_changes();
        video_channel__apply_permission_changes();
    }
    server_msg__administration__send_permission_updates_to_all_clients();
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks permission to add or remove a group from a saved identity,
 *        working on offline identities too
 *
 *        the store is updated, and if the identity's holder is connected right now the group is
 *        added or removed live on them and broadcast. persistence to disk waits for the next
 *        "save server settings". the requesting admin then gets a refreshed identity list.
 *
 * @param cJSON* json_root -> message.public_key_hash, message.group_id, message.add (bool)
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__administration__process_modify_identity_group(cJSON* json_root, uint64 sender_client_id)
{
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* hash = cJSON_GetObjectItemCaseSensitive(message, "public_key_hash");
    cJSON* id = cJSON_GetObjectItemCaseSensitive(message, "group_id");
    cJSON* json_add = cJSON_GetObjectItemCaseSensitive(message, "add");
    client_t* sender = NULL_POINTER;
    client_t* target = NULL_POINTER;
    client_t stored_client;
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 group_id = 0;
    uint64 i = 0;
    uint64 t = 0;
    int64 group_index = -1;
    boole add = FALSE;
    boole changed = FALSE;

    memset(&stored_client, 0, sizeof(stored_client));
    if (cJSON_IsString(hash) == FALSE || hash->valuestring == NULL_POINTER
        || strlen(hash->valuestring) >= sizeof(client_hash) || cJSON_IsNumber(id) == FALSE
        || id->valuedouble != (double)id->valueint || id->valueint < 0 || id->valueint >= MAX_GROUPS
        || cJSON_IsBool(json_add) == FALSE)
    {
        return;
    }
    group_id = (uint64)id->valueint;
    add = (boole)cJSON_IsTrue(json_add);
    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_groups_global_rwlock_guard);
    sender = &g_clients_array[sender_client_id];
    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE
            || g_clients_array[i].is_music_bot == TRUE)
        {
            continue;
        }
        util__hash_password_to_base64(g_clients_array[i].public_key, client_hash, sizeof(client_hash));
        if (strcmp(client_hash, hash->valuestring) == 0)
        {
            target = &g_clients_array[i];
            break;
        }
    }
    if (target == NULL_POINTER)
    {
        pthread_mutex_lock(&g_client_stored_data_mutex);
        for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
        {
            if (strcmp(g_client_stored_data[i].public_key, hash->valuestring) != 0)
            {
                continue;
            }
            stored_client.is_existing = TRUE;
            stored_client.is_authenticated = TRUE;
            for (t = 0; t < g_client_stored_data[i].group_id_count; t++)
            {
                cvector_push_back(stored_client.group_ids, (int)g_client_stored_data[i].group_ids[t]);
            }
            target = &stored_client;
            break;
        }
        pthread_mutex_unlock(&g_client_stored_data_mutex);
    }
    if (group_id == g_guest_group_id || permissions__can_change_member(sender, target, group_id, add) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
        goto label_modify_identity_group_end;
    }
    if (target == &stored_client)
    {
        changed = identity_store__modify_identity_group_in_store(hash->valuestring, group_id, add);
    }
    else
    {
        group_index = server_admin__get_index_of_group_id_of_client(target->client_id, group_id);
        if (add == TRUE && group_index == -1)
        {
            cvector_push_back(target->group_ids, (int)group_id);
            changed = TRUE;
        }
        else if (add == FALSE && group_index != -1)
        {
            cvector_erase(target->group_ids, group_index);
            changed = TRUE;
        }
        if (changed == TRUE)
        {
            if (group_id == ADMIN_GROUP_ID)
            {
                target->is_admin = add;
                server_msg__administration__send_admin_status_to_single_client(target);
                if (g_server_settings.is_admin_country_flag_hidden == TRUE)
                {
                    server_msg__profile__send_client_country_code_changed_to_all_clients(target);
                }
            }
            if (add == TRUE)
            {
                server_msg__administration__send_add_group_to_client_event_to_all_clients(target, group_id);
            }
            else
            {
                server_msg__administration__send_remove_group_from_client_event_to_all_clients(target, group_id);
            }
            _client_msg_administration_internal__refresh_guest_membership(target);
            identity_store__sync_client_identity_in_store(target);
        }
    }
label_modify_identity_group_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    if (target != NULL_POINTER && target != &stored_client)
    {
        if (changed == TRUE)
        {
            audio_channel__apply_permission_changes();
            video_channel__apply_permission_changes();
        }
        server_msg__administration__send_permission_update_to_single_client(target);
    }
    if (changed == TRUE)
    {
        server_msg__administration__send_identity_list_to_single_client(sender);
    }
    cvector_free(stored_client.group_ids);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks permission to delete an icon from the server's icon pool. groups that still reference the
 *        deleted icon keep their icon_id, but their image no longer resolves.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_delete_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_icon_id = 0;
    uint64 icon_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_icon_id) == FALSE
        || json_icon_id->valuedouble != (double)json_icon_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_delete_icon: icon_id missing or not a number");
        return;
    }

    icon_id = (uint64)json_icon_id->valueint;

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_icons_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (permissions__is_allowed(sender_client, PERMISSION_MANAGE_ICONS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_set_server_settings_delete_icon_end;
    }

    if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_icon_end;
    }

    // free the icon's pool slot (an icon id equals its index in g_icons_array)
    g_icons_array[icon_id].is_existing = FALSE;
    server_admin__free_icon_base64(&g_icons_array[icon_id]);

    server_msg__administration__send_remove_icon_event_to_all_clients(icon_id);

label_client_msg__process_set_server_settings_delete_icon_end:
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks permission to change a group's icon. an icon_id in the message assigns that
 *        icon (it must exist); an absent icon_id clears the group's icon. the change is broadcast to all clients.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_set_group_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_group_id = 0;
    cJSON* json_icon_id = 0;
    uint64 group_id = 0;
    uint64 icon_id = 0;
    boole wants_icon = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_group_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "group_id");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_group_id) == FALSE
        || json_group_id->valuedouble != (double)json_group_id->valueint)
    {
        return;
    }

    group_id = (uint64)json_group_id->valueint;
    if (json_icon_id != NULL_POINTER && cJSON_IsNumber(json_icon_id) == FALSE)
    {
        return;
    }
    wants_icon = cJSON_IsNumber(json_icon_id);
    if (wants_icon == TRUE)
    {
        if (json_icon_id->valuedouble != (double)json_icon_id->valueint)
        {
            return;
        }
        icon_id = (uint64)json_icon_id->valueint;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_groups_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (permissions__can_manage_group(sender_client, group_id, PERMISSION_EDIT_GROUPS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender_client, NULL_POINTER);
        goto label_client_msg__process_set_server_settings_set_group_icon_end;
    }

    if (server_admin__is_group_id_real(group_id) == FALSE)
    {
        goto label_client_msg__process_set_server_settings_set_group_icon_end;
    }

    if (wants_icon == TRUE)
    {
        // the assigned icon must exist (an icon id equals its index)
        if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
        {
            goto label_client_msg__process_set_server_settings_set_group_icon_end;
        }

        g_groups_array[group_id].has_icon = TRUE;
        g_groups_array[group_id].icon_id = icon_id;
    }
    else
    {
        // no icon_id in the request -> clear the group's icon
        g_groups_array[group_id].has_icon = FALSE;
        g_groups_array[group_id].icon_id = 0;
    }

    server_msg__administration__send_group_icon_changed_event_to_all_clients(group_id, g_groups_array[group_id].has_icon, g_groups_array[group_id].icon_id);

label_client_msg__process_set_server_settings_set_group_icon_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks permission to read the server log and replies with every stored line, oldest
 *        first. only an authenticated admin gets a reply - anyone else is silently ignored
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_admin_log_request(cJSON* json_root, uint64 sender_client_id)
{
    boole is_sender_admin = FALSE;

    (void)json_root; // the request has no fields to read

    // the reply is sent under the clients read lock, which also covers reading the sender's slot
    clib__read_lock(&g_clients_global_rwlock_guard);

    is_sender_admin = permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_VIEW_SERVER_LOGS);
    if (is_sender_admin == TRUE)
    {
        server_msg__administration__send_admin_log_to_single_client(&g_clients_array[sender_client_id]);
    }
    else
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__administration__process_admin_log_request sender with sender_client_id", sender_client_id, "is not an admin");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to clear the admin log. the clearing itself becomes the first line
 *        of the fresh log, so with several admins it stays visible who emptied it. the refreshed
 *        log is sent back right away. Other clients receive an insufficient permissions alert
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_admin_log_clear(cJSON* json_root, uint64 sender_client_id)
{
    boole is_sender_admin = FALSE;

    (void)json_root; // the request has no fields to read

    clib__read_lock(&g_clients_global_rwlock_guard);

    is_sender_admin = base__is_client_valid_admin(sender_client_id);
    if (is_sender_admin == TRUE)
    {
        server_logs__cleared_by(g_clients_array[sender_client_id].username);

        server_msg__administration__send_admin_log_to_single_client(&g_clients_array[sender_client_id]);
    }
    else
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__administration__process_admin_log_clear sender with sender_client_id", sender_client_id, "is not an admin");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a kick/ban request: the target client id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_kick_ban_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE
        || json_client_id->valuedouble != (double)json_client_id->valueint)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_kick_ban_request_valid cJSON_IsNumber(json_client_id)");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_client_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Checks kick permission and protection before disconnecting a client.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_kick_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    client_t* admin = 0;
    client_t* receiver = 0;

    status = _client_msg_administration_internal__is_kick_ban_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_kick_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE)
    {
        goto label_client_msg__process_kick_request_end;
    }

    receiver = &g_clients_array[json_client_id->valueint];
    if (receiver->is_authenticated == FALSE || receiver->is_existing == FALSE)
    {
        goto label_client_msg__process_kick_request_end;
    }

    if (permissions__can_moderate(admin, receiver, PERMISSION_KICK) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_kick_request_end;
    }

    server_logs__client_kicked(receiver->username, admin->username);

    ws_close_client(receiver->p_ws_connection);

label_client_msg__process_kick_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks ban permission and protection before banning a client.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_ban_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    client_t* admin = 0;
    client_t* receiver = 0;
    boole should_ban = FALSE;
    char banned_ip[BAN_IP_MAX_LENGTH];
    char banned_country[COUNTRY_ISO_CODE_LENGTH];
    char banned_identity[MAX_PUBLIC_KEY_LENGTH];

    clib__null_memory(banned_ip, sizeof(banned_ip));
    clib__null_memory(banned_country, sizeof(banned_country));
    clib__null_memory(banned_identity, sizeof(banned_identity));

    status = _client_msg_administration_internal__is_kick_ban_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_ban_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE)
    {
        goto label_client_msg__process_ban_request_end;
    }

    receiver = &g_clients_array[json_client_id->valueint];
    if (receiver->is_authenticated == FALSE || receiver->is_existing == FALSE)
    {
        goto label_client_msg__process_ban_request_end;
    }

    if (permissions__can_moderate(admin, receiver, PERMISSION_BAN) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_ban_request_end;
    }

    // snapshot the target's identifying data while we hold the clients lock, then disconnect them
    clib__copy_memory(&receiver->ip_address[0], banned_ip, clib__utf8_string_length(&receiver->ip_address[0]), BAN_IP_MAX_LENGTH - 1);
    clib__copy_memory(&receiver->country_iso_code[0], banned_country, clib__utf8_string_length(&receiver->country_iso_code[0]), COUNTRY_ISO_CODE_LENGTH - 1);
    clib__copy_memory(&receiver->public_key[0], banned_identity, clib__utf8_string_length(&receiver->public_key[0]), MAX_PUBLIC_KEY_LENGTH - 1);
    should_ban = TRUE;

    server_logs__client_banned(receiver->username, &receiver->ip_address[0], admin->username);

    ws_close_client(receiver->p_ws_connection);

label_client_msg__process_ban_request_end:

    // Keep the clients lock through persistence. Lock order puts bans last,
    // after channels, icons and groups.
    if (should_ban == TRUE)
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        clib__read_lock(&g_icons_global_rwlock_guard);
        clib__read_lock(&g_groups_global_rwlock_guard);
        clib__write_lock(&g_bans_global_rwlock_guard);

        server_admin__add_ban(banned_ip, banned_country, banned_identity, "");
        settings__save_server_settings_to_file();

        clib__unlock(&g_bans_global_rwlock_guard);
        clib__unlock(&g_groups_global_rwlock_guard);
        clib__unlock(&g_icons_global_rwlock_guard);
        clib__unlock(&g_channels_global_rwlock_guard);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to remove one ban (by ip address) from the persisted ban list
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_remove_ban_request(cJSON* json_root, uint64 sender_client_id)
{
    cJSON* json_message_object = 0;
    cJSON* json_ip = 0;
    boole is_admin = FALSE;
    boole removed = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_ip = cJSON_GetObjectItemCaseSensitive(json_message_object, "ip_address");
    if (cJSON_IsString(json_ip) == FALSE || json_ip->valuestring == NULL_POINTER)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    is_admin = (boole)(g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE && g_clients_array[sender_client_id].is_admin == TRUE);

    if (is_admin == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_groups_global_rwlock_guard);
    clib__write_lock(&g_bans_global_rwlock_guard);

    removed = server_admin__remove_ban_by_ip(json_ip->valuestring);
    if (removed == TRUE)
    {
        settings__save_server_settings_to_file();
    }

    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Sends connection details only when the sender has permission to view them.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_get_client_info_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    client_t* admin = 0;
    client_t* target = 0;

    status = _client_msg_administration_internal__is_kick_ban_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_get_client_info_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__read_lock(&g_clients_global_rwlock_guard);

    admin = &g_clients_array[sender_client_id];

    if (permissions__is_allowed(admin, PERMISSION_VIEW_CLIENT_DETAILS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(admin, NULL_POINTER);
        goto label_client_msg__process_get_client_info_request_end;
    }

    target = &g_clients_array[json_client_id->valueint];
    if (target->is_authenticated == FALSE || target->is_existing == FALSE)
    {
        goto label_client_msg__process_get_client_info_request_end;
    }

    server_msg__administration__send_client_info_to_single_client(admin, target);

label_client_msg__process_get_client_info_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks and applies permission changes to an existing group.
 * @param json_root -> request containing the group and all permission values
 * @param sender_client_id -> client requesting the change
 */
void client_msg__administration__process_set_group_permissions(cJSON* json_root, uint64 sender_client_id)
{
    boole powers_changed = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* id = cJSON_GetObjectItemCaseSensitive(message, "group_id");
    group_permissions_t permissions;
    client_t* sender = NULL_POINTER;
    uint64 group_id = 0;

    if (sender_client_id >= g_server_settings.max_client_count || cJSON_IsNumber(id) == FALSE
        || !(id->valuedouble >= ADMIN_GROUP_ID && id->valuedouble < MAX_GROUPS)
        || id->valuedouble != (double)id->valueint
        || permissions__read(cJSON_GetObjectItemCaseSensitive(message, "permissions"), &permissions) == FALSE)
    {
        return;
    }
    group_id = (uint64)id->valueint;
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (g_server_settings.is_permission_system_enabled == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], "Server groups and icons are disabled.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__write_lock(&g_groups_global_rwlock_guard);
    sender = &g_clients_array[sender_client_id];
    if (permissions__can_edit_group_permissions(sender, group_id, &permissions) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
    }
    else if (sender->is_existing == TRUE && sender->is_authenticated == TRUE
        && g_groups_array[group_id].is_existing == TRUE)
    {
        g_groups_array[group_id].permissions = permissions;
        powers_changed = TRUE;
        permissions__refresh_all_client_powers();
        server_msg__administration__send_group_permissions_changed(group_id);
    }
    clib__unlock(&g_groups_global_rwlock_guard);
    if (powers_changed == TRUE)
    {
        audio_channel__apply_permission_changes();
        video_channel__apply_permission_changes();
    }
    server_msg__administration__send_permission_updates_to_all_clients();
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Changes a group's display name after checking its protection.
 * @param json_root -> request containing group_id and group_name
 * @param sender_client_id -> client requesting the change
 */
void client_msg__administration__process_rename_group(cJSON* json_root, uint64 sender_client_id)
{
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* id = cJSON_GetObjectItemCaseSensitive(message, "group_id");
    cJSON* name = cJSON_GetObjectItemCaseSensitive(message, "group_name");
    uint64 i = 0;

    if (cJSON_IsNumber(id) == FALSE || id->valuedouble != (double)id->valueint
        || id->valueint < 0 || id->valueint >= MAX_GROUPS || cJSON_IsString(name) == FALSE
        || name->valuestring == NULL_POINTER || name->valuestring[0] == 0 || strlen(name->valuestring) >= GROUP_MAX_NAME_LENGTH)
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_groups_global_rwlock_guard);
    if (permissions__can_manage_group(&g_clients_array[sender_client_id], (uint64)id->valueint, PERMISSION_EDIT_GROUPS) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        goto label_rename_group_end;
    }
    for (i = 0; i < MAX_GROUPS; i++)
    {
        if (i != (uint64)id->valueint && g_groups_array[i].is_existing == TRUE && strcmp(g_groups_array[i].name, name->valuestring) == 0)
        {
            goto label_rename_group_end;
        }
    }
    memset(g_groups_array[id->valueint].name, 0, GROUP_MAX_NAME_LENGTH);
    memcpy(g_groups_array[id->valueint].name, name->valuestring, strlen(name->valuestring));
label_rename_group_end:
    clib__unlock(&g_groups_global_rwlock_guard);
    server_msg__administration__send_permission_updates_to_all_clients();
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Saves current group changes without accepting changes to server settings.
 * @param json_root -> request with no additional fields
 * @param sender_client_id -> client requesting the save
 */
void client_msg__administration__process_save_groups(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender = NULL_POINTER;
    boole allowed = FALSE;

    (void)json_root;
    clib__write_lock(&g_clients_global_rwlock_guard);
    sender = &g_clients_array[sender_client_id];
    allowed = (boole)(g_server_settings.is_permission_system_enabled == TRUE
        && (permissions__is_allowed(sender, PERMISSION_CREATE_GROUPS) == TRUE
            || permissions__is_allowed(sender, PERMISSION_EDIT_GROUPS) == TRUE
            || permissions__is_allowed(sender, PERMISSION_DELETE_GROUPS) == TRUE
            || permissions__is_allowed(sender, PERMISSION_ASSIGN_GROUPS) == TRUE
            || permissions__is_allowed(sender, PERMISSION_REMOVE_GROUPS) == TRUE
            || permissions__is_allowed(sender, PERMISSION_EDIT_GROUP_PERMISSIONS) == TRUE
            || permissions__is_allowed(sender, PERMISSION_MANAGE_ICONS) == TRUE));
    if (allowed == TRUE)
    {
        settings__save_server_settings_to_file();
    }
    else
    {
        server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}
