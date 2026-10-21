// This file checks requests to change usernames, avatars and aliases or to view stored identities.
#include "client_msg.h"
#include "../../core/client_session.h"
#include "../../core/identity_store.h"
#include "../server/server_msg.h"
#include "../../core/server_logs.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"

// declarations
static boole _client_msg_profile_internal__is_change_client_username_message_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_profile_internal__is_set_identity_alias_request_valid(cJSON* json_root);
static boole _client_msg_profile_internal__is_set_alias_request_valid(cJSON* json_root);

/**
 * @brief This function checks if clients username change request json message has all nessecary fields
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within msg/client/client_msg_profile.c
 *
 * @return boole
 */
static boole _client_msg_profile_internal__is_change_client_username_message_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_new_username = 0;
    cJSON* json_client_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        // no object is not present under key "message", not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    json_new_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    if (json_client_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_client_id == NULL_POINTER \n");
        return FALSE;
    }
    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_profile_internal__is_change_client_username_message_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_profile_internal__is_change_client_username_message_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    if (json_new_username == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " new_username != string \n");
        return FALSE;
    }

    if (cJSON_IsString(json_new_username) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " new_username != string \n");
        return FALSE;
    }

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
void client_msg__profile__process_change_client_username(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_client_have_permission_to_change_username = FALSE;
    cJSON* json_message_value = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    boole is_username_taken = FALSE;
    boole is_change_success = FALSE;

    client_t* client_to_alter = 0;

    int64 new_username_length = 0;
    uint64 i = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_change_client_username base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_profile_internal__is_change_client_username_message_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_change_client_username client_msg__is_change_client_username_message_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client has permission to change the username
    client_to_alter = &g_clients_array[json_client_id->valueint];

    // a client that is requesting a change of its own username: allow it - UNLESS the client is registered.
    // a registered user's name is admin-controlled (set through the register-username action), so
    // the client cannot rename itself out of it; an admin still can, via that action
    if (json_client_id->valueint == sender_client_id)
    {
        if (client_to_alter->is_registered == TRUE)
        {
            DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_id", sender_client_id, "is registered - self username change denied \n");
            does_client_have_permission_to_change_username = FALSE;
        }
        else
        {
            does_client_have_permission_to_change_username = TRUE;
        }
    }
    else
    {
        // client that is requesting change for username does not own it, maybe it's admin messing around with music bot?
        if (client_to_alter->is_music_bot == TRUE && g_clients_array[sender_client_id].is_admin == TRUE)
        {
            does_client_have_permission_to_change_username = TRUE;
        }
    }

    if (does_client_have_permission_to_change_username == FALSE)
    {
        goto label_client_msg__process_change_client_username_end;
    }

    // renames can be switched off server-wide; the request is then silently ignored, because the
    // switch is meant against name games by users - an admin still renames freely
    if (g_server_settings.is_client_rename_allowed == FALSE && g_clients_array[sender_client_id].is_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_id", sender_client_id, "rename ignored - renames are off for users \n");
        goto label_client_msg__process_change_client_username_end;
    }

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

    // registered names are RESERVED: without this a guest could take the name of a registered user
    // while that user is offline, and the moment the user came back there would be two of them. refusing
    // the registration of a name in use (see the register handler) only holds if this holds too
    if (is_username_taken == FALSE && client_to_alter->public_key[0] != 0)
    {
        char requester_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
        util__hash_password_to_base64(client_to_alter->public_key, requester_identity_hash, sizeof(requester_identity_hash));

        if (identity_store__is_alias_taken_by_another_identity(json_message_value->valuestring, requester_identity_hash) == TRUE)
        {
            DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " is a registered name of another identity \n");
            is_username_taken = TRUE;
        }
    }

    if (is_username_taken == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " not taken \n");

        if (client_to_alter->is_authenticated == TRUE)
        {
            new_username_length = clib__utf8_string_length_check_max_length(json_message_value->valuestring, 50);
            if (new_username_length == -1)
            {
                DBG_CLIENT_MESSAGE log_info("%s %s %s", "username ", json_message_value->valuestring, " max length exceeded 50 characters \n");
            }
            else if (new_username_length > 0)
            {
                server_logs__username_changed(client_to_alter->username, json_message_value->valuestring);

                clib__null_memory(client_to_alter->username, USERNAME_MAX_LENGTH);
                clib__copy_memory(json_message_value->valuestring, client_to_alter->username, new_username_length, USERNAME_MAX_LENGTH);
                is_change_success = TRUE;
            }
        }
    }

label_client_msg__process_change_client_username_end:

    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_change_success == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__profile__send_client_rename_message_to_all_clients \n");

        clib__read_lock(&g_clients_global_rwlock_guard);
        server_msg__profile__send_client_rename_message_to_all_clients(client_to_alter, client_to_alter->username);
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief validates an admin's set-identity-alias request: a stored identity hash and an alias string
 *
 *        the public_key_hash must be a non-empty string shorter than MAX_PUBLIC_KEY_LENGTH. the
 *        alias must be a string shorter than USERNAME_MAX_LENGTH, and an empty one is allowed -
 *        that is how an alias is cleared, unregistering the identity.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_profile_internal__is_set_identity_alias_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_public_key_hash = 0;
    cJSON* json_alias = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return FALSE;
    }

    json_public_key_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");
    if (cJSON_IsString(json_public_key_hash) == FALSE || json_public_key_hash->valuestring == NULL_POINTER)
    {
        return FALSE;
    }

    if (clib__utf8_string_length(json_public_key_hash->valuestring) == 0 || clib__utf8_string_length(json_public_key_hash->valuestring) >= MAX_PUBLIC_KEY_LENGTH)
    {
        return FALSE;
    }

    json_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "alias");
    if (cJSON_IsString(json_alias) == FALSE || json_alias->valuestring == NULL_POINTER)
    {
        return FALSE;
    }

    if (clib__utf8_string_length(json_alias->valuestring) >= USERNAME_MAX_LENGTH)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief admin registers (or clears) an alias on a STORED identity, addressed by its hash rather
 *        than by a connected client id
 *
 *        that is the difference from client_msg__profile__process_set_alias_request: this one works while
 *        the owner is offline, which is what the identity list in server settings needs. the
 *        request is refused unless identities and alias registrations are enabled, the sender is a
 *        valid admin, and the alias is not already taken by another identity. if the owner happens
 *        to be connected, their live client row is updated, the change is broadcast to everyone,
 *        and a newly registered owner is handed the stored-clients roster without reconnecting.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients read lock for the admin check, then the clients write lock while the
 *       live client row is updated
 *
 * @return void
 */
void client_msg__profile__process_set_identity_alias_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_sender_admin = FALSE;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_public_key_hash = 0;
    cJSON* json_alias = 0;
    char connected_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_alias_registration_allowed == FALSE)
    {
        return;
    }

    status = _client_msg_profile_internal__is_set_identity_alias_request_valid(json_root);
    if (status == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_public_key_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");
    json_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "alias");

    clib__read_lock(&g_clients_global_rwlock_guard);
    is_sender_admin = base__is_client_valid_admin(sender_client_id);
    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_sender_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_set_identity_alias_request sender is not an admin \n");
        return;
    }

    // aliases are the handle offline entries are paired by, so two identities must never share one
    if (json_alias->valuestring[0] != 0 && identity_store__is_alias_taken_by_another_identity(json_alias->valuestring, json_public_key_hash->valuestring) == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_set_identity_alias_request alias is already taken \n");
        return;
    }

    identity_store__set_identity_alias_by_hash(json_public_key_hash->valuestring, json_alias->valuestring);

    // the owner may be connected right now: keep their live row and everyone else's view in step
    clib__write_lock(&g_clients_global_rwlock_guard);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE || client->public_key[0] == 0)
        {
            continue;
        }

        util__hash_password_to_base64(client->public_key, connected_identity_hash, sizeof(connected_identity_hash));

        if (clib__is_string_equal(connected_identity_hash, json_public_key_hash->valuestring) == FALSE)
        {
            continue;
        }

        clib__null_memory(&client->alias[0], USERNAME_MAX_LENGTH);
        clib__copy_memory(json_alias->valuestring, &client->alias[0], clib__utf8_string_length(json_alias->valuestring), USERNAME_MAX_LENGTH - 1);
        client->is_registered = (boole)(client->alias[0] != 0);

        server_msg__profile__send_client_alias_changed_to_all_clients(client, &client->alias[0]);

        // just became registered: hand over the offline roster without making them reconnect
        if (client->is_registered == TRUE && g_server_settings.is_stored_client_list_allowed == TRUE)
        {
            server_msg__profile__send_stored_clients_to_single_client(client);
        }
        break;
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a set-alias request: client id present and within range, alias present as a string
 *        and short enough. an empty alias is valid - it clears the registration.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole TRUE when the request is well-formed
 */
static boole _client_msg_profile_internal__is_set_alias_request_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_alias = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_profile_internal__is_set_alias_request_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_profile_internal__is_set_alias_request_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "alias");
    if (cJSON_IsString(json_alias) == FALSE || json_alias->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_profile_internal__is_set_alias_request_valid cJSON_IsString(alias) \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_alias->valuestring) >= USERNAME_MAX_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_profile_internal__is_set_alias_request_valid alias too long \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief admin registers (or clears, with an empty string) an alias - a display name - on another
 *        connected client's identity
 *
 *        the alias is set on the live client and in the identity store, so it survives reconnects
 *        within the run; disk persistence follows on the next settings save. the change is then
 *        broadcast to everyone. requires identities and alias registrations to be enabled.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__profile__process_set_alias_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    client_t* client_to_alias = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    cJSON* json_alias = 0;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_alias_registration_allowed == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_set_alias_request alias registrations are not enabled \n");
        return;
    }

    status = _client_msg_profile_internal__is_set_alias_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_set_alias_request _client_msg_profile_internal__is_set_alias_request_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_set_alias_request_end;
    }

    if (client->is_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to set alias \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "alias");

    client_to_alias = &g_clients_array[json_client_id->valueint];

    if (client_to_alias->is_authenticated == FALSE || client_to_alias->is_existing == FALSE || client_to_alias->is_music_bot == TRUE)
    {
        goto label_client_msg__process_set_alias_request_end;
    }

    if (client_to_alias->public_key[0] == 0)
    {
        goto label_client_msg__process_set_alias_request_end; // no identity to attach the alias to
    }

    util__hash_password_to_base64(client_to_alias->public_key, identity_hash, sizeof(identity_hash));

    // aliases are the handle the stored-clients list is keyed by and what clients pair offline
    // entries against, so two identities must never share one (compared ignoring ascii case)
    if (identity_store__is_alias_taken_by_another_identity(json_alias->valuestring, identity_hash) == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__profile__process_set_alias_request alias [", json_alias->valuestring, "] is already taken by another identity \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    // a registered name BECOMES the username, so it must be free among the people connected right
    // now too - the check above only sees stored identities and would happily hand out a name a
    // guest is wearing, leaving two people on screen under one name. refuse instead
    if (client_session__is_username_taken_by_another_client(json_alias->valuestring, client_to_alias->client_id) == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__profile__process_set_alias_request username [", json_alias->valuestring, "] is in use by a connected client \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    // the registered name is a username as well, so hold it to the same length limit the rename
    // path enforces - it used to accept anything up to the buffer size
    if (clib__utf8_string_length_check_max_length(json_alias->valuestring, 50) == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__profile__process_set_alias_request alias [", json_alias->valuestring, "] length is not acceptable \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    // live client
    clib__null_memory(&client_to_alias->alias[0], USERNAME_MAX_LENGTH);
    clib__copy_memory(json_alias->valuestring, &client_to_alias->alias[0], clib__utf8_string_length(json_alias->valuestring), USERNAME_MAX_LENGTH - 1);

    // identity store, keyed by the identity hash, so the name survives reconnects
    identity_store__set_identity_alias_by_hash(identity_hash, json_alias->valuestring);

    // holding a registered name IS being registered - keep it live so the user does not have to
    // reconnect (and loses the right again the moment the admin clears it)
    client_to_alias->is_registered = (boole)(client_to_alias->alias[0] != 0);

    // the registered name IS the username now: mirror it into the live username and tell everyone,
    // so there is ONE name on screen instead of a username plus a separate alias. on unregister the
    // name stays put (the person keeps it) but self-rename unlocks again
    if (client_to_alias->is_registered == TRUE)
    {
        clib__null_memory(&client_to_alias->username[0], USERNAME_MAX_LENGTH);
        clib__copy_memory(client_to_alias->alias, &client_to_alias->username[0], clib__utf8_string_length(client_to_alias->alias), USERNAME_MAX_LENGTH - 1);
        server_msg__profile__send_client_rename_message_to_all_clients(client_to_alias, client_to_alias->username);
    }

    // this is the moment an identity BECOMES registered, so it is also the moment its raw public key
    // has to be kept - offline messages are encrypted with it while the person is away. it used to be
    // collected only at authentication (skipped here: the client was still unregistered when it connected) and
    // at an admin settings save (only sweeps who is connected at that instant). a user aliased mid
    // session and then leaving was therefore listed as an offline contact WITHOUT a key, and everyone
    // trying to write to the client got "this server does not keep messages for people who are offline"
    // until the client reconnected once. same guards as the authentication path
    if (g_server_settings.is_offline_messaging_allowed == TRUE && client_to_alias->is_registered == TRUE && client_to_alias->public_key[0] != 0 && client_to_alias->is_music_bot == FALSE)
    {
        identity_store__store_identity_raw_public_key(identity_hash, client_to_alias->public_key);
    }

    // alias taken away: the client is not registered anymore, so nobody may write to it while it is away -
    // drop the key that would allow it instead of leaving it lying in the identity store
    if (client_to_alias->is_registered == FALSE)
    {
        identity_store__clear_identity_raw_public_key(identity_hash);
    }

    server_msg__profile__send_client_alias_changed_to_all_clients(client_to_alias, &client_to_alias->alias[0]);

    // just became registered: send the client the roster now instead of making it reconnect for it
    if (client_to_alias->is_registered == TRUE && g_server_settings.is_stored_client_list_allowed == TRUE)
    {
        server_msg__profile__send_stored_clients_to_single_client(client_to_alias);
    }

label_client_msg__process_set_alias_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief sends the requester the identities this server has stored, so it can show people that are
 *        registered here even while they are offline
 *
 *        the response carries only alias, avatar and tags - never public keys, identity hashes or
 *        usernames. it is gated by identities + is_stored_client_list_allowed, and the requester must
 *        itself be REGISTERED (carry an admin-granted alias), so a random guest cannot join and
 *        harvest everyone's name and face. every client sends this request on connect and a denial
 *        is simply silence. the request itself has no payload.
 *
 * @param cJSON* json_root -> the parsed client request (unused, the request carries no fields)
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__profile__process_request_stored_clients(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;

    (void)json_root; // the request has no fields to read

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_stored_client_list_allowed == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__profile__process_request_stored_clients stored clients list is not enabled \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE || client->is_music_bot == TRUE)
    {
        goto label_client_msg__process_request_stored_clients_end;
    }

    // only people the admin registered may see the roster of registered people
    if (client->is_registered == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__profile__process_request_stored_clients client_id", sender_client_id, "is not registered, denying \n");
        goto label_client_msg__process_request_stored_clients_end;
    }

    server_msg__profile__send_stored_clients_to_single_client(client);

label_client_msg__process_request_stored_clients_end:
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
void client_msg__profile__process_avatar_upload(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_base64_avatar = 0;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 base64_length = 0;
    uint64 max_base64_length = 0;

    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_base64_avatar = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_avatar");
    if (cJSON_IsString(json_base64_avatar) == FALSE || json_base64_avatar->valuestring == NULL_POINTER)
    {
        return;
    }

    base64_length = clib__utf8_string_length(json_base64_avatar->valuestring);

    // raw image cap -> base64 is ~4/3 of raw (plus a small data-url prefix). oversize/empty uploads are
    // silently dropped (no error is sent back). also never exceed the store buffer.
    max_base64_length = (uint64)((g_server_settings.avatar_max_size_bytes * 4) / 3) + 64;
    if (base64_length == 0 || base64_length >= MAX_CLIENT_AVATAR_LENGTH || base64_length > max_base64_length)
    {
        DBG_IDENTITIES log_info("%s %llu %s", "avatar_upload: dropped empty/oversize avatar from client_id", sender_client_id, "\n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];
    if (client->is_authenticated == FALSE || client->is_existing == FALSE || client->is_music_bot == TRUE || client->public_key[0] == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    if (base__is_server_ram_budget_reached())
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
    client->base64_avatar = (char*)memorymanager__allocate(base64_length + 1, MEMALLOC_AVATAR);
    if (client->base64_avatar != NULL_POINTER)
    {
        clib__copy_memory(json_base64_avatar->valuestring, client->base64_avatar, base64_length, base64_length);
    }

    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    identity_store__set_identity_avatar_by_hash(identity_hash, json_base64_avatar->valuestring);

    server_msg__profile__send_avatar_changed_event_to_all_clients(client);

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
void client_msg__profile__process_delete_avatar(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];

    (void)json_root;

    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];
    if (client->is_authenticated == FALSE || client->is_existing == FALSE || client->public_key[0] == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    if (client->base64_avatar != NULL_POINTER)
    {
        memorymanager__free((nuint)client->base64_avatar);
        client->base64_avatar = NULL_POINTER;
    }

    util__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    identity_store__set_identity_avatar_by_hash(identity_hash, ""); // empty string clears it

    server_msg__profile__send_avatar_changed_event_to_all_clients(client);

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
void client_msg__profile__process_request_avatar_for_client(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender = 0;
    client_t* target = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    uint64 target_client_id = 0;

    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        return;
    }

    target_client_id = (uint64)json_client_id->valueint;
    if (target_client_id >= g_server_settings.max_client_count)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];
    target = &g_clients_array[target_client_id];

    if (sender->is_authenticated == FALSE || sender->is_existing == FALSE || target->is_existing == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    server_msg__profile__send_client_avatar_to_single_client(sender, target_client_id, (target->base64_avatar != NULL_POINTER) ? target->base64_avatar : "");

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
void client_msg__profile__process_request_avatars_batch(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender = 0;
    client_t* target = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_ids = 0;
    cJSON* json_id = 0;
    uint64 target_client_id = 0;
    uint64 sent = 0;

    if (g_server_settings.is_avatar_upload_allowed == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_client_ids = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_ids");
    if (cJSON_IsArray(json_client_ids) == FALSE)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];
    if (sender->is_authenticated == FALSE || sender->is_existing == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    cJSON_ArrayForEach(json_id, json_client_ids)
    {
        if (sent >= 100)
        {
            break; // cap one batch so a single request can't blast unbounded work
        }
        if (cJSON_IsNumber(json_id) == FALSE)
        {
            continue;
        }

        target_client_id = (uint64)json_id->valueint;
        if (target_client_id >= g_server_settings.max_client_count)
        {
            continue;
        }

        target = &g_clients_array[target_client_id];
        if (target->is_existing == FALSE || target->base64_avatar == NULL_POINTER)
        {
            continue; // skip clients without a live avatar; the client keeps its placeholder
        }

        server_msg__profile__send_client_avatar_to_single_client(sender, target_client_id, target->base64_avatar);
        sent++;
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}
