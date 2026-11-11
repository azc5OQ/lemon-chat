// This file checks requests to change usernames and avatars or to view stored identities.
#include "client_msg.h"
#include "../../core/permissions.h"
#include "../../core/client_session.h"
#include "../../core/identity_store.h"
#include "../server/server_msg.h"
#include "../../core/server_logs.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"

// declarations
static boole _client_msg_profile_internal__is_change_client_username_message_valid(cJSON *json_root, uint64 client_id);

/**
 * @brief This function checks if clients username change request json message has all nessecary
 *        fields
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within msg/client/client_msg_profile.c
 *
 * @return boole
 */
static boole _client_msg_profile_internal__is_change_client_username_message_valid(cJSON *json_root, uint64 client_id)
{
    cJSON *json_new_username = NULL_POINTER;
    cJSON *json_client_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    uint64 validated_client_id = 0;

    // check the message object
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        // no object is not present under key "message", not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    // read name and id
    json_new_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    // refuse missing client id
    if (json_client_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_client_id == NULL_POINTER \n");
        return FALSE;
    }

    // check the client index
    if (g_server_settings.max_client_count == 0 || util__json_read_uint64(json_client_id, g_server_settings.max_client_count - 1, &validated_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_profile_internal__is_change_client_username_message_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    // check client id range
    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_profile_internal__is_change_client_username_message_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    // refuse a missing name
    if (json_new_username == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " new_username != string \n");
        return FALSE;
    }

    // check name is text
    if (cJSON_IsString(json_new_username) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " new_username != string \n");
        return FALSE;
    }

    // refuse missing text
    if (json_new_username->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " new_username->valuestring == NULL \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function processes connection check message from client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @attention this function uses write lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__profile__process_change_client_username(cJSON *json_root, uint64 sender_client_id)
{
    boole does_client_have_permission_to_change_username = FALSE;
    cJSON *json_message_value = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_client_id = NULL_POINTER;
    boole is_username_taken = FALSE;
    boole is_change_success = FALSE;
    client_t *client_to_alter = NULL_POINTER;
    int64 new_username_length = 0;
    uint64 i = 0;

    // check the cooldown time
    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_change_client_username base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    // check name and id
    if (_client_msg_profile_internal__is_change_client_username_message_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_change_client_username client_msg__is_change_client_username_message_valid == FALSE \n");
        return;
    }

    // read name and id
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    // check the sender connection
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto label_client_msg__process_change_client_username_end;
    }

    // check if client has permission to change the username
    client_to_alter = &g_clients_array[json_client_id->valueint];

    if (json_client_id->valueint == sender_client_id)
    {
        does_client_have_permission_to_change_username = TRUE;
    }
    else
    {
        // client that is requesting change for username does not own it, maybe it's admin messing around with music bot?
        if (client_to_alter->is_music_bot == TRUE && g_clients_array[sender_client_id].is_admin == TRUE)
        {
            does_client_have_permission_to_change_username = TRUE;
        }
    }

    // refuse without permission
    if (does_client_have_permission_to_change_username == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        goto label_client_msg__process_change_client_username_end;
    }

    // the server can disable renaming for ordinary users. admins can still rename clients
    if (g_server_settings.is_client_rename_allowed == FALSE && g_clients_array[sender_client_id].is_admin == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_id", sender_client_id, "rename ignored - renames are off for users \n");
        goto label_client_msg__process_change_client_username_end;
    }

    // check name is free
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        // is_existing needs to be guarded with mutex, while opening client, rwlock is not usable
        if (g_clients_array[i].is_existing == FALSE)
        { // client not is_existing, skip, this needs global lock
            continue;
        }

        if (g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        is_username_taken = clib__is_string_equal(g_clients_array[i].username, json_message_value->valuestring);

        if (is_username_taken == TRUE)
        {
            // username used by some of the clients, start another loop, with incremented numeric part of client's username
            DBG_CLIENT_MESSAGE log_info("%s %d %s", "username ", is_username_taken, " already taken \n");
            break;
        }
    }

    // store the new name
    if (is_username_taken == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " not taken \n");

        if (client_to_alter->is_authenticated == TRUE)
        {
            // check the name length
            new_username_length = clib__utf8_string_length_check_max_length(json_message_value->valuestring, 50);
            if (new_username_length == -1)
            {
                DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " max length exceeded 50 characters \n");
            }
            else if (new_username_length > 0)
            {
                // log the name change
                server_logs__username_changed(client_to_alter->username, json_message_value->valuestring);

                // clear the old name
                clib__null_memory(client_to_alter->username, USERNAME_MAX_LENGTH);

                // the client chose this name itself, so it is the name to save with the identity
                clib__null_memory(client_to_alter->preferred_username, USERNAME_MAX_LENGTH);
                clib__string_copy_n(client_to_alter->username, sizeof(client_to_alter->username), json_message_value->valuestring, new_username_length);
                is_change_success = TRUE;
            }
        }
    }

label_client_msg__process_change_client_username_end:

    // save and send name
    if (is_change_success == TRUE)
    {
        // keep the renamed target in this slot until its saved identity and event
        // are published; both callees already require the clients lock
        identity_store__sync_client_identity_in_store(client_to_alter);
        server_msg__profile__send_client_rename_message_to_all_clients(client_to_alter, client_to_alter->username);
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Sends stored names and avatars when the server settings and the client's groups allow it.
 * @param json_root -> request with no additional fields
 * @param sender_client_id -> client requesting the list
 */
void client_msg__profile__process_request_stored_clients(cJSON *json_root, uint64 sender_client_id)
{
    client_t *client = NULL_POINTER;

    (void)json_root; // the request has no fields to read

    // refuse a disabled list
    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_stored_client_list_allowed == FALSE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (base__is_current_client_request(sender_client_id) == TRUE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_request_stored_clients stored clients list is not enabled \n");
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    // check the sender state
    if (base__is_current_client_request(sender_client_id) == FALSE ||
        client->is_authenticated == FALSE ||
        client->is_existing == FALSE ||
        client->is_music_bot == TRUE)
    {
        goto label_client_msg__process_request_stored_clients_end;
    }

    // groups can change who may view the stored names and avatars
    if (permissions__is_allowed(client, PERMISSION_VIEW_STORED_USERS) == FALSE)
    {
        server_msg__administration__send_permission_denied_to_single_client(client, permissions__name(PERMISSION_VIEW_STORED_USERS));
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__profile__process_request_stored_clients client_id", sender_client_id, "cannot view stored users \n");
        goto label_client_msg__process_request_stored_clients_end;
    }

    // send the stored clients
    server_msg__profile__send_stored_clients_to_single_client(client);

label_client_msg__process_request_stored_clients_end:
    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client sets its OWN avatar, a base64 image data-url, no admin rights needed
 *
 *        gated by is_avatar_upload_allowed. empty and oversize uploads (per the server's configured max, and
 *        never past MAX_CLIENT_AVATAR_LENGTH) are SILENTLY dropped, no error goes back. the image
 *        is kept on the live client_t so it can be served without a store lookup, and mirrored into
 *        the identity store keyed by the client's public-key hash so it persists with their
 *        identity. a lightweight avatar_changed event is then broadcast so others can re-request it.
 *
 * @param cJSON* json_root -> the parsed client request, carries message.base64_avatar
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock while the live avatar and the identity store are updated
 *
 * @return void
 */
void client_msg__profile__process_avatar_upload(cJSON *json_root, uint64 sender_client_id)
{
    client_t *client = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_base64_avatar = NULL_POINTER;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)] = { 0 };
    uint64 base64_length = 0;
    uint64 max_base64_length = 0;

    // check avatars are enabled
    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (base__is_current_client_request(sender_client_id) == TRUE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check the message object
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    // check avatar is text
    json_base64_avatar = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_avatar");
    if (cJSON_IsString(json_base64_avatar) == FALSE || json_base64_avatar->valuestring == NULL_POINTER)
    {
        return;
    }

    // read the avatar length
    base64_length = clib__utf8_string_length(json_base64_avatar->valuestring);

    // raw image cap -> base64 is ~4/3 of raw (plus a small data-url prefix). oversize/empty uploads are
    // silently dropped (no error is sent back). also never exceed the store buffer
    max_base64_length = (uint64)((g_server_settings.avatar_max_size_bytes * 4) / 3) + 64;
    if (base64_length == 0 || base64_length >= MAX_CLIENT_AVATAR_LENGTH || base64_length > max_base64_length)
    {
        DBG_IDENTITIES log_info("%s %llu %s", "avatar_upload: dropped empty/oversize avatar from client_id", sender_client_id, "\n");
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    // check the sender state
    if (base__is_current_client_request(sender_client_id) == FALSE ||
        client->is_authenticated == FALSE ||
        client->is_existing == FALSE ||
        client->is_music_bot == TRUE ||
        client->public_key[0] == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // check the RAM limit
    if (base__is_server_ram_budget_reached() != FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(client, "The server has reached its RAM budget. Try uploading again later.");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // store the live avatar on the client (heap, freed on disconnect/replace) so it can be served to
    // others without a store lookup; also mirror it into the identity store for cross-session persistence
    if (client->base64_avatar != NULL_POINTER)
    {
        memorymanager__free((nuint)client->base64_avatar);
        client->base64_avatar = NULL_POINTER;
    }

    // copy the new avatar
    client->base64_avatar = (char *)memorymanager__allocate(base64_length + 1, MEMALLOC_AVATAR);
    if (client->base64_avatar != NULL_POINTER)
    {
        clib__copy_memory(json_base64_avatar->valuestring, client->base64_avatar, base64_length, base64_length);
    }

    // save avatar with identity
    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    identity_store__set_identity_avatar_by_hash(identity_hash, json_base64_avatar->valuestring);

    // send the change event
    server_msg__profile__send_avatar_changed_event_to_all_clients(client);

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client deletes its OWN avatar
 *
 *        frees the live avatar on the client_t, clears the entry in the identity store (an empty
 *        string is what clears it) and broadcasts avatar_changed so everyone drops their copy.
 *        gated by is_avatar_upload_allowed. the request carries no fields.
 *
 * @param cJSON* json_root -> the parsed client request (unused, the request carries no fields)
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock while the live avatar and the identity store are updated
 *
 * @return void
 */
void client_msg__profile__process_delete_avatar(cJSON *json_root, uint64 sender_client_id)
{
    client_t *client = NULL_POINTER;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)] = { 0 };

    (void)json_root;

    // check avatars are enabled
    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (base__is_current_client_request(sender_client_id) == TRUE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    // check the sender state
    if (base__is_current_client_request(sender_client_id) == FALSE ||
        client->is_authenticated == FALSE ||
        client->is_existing == FALSE ||
        client->public_key[0] == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // free the avatar
    if (client->base64_avatar != NULL_POINTER)
    {
        memorymanager__free((nuint)client->base64_avatar);
        client->base64_avatar = NULL_POINTER;
    }

    // clear the stored avatar
    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    identity_store__set_identity_avatar_by_hash(identity_hash, ""); // empty string clears it

    // send the change event
    server_msg__profile__send_avatar_changed_event_to_all_clients(client);

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client requests ONE other client's avatar, as the profile pane needs
 *
 *        responds with a single client_avatar message, served straight from the target's live
 *        client_t->base64_avatar, or an empty string when the target has none. gated by
 *        is_avatar_upload_allowed, and the requested client id must be in range.
 *
 * @param cJSON* json_root -> the parsed client request, carries message.client_id
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock while the target is read and the response is sent
 *
 * @return void
 */
void client_msg__profile__process_request_avatar_for_client(cJSON *json_root, uint64 sender_client_id)
{
    client_t *sender = NULL_POINTER;
    client_t *target = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_client_id = NULL_POINTER;
    uint64 target_client_id = 0;

    // check avatars are enabled
    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        return;
    }

    // check the message object
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    // check the target id
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (g_server_settings.max_client_count == 0 || util__json_read_uint64(json_client_id, g_server_settings.max_client_count - 1, &target_client_id) == FALSE)
    {
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];
    target = &g_clients_array[target_client_id];

    // check sender and target
    if (base__is_current_client_request(sender_client_id) == FALSE ||
        sender->is_authenticated == FALSE ||
        sender->is_existing == FALSE ||
        target->is_existing == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // send the avatar
    server_msg__profile__send_client_avatar_to_single_client(sender, target_client_id, (target->base64_avatar != NULL_POINTER) ? target->base64_avatar : "");

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client requests a CHUNK of avatars, a client_ids array, for lazy-loading
 *
 *        the server sends one client_avatar message per target that actually has a live avatar.
 *        targets with none are skipped so the client keeps its placeholder. no more than 100
 *        avatars go out per request, so a single call cannot blast unbounded work. gated by
 *        is_avatar_upload_allowed.
 *
 * @param cJSON* json_root -> the parsed client request, carries the message.client_ids array
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock for the whole batch loop
 *
 * @return void
 */
void client_msg__profile__process_request_avatars_batch(cJSON *json_root, uint64 sender_client_id)
{
    client_t *sender = NULL_POINTER;
    client_t *target = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_client_ids = NULL_POINTER;
    cJSON *json_id = NULL_POINTER;
    uint64 target_client_id = 0;
    uint64 sent = 0;

    // check avatars are enabled
    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        return;
    }

    // check the message object
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    // check the id array
    json_client_ids = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_ids");
    if (cJSON_IsArray(json_client_ids) == FALSE)
    {
        return;
    }

    // lock the clients
    clib__write_lock(&g_clients_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];

    // check the sender state
    if (base__is_current_client_request(sender_client_id) == FALSE || sender->is_authenticated == FALSE || sender->is_existing == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // read each client id
    cJSON_ArrayForEach(json_id, json_client_ids)
    {
        // stop after 100 avatars
        if (sent >= 100)
        {
            break; // cap one batch so a single request can't blast unbounded work
        }

        // skip an invalid id
        if (g_server_settings.max_client_count == 0 || util__json_read_uint64(json_id, g_server_settings.max_client_count - 1, &target_client_id) == FALSE)
        {
            continue;
        }

        // skip clients without avatar
        target = &g_clients_array[target_client_id];
        if (target->is_existing == FALSE || target->base64_avatar == NULL_POINTER)
        {
            continue; // skip clients without a live avatar; the client keeps its placeholder
        }

        // send the avatar
        server_msg__profile__send_client_avatar_to_single_client(sender, target_client_id, target->base64_avatar);
        sent++;
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);
}
