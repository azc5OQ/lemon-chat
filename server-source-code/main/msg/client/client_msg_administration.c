// This file checks administrator requests for passwords, bans, tags, icons, identities and logs.
#include "client_msg.h"
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
static boole _client_msg_administration_internal__is_add_tag_to_client_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_remove_tag_from_client_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_process_server_settings_icon_upload_message_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_process_server_settings_add_new_tag_message_valid(cJSON* json_root);
static boole _client_msg_administration_internal__is_kick_ban_request_valid(cJSON* json_root);

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
    boole is_tag_found = FALSE;

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

        if (g_server_settings.is_display_admin_tag_active == TRUE)
        {
            // only add the admin tag to the client if the client does not have it
            if (client->tag_ids != NULL_POINTER)
            {
                for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
                {
                    if (client->tag_ids[cvector_loop_index] == ADMIN_TAG_ID)
                    {
                        is_tag_found = TRUE;
                        break;
                    }
                }
            }

            if (is_tag_found == FALSE)
            {
                cvector_push_back(client->tag_ids, ADMIN_TAG_ID);
            }

            server_msg__administration__send_add_tag_to_client_event_to_all_clients(client, ADMIN_TAG_ID);
        }

        // an admin's flag may be withheld, so the rows showing the client learn the code they show from now on
        if (g_server_settings.is_admin_country_flag_hidden == TRUE)
        {
            server_msg__profile__send_client_country_code_changed_to_all_clients(client);
        }

        // tie this identity to its tags in ram right away, so a reconnect restores admin within this
        // server run even before any disk save. persistence to disk still needs a settings save
        identity_store__sync_client_identity_in_store(client);

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
 * @brief validates an add-tag-to-client request: client id and tag id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_add_tag_to_client_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_add_tag_to_client_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_add_tag_to_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_add_tag_to_client_valid cJSON_IsNumber(tag_id) \n");
        return FALSE;
    }

    if (json_tag_id->valueint < 0 || json_tag_id->valueint >= MAX_TAGS)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_add_tag_to_client_valid json_tag_id->valueint is not valid ", json_tag_id->valueint, "\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin request to add a tag to a client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_add_tag_to_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_add_tag = FALSE;
    client_t* client = 0;
    client_t* client_to_add_tag_to = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id_to_add_tag_to = 0;
    cJSON* json_tag_id = 0;

    status = _client_msg_administration_internal__is_add_tag_to_client_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_add_tag_to_client_message _client_msg_administration_internal__is_add_tag_to_client_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // check if client that is sending request has permission to add tags
    does_sender_have_permissions_to_add_tag = client->is_admin;

    if (does_sender_have_permissions_to_add_tag == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to add tag \n");
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id_to_add_tag_to = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

    if (g_server_settings.is_display_admin_tag_active == FALSE && json_tag_id->valueint == ADMIN_TAG_ID)
    {
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // check if client whose tag is about to be added exists
    client_to_add_tag_to = &g_clients_array[json_client_id_to_add_tag_to->valueint];

    if (client_to_add_tag_to->is_authenticated == FALSE || client_to_add_tag_to->is_existing == FALSE)
    {
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // check if the tag itself that is about to be added exists
    status = server_admin__is_tag_id_real(json_tag_id->valueint);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "tag id : ", json_tag_id->valueint, "does not exist \n");
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // now check if the client already has the tag id about to be added
    status = server_admin__is_client_already_assigned_this_tag_id(json_client_id_to_add_tag_to->valueint, json_tag_id->valueint);
    if (status == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "tag id : ", json_tag_id->valueint, " already is assigned to client ", json_client_id_to_add_tag_to->valueint, "\n");
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // at this point following is clear
    // - client has a permission to add this tag id
    // - receiving client exists and doesn't have that tag id yet
    // - tag id is valid (exists)
    // - all that is left is to add that tag id to the client, and make the client admin if the tag id happens to be admin
    cvector_push_back(client_to_add_tag_to->tag_ids, json_tag_id->valueint);

    if (json_tag_id->valueint == ADMIN_TAG_ID)
    {
        client_to_add_tag_to->is_admin = TRUE;
    }

    server_msg__administration__send_add_tag_to_client_event_to_all_clients(client_to_add_tag_to, json_tag_id->valueint);

    // an admin's flag may be withheld, so the rows showing the client learn the code they show from now on
    if (json_tag_id->valueint == ADMIN_TAG_ID && g_server_settings.is_admin_country_flag_hidden == TRUE)
    {
        server_msg__profile__send_client_country_code_changed_to_all_clients(client_to_add_tag_to);
    }

    // mirror the change into the ram identity store immediately, so the tag survives a reconnect
    // within this server run without needing an admin disk save
    identity_store__sync_client_identity_in_store(client_to_add_tag_to);

    server_logs__tag_added(json_tag_id->valueint, client_to_add_tag_to->username, client->username);

    // send admin status to other clients possibly, or not, do they have to know you are an admin
label_client_msg__process_add_tag_to_client_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a remove-tag-from-client request: client id and tag id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_remove_tag_from_client_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_remove_tag_from_client_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_remove_tag_from_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_remove_tag_from_client_valid cJSON_IsNumber(tag_id) \n");
        return FALSE;
    }

    if (json_tag_id->valueint < 0 || json_tag_id->valueint >= MAX_TAGS)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_administration_internal__is_remove_tag_from_client_valid json_tag_id->valueint is not valid ", json_tag_id->valueint, "\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin request to remove a tag from a client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_remove_tag_from_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_remove_tag = FALSE;
    client_t* client = 0;
    client_t* client_to_remove_tag_id_from = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id_to_remove_tag_id_from = 0;
    cJSON* json_tag_id = 0;
    int64 tag_id_index = 0;

    status = _client_msg_administration_internal__is_remove_tag_from_client_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_remove_tag_from_client_message _client_msg_administration_internal__is_remove_tag_from_client_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // check if client that is sending request has permission to remove tags
    does_sender_have_permissions_to_remove_tag = client->is_admin;

    if (does_sender_have_permissions_to_remove_tag == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to remove tag \n");
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id_to_remove_tag_id_from = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

    // check if tag happens to be admin tag
    if (g_server_settings.is_display_admin_tag_active == FALSE && json_tag_id->valueint == ADMIN_TAG_ID)
    {
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // check if client whose tag is about to be removed exists
    client_to_remove_tag_id_from = &g_clients_array[json_client_id_to_remove_tag_id_from->valueint];

    if (client_to_remove_tag_id_from->is_authenticated == FALSE || client_to_remove_tag_id_from->is_existing == FALSE)
    {
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // check if the tag itself that is about to be removed exists
    status = server_admin__is_tag_id_real(json_tag_id->valueint);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "tag id : ", json_tag_id->valueint, "does not exist \n");
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // now check if the client actually has the tag id about to be removed
    status = server_admin__is_client_already_assigned_this_tag_id(json_client_id_to_remove_tag_id_from->valueint, json_tag_id->valueint);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "tag id : ", json_tag_id->valueint, " already is not assigned to client ", json_client_id_to_remove_tag_id_from->valueint, "\n");
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // at this point following is clear
    // - client has a permission to remove this tag id
    // - receiving client exists and has that tag id
    // - tag id is valid (exists)
    // - all that is left is to remove that tag id from client, and revoke admin if tag id happens to be admin
    tag_id_index = server_admin__get_index_of_tag_id_of_client(json_client_id_to_remove_tag_id_from->valueint, json_tag_id->valueint);

    if (tag_id_index != -1)
    {
        cvector_erase(client_to_remove_tag_id_from->tag_ids, tag_id_index);

        if (json_tag_id->valueint == ADMIN_TAG_ID)
        {
            client_to_remove_tag_id_from->is_admin = FALSE;
        }

        server_msg__administration__send_remove_tag_from_client_event_to_all_clients(client_to_remove_tag_id_from, json_tag_id->valueint);

        // the client's flag was withheld as an admin's; the rows showing the client get the real code back
        if (json_tag_id->valueint == ADMIN_TAG_ID && g_server_settings.is_admin_country_flag_hidden == TRUE)
        {
            server_msg__profile__send_client_country_code_changed_to_all_clients(client_to_remove_tag_id_from);
        }

        // mirror the removal into the ram identity store immediately (drops the identity entirely
        // if it now wears no tags), so a reconnect does not resurrect a tag that was just removed
        identity_store__sync_client_identity_in_store(client_to_remove_tag_id_from);
    }

label_client_msg__process_remove_tag_from_client_message_end:
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
    if (base64_icon_length >= ICON_MAX_LENGTH || base64_icon_length > max_base64_length || base64_icon_length < 128)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "icon upload dropped: base64 length", base64_icon_length, "is outside the allowed range \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes an admin request to upload a new server icon
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
    clib__write_lock(&g_icons_global_rwlock_guard);

    does_sender_have_permissions_to_upload_icon = base__is_client_valid_admin(sender_client_id);

    if (does_sender_have_permissions_to_upload_icon == FALSE)
    {
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
 * @brief validates an add-new-tag request: the tag name and linked icon id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check, it doesnt access any of array structures array_icons or array_tags
 *
 * @return boole
 */
static boole _client_msg_administration_internal__is_process_server_settings_add_new_tag_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_tag_name = 0;
    cJSON* json_linked_icon_id = 0;
    uint64 new_tag_name_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_tag_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_name");

    if (cJSON_IsString(json_tag_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_add_new_tag_message_valid cJSON_IsString(json_tag_name)");
        return FALSE;
    }

    new_tag_name_length = clib__utf8_string_length(json_tag_name->valuestring);

    if (new_tag_name_length >= TAG_MAX_NAME_LENGTH || new_tag_name_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_add_new_tag_message_valid (clib__utf8_string_length(json_tag_name->valuestring) >= TAG_MAX_NAME_LENGTH)");
        return FALSE;
    }

    // the linked icon is optional; a tag may be created without one. only validate it when present
    json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
    if (json_linked_icon_id != NULL_POINTER)
    {
        if (cJSON_IsNumber(json_linked_icon_id) == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_administration_internal__is_process_server_settings_add_new_tag_message_valid cJSON_IsNumber(json_linked_icon_id)");
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
 * @brief processes an admin request to create a new tag with a linked icon
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_add_tag(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_add_new_tag_to_server = FALSE;
    cJSON* json_linked_icon_id = 0;
    cJSON* json_tag_name = 0;
    cJSON* json_message_object = 0;
    tag_t* newly_added_tag = 0;
    tag_t* tag_in_loop = 0;
    uint64 tag_index = 0;
    boole is_same_tag_name_found = FALSE;

    status = _client_msg_administration_internal__is_process_server_settings_add_new_tag_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_add_tag is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
    json_tag_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_name");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_tags_global_rwlock_guard);

    // the linked icon is optional. when one is given it must reference an existing icon (id equals icon index,
    // range already checked in the validator); when none is given the tag is created without an icon
    if (json_linked_icon_id != NULL_POINTER)
    {
        status = g_icons_array[json_linked_icon_id->valueint].is_existing;

        if (status == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_add_tag linked icon id does not exist \n");
            goto label_client_msg__process_server_settings_add_new_tag_end;
        }
    }

    // check if client that is sending request has permission to create new server tags
    does_sender_have_permissions_to_add_new_tag_to_server = base__is_client_valid_admin(sender_client_id);

    if (does_sender_have_permissions_to_add_new_tag_to_server == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__administration__process_add_tag sender with sender_client_id", sender_client_id, "does not have permission to add newly_added_tag \n");
        goto label_client_msg__process_server_settings_add_new_tag_end;
    }

    // do not allow creation of new tag if tag with same name already exists
    for (tag_index = 0; tag_index < MAX_TAGS; tag_index++)
    {
        tag_in_loop = &g_tags_array[tag_index];
        if (tag_in_loop->is_existing == TRUE)
        {
            status = clib__is_string_equal(json_tag_name->valuestring, tag_in_loop->name);

            if (status == TRUE)
            {
                is_same_tag_name_found = TRUE;
                break;
            }
        }
    }

    if (is_same_tag_name_found == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", " is_same_tag_name_found == TRUE", json_tag_name->valuestring, "\n");
        goto label_client_msg__process_server_settings_add_new_tag_end;
    }

    // at this point client is verified, icon id is correct, newly_added_tag length is correct
    // start at index 1: id 0 is ADMIN_TAG_ID (reserved for the admin tag). a user tag at id 0 would collide
    // with it - adding it would grant admin, and adding/removing it would add/remove the admin tag.
    for (tag_index = 1; tag_index < MAX_TAGS; tag_index++)
    {
        tag_in_loop = &g_tags_array[tag_index];

        if (tag_in_loop->is_existing == FALSE)
        {
            tag_in_loop->is_existing = TRUE;
            if (json_linked_icon_id != NULL_POINTER)
            {
                tag_in_loop->has_icon = TRUE;
                tag_in_loop->icon_id = (uint64)json_linked_icon_id->valueint;
            }
            else
            {
                tag_in_loop->has_icon = FALSE;
                tag_in_loop->icon_id = 0;
            }
            clib__copy_memory((void*)&json_tag_name->valuestring[0], (void*)&tag_in_loop->name[0], clib__utf8_string_length(json_tag_name->valuestring), TAG_MAX_NAME_LENGTH);
            tag_in_loop->id = tag_index;

            newly_added_tag = tag_in_loop;
            break;
        }
    }

    if (newly_added_tag == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "newly_added_tag == NULL_POINTER");
        goto label_client_msg__process_server_settings_add_new_tag_end;
    }

    server_msg__administration__send_create_new_tag_event_to_all_clients(tag_in_loop->id, tag_in_loop->name, tag_in_loop->icon_id, tag_in_loop->has_icon);

label_client_msg__process_server_settings_add_new_tag_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to delete a tag from the server's tag pool. the tag is stripped from every
 *        client that currently holds it, its pool slot is freed, and all clients are told to remove it. the
 *        admin tag (ADMIN_TAG_ID) can never be deleted this way.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_delete_tag(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_tag_id = 0;
    uint64 tag_id = 0;
    uint64 i = 0;
    int64 tag_id_index = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_delete_tag: tag_id missing or not a number");
        return;
    }

    tag_id = (uint64)json_tag_id->valueint;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_tags_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_tag_end;
    }

    // the admin tag is in slot 0 and is never a deletable pool tag
    if (tag_id == ADMIN_TAG_ID)
    {
        goto label_client_msg__process_set_server_settings_delete_tag_end;
    }

    if (server_admin__is_tag_id_real(tag_id) == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_tag_end;
    }

    // remove the tag from every client that currently holds it, so server state stays consistent
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE)
        {
            continue;
        }

        tag_id_index = server_admin__get_index_of_tag_id_of_client(client->client_id, tag_id);
        if (tag_id_index != -1)
        {
            cvector_erase(client->tag_ids, tag_id_index);
        }
    }

    // free the tag's pool slot (a tag id equals its index in g_tags_array)
    g_tags_array[tag_id].is_existing = FALSE;
    g_tags_array[tag_id].icon_id = 0;
    clib__null_memory(&g_tags_array[tag_id].name[0], TAG_MAX_NAME_LENGTH);

    server_msg__administration__send_remove_tag_event_to_all_clients(tag_id);

label_client_msg__process_set_server_settings_delete_tag_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin asks for the identity-management list; replies with every stored identity (hash, its
 *        tags, and whether it is currently online). admin-only.
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

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_request_identity_list_end;
    }

    server_msg__administration__send_identity_list_to_single_client(sender_client);

label_client_msg__process_request_identity_list_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin-only, deletes one stored identity addressed by its public-key hash
 *
 *        the identity leaves the RAM store. if its holder is connected right now, their tags are
 *        stripped and admin is revoked on the live client, and each removal is broadcast one event
 *        at a time because there is no multi-tag-removal event. the on-disk copy is only dropped on
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
    uint64 tags_to_remove[MAX_TAGS_FOR_SINGLE_CLIENT];
    uint64 tags_to_remove_count = 0;
    uint64 holder_client_id = 0;
    boole holder_was_admin = FALSE;
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
        goto label_client_msg__process_delete_identity_end;
    }

    // if this identity is used by a connected client right now, remove every tag from them
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

        // snapshot the tag ids first, then clear them off the client and revoke admin
        tags_to_remove_count = 0;
        for (t = 0; t < cvector_size(holder->tag_ids) && tags_to_remove_count < MAX_TAGS_FOR_SINGLE_CLIENT; t++)
        {
            tags_to_remove[tags_to_remove_count] = (uint64)holder->tag_ids[t];
            tags_to_remove_count++;
        }

        while (cvector_size(holder->tag_ids) > 0)
        {
            cvector_erase(holder->tag_ids, 0);
        }
        holder_was_admin = holder->is_admin;
        holder->is_admin = FALSE;

        break;
    }

    // drop the identity from the ram store (disk copy stays until the next settings save)
    identity_store__delete_identity_from_store_by_hash(target_hash);

    // tell everyone about each stripped tag (one event per tag; there is no bulk-removal event)
    for (t = 0; t < tags_to_remove_count; t++)
    {
        server_msg__administration__send_remove_tag_from_client_event_to_all_clients(&g_clients_array[holder_client_id], tags_to_remove[t]);
    }

    // the client's flag was withheld as an admin's; the rows showing the client get the real code back
    if (holder_was_admin == TRUE && g_server_settings.is_admin_country_flag_hidden == TRUE)
    {
        server_msg__profile__send_client_country_code_changed_to_all_clients(holder);
    }

    // refresh the requesting admin's identity list so the deleted row disappears
    server_msg__administration__send_identity_list_to_single_client(sender_client);

label_client_msg__process_delete_identity_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin-only, adds or removes a single tag on a stored identity addressed by its hash,
 *        working on offline identities too
 *
 *        the store is updated, and if the identity's holder is connected right now the tag is
 *        added or removed live on them and broadcast. persistence to disk waits for the next
 *        "save server settings". the requesting admin then gets a refreshed identity list.
 *
 * @param cJSON* json_root -> message.public_key_hash, message.tag_id, message.add (bool)
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__administration__process_modify_identity_tag(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    client_t* holder = 0;
    cJSON* json_message_object = 0;
    cJSON* json_hash = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_add = 0;
    char target_hash[BASE64_ENCODE_OUT_SIZE(32)];
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 tag_id = 0;
    boole add = FALSE;
    uint64 c = 0;
    int64 tag_index = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    json_add = cJSON_GetObjectItemCaseSensitive(json_message_object, "add");

    if (cJSON_IsString(json_hash) == FALSE || json_hash->valuestring == NULL_POINTER || cJSON_IsNumber(json_tag_id) == FALSE || cJSON_IsBool(json_add) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_modify_identity_tag: bad fields \n");
        return;
    }

    tag_id = (uint64)json_tag_id->valueint;
    add = (cJSON_IsTrue(json_add) == TRUE) ? TRUE : FALSE;

    clib__null_memory(target_hash, sizeof(target_hash));
    clib__copy_memory(json_hash->valuestring, target_hash, clib__utf8_string_length(json_hash->valuestring), sizeof(target_hash) - 1);

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_tags_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_modify_identity_tag_end;
    }

    // adding requires a real tag; the admin tag (0) is real and may be granted/revoked this way
    if (add == TRUE && server_admin__is_tag_id_real(tag_id) == FALSE)
    {
        goto label_client_msg__process_modify_identity_tag_end;
    }

    // update the stored identity
    identity_store__modify_identity_tag_in_store(target_hash, tag_id, add);

    // if the identity is connected right now, mirror the change on the live client + broadcast it
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

        tag_index = server_admin__get_index_of_tag_id_of_client(holder->client_id, tag_id);

        if (add == TRUE)
        {
            if (tag_index == -1)
            {
                cvector_push_back(holder->tag_ids, (int)tag_id);
                if (tag_id == ADMIN_TAG_ID)
                {
                    holder->is_admin = TRUE;
                }
                server_msg__administration__send_add_tag_to_client_event_to_all_clients(holder, tag_id);

                // an admin's flag may be withheld, so the rows showing the client learn the code they show from now on
                if (tag_id == ADMIN_TAG_ID && g_server_settings.is_admin_country_flag_hidden == TRUE)
                {
                    server_msg__profile__send_client_country_code_changed_to_all_clients(holder);
                }
            }
        }
        else
        {
            if (tag_index != -1)
            {
                cvector_erase(holder->tag_ids, tag_index);
                if (tag_id == ADMIN_TAG_ID)
                {
                    holder->is_admin = FALSE;
                }
                server_msg__administration__send_remove_tag_from_client_event_to_all_clients(holder, tag_id);

                // the client's flag was withheld as an admin's; the rows showing the client get the real code back
                if (tag_id == ADMIN_TAG_ID && g_server_settings.is_admin_country_flag_hidden == TRUE)
                {
                    server_msg__profile__send_client_country_code_changed_to_all_clients(holder);
                }
            }
        }

        break;
    }

    // refresh the requesting admin's identity list
    server_msg__administration__send_identity_list_to_single_client(sender_client);

label_client_msg__process_modify_identity_tag_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to delete an icon from the server's icon pool. tags that still reference the
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

    if (cJSON_IsNumber(json_icon_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__administration__process_delete_icon: icon_id missing or not a number");
        return;
    }

    icon_id = (uint64)json_icon_id->valueint;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_icons_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
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
 * @brief processes an admin request to set or clear a tag's linked icon. an icon_id in the message assigns that
 *        icon (it must exist); an absent icon_id clears the tag's icon. the change is broadcast to all clients.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__administration__process_set_tag_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_icon_id = 0;
    uint64 tag_id = 0;
    uint64 icon_id = 0;
    boole wants_icon = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        return;
    }

    tag_id = (uint64)json_tag_id->valueint;
    wants_icon = cJSON_IsNumber(json_icon_id);
    if (wants_icon == TRUE)
    {
        icon_id = (uint64)json_icon_id->valueint;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_tags_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_set_server_settings_set_tag_icon_end;
    }

    if (server_admin__is_tag_id_real(tag_id) == FALSE)
    {
        goto label_client_msg__process_set_server_settings_set_tag_icon_end;
    }

    if (wants_icon == TRUE)
    {
        // the assigned icon must exist (an icon id equals its index)
        if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
        {
            goto label_client_msg__process_set_server_settings_set_tag_icon_end;
        }

        g_tags_array[tag_id].has_icon = TRUE;
        g_tags_array[tag_id].icon_id = icon_id;
    }
    else
    {
        // no icon_id in the request -> clear the tag's icon
        g_tags_array[tag_id].has_icon = FALSE;
        g_tags_array[tag_id].icon_id = 0;
    }

    server_msg__administration__send_tag_icon_changed_event_to_all_clients(tag_id, g_tags_array[tag_id].has_icon, g_tags_array[tag_id].icon_id);

label_client_msg__process_set_server_settings_set_tag_icon_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to read the admin log: replies with every stored line, oldest
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

    is_sender_admin = base__is_client_valid_admin(sender_client_id);
    if (is_sender_admin == TRUE)
    {
        server_msg__administration__send_admin_log_to_single_client(&g_clients_array[sender_client_id]);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__administration__process_admin_log_request sender with sender_client_id", sender_client_id, "is not an admin");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to clear the admin log. the clearing itself becomes the first line
 *        of the fresh log, so with several admins it stays visible who emptied it. the refreshed
 *        log is sent back right away. non-admin requests are silently ignored
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
    if (cJSON_IsNumber(json_client_id) == FALSE)
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
 * @brief processes an admin request to kick a client from the server
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

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        goto label_client_msg__process_kick_request_end;
    }

    receiver = &g_clients_array[json_client_id->valueint];
    if (receiver->is_authenticated == FALSE || receiver->is_existing == FALSE)
    {
        goto label_client_msg__process_kick_request_end;
    }

    server_logs__client_kicked(receiver->username, admin->username);

    ws_close_client(receiver->p_ws_connection);

label_client_msg__process_kick_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to ban a client from the server
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

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        goto label_client_msg__process_ban_request_end;
    }

    receiver = &g_clients_array[json_client_id->valueint];
    if (receiver->is_authenticated == FALSE || receiver->is_existing == FALSE)
    {
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
    // after channels, icons and tags.
    if (should_ban == TRUE)
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        clib__read_lock(&g_icons_global_rwlock_guard);
        clib__read_lock(&g_tags_global_rwlock_guard);
        clib__write_lock(&g_bans_global_rwlock_guard);

        server_admin__add_ban(banned_ip, banned_country, banned_identity, "");
        settings__save_server_settings_to_file();

        clib__unlock(&g_bans_global_rwlock_guard);
        clib__unlock(&g_tags_global_rwlock_guard);
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
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_tags_global_rwlock_guard);
    clib__write_lock(&g_bans_global_rwlock_guard);

    removed = server_admin__remove_ban_by_ip(json_ip->valuestring);
    if (removed == TRUE)
    {
        settings__save_server_settings_to_file();
    }

    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to read another client's info (connected time, ip, country, identity).
 *        only an admin gets a reply; a non-admin's request is silently ignored ("nothing happens for now")
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

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
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
