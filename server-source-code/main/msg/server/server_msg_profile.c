// This file sends usernames, aliases, avatars, visible country codes and stored client information.

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
 * @brief broadcasts "this client's alias changed" (with the alias text; empty = cleared) to all
 *        authenticated non-bot clients. must be called with the clients lock already held
 *        (like the tag-event broadcasts).
 *
 * @param client_t* subject -> the client whose identity just got the alias
 * @param char* alias -> the new alias text, empty or NULL when cleared
 *
 * @return void
 */
void server_msg__profile__send_client_alias_changed_to_all_clients(client_t* subject, char* alias)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("client_alias_changed", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddStringToObject(message, "alias", (alias == NULL_POINTER) ? "" : alias);

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
 * @note must be called with the clients lock already held (like the tag-event broadcasts)
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
 * @note must be called with the clients lock already held (like the tag-event broadcasts)
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
 * @brief sends the stored-identity list to one client, so it can show the people registered on this
 *        server even while they are offline.
 *
 *        every entry carries the alias, the avatar and the tag ids. no identity hash and no username
 *        is sent: the alias is the only handle, and it is what the client pairs against a live client
 *        of the same alias. the raw public key is added on top only while is_offline_messaging_allowed is
 *        enabled, since that is what lets a peer encrypt to somebody who is not connected - it is
 *        public by definition, but it still only goes to registered requesters (the caller checked
 *        that). last_seen (unix seconds) is added only while is_last_seen_enabled is enabled, so nothing
 *        leaks while the setting is off. slots with no public key or no alias are skipped - they are
 *        free, or they have no name to show and nothing to pair on.
 *
 * @param client_t* receiver -> the client that receives the list
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock), exactly like
 *       the identity serializer in base.c.
 *
 * @return void
 */
void server_msg__profile__send_stored_clients_to_single_client(client_t* receiver)
{
    uint64 i = 0;
    uint64 t = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_stored_clients = 0;
    cJSON* json_stored_client = 0;
    cJSON* json_tag_ids = 0;

    if (receiver == NULL_POINTER)
    {
        return;
    }

    root = server_msg__delivery__new_message("stored_clients_list", &message);
    json_stored_clients = cJSON_CreateArray();

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue; // free slot, or nothing to name and pair it by
        }

        json_stored_client = cJSON_CreateObject();
        cJSON_AddStringToObject(json_stored_client, "alias", &g_client_stored_data[i].alias[0]);
        cJSON_AddStringToObject(json_stored_client, "base64_avatar", &g_client_stored_data[i].base64_avatar[0]);

        // with offline messages on, registered peers also get this identity's public key: it is what
        // lets them encrypt a message to somebody who is not connected. a public key is public by
        // definition, but it is still only handed to REGISTERED requesters (the caller checked) and
        // only while the feature is enabled
        if (g_server_settings.is_offline_messaging_allowed == TRUE && g_client_stored_data[i].raw_public_key[0] != 0)
        {
            cJSON_AddStringToObject(json_stored_client, "public_key", &g_client_stored_data[i].raw_public_key[0]);
        }

        // when the admin enabled last-seen, tell the receiver when this identity was last connected
        // (unix seconds). left out entirely while the setting is off, so nothing leaks
        if (g_server_settings.is_last_seen_enabled == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            cJSON_AddNumberToObject(json_stored_client, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds);
        }

        json_tag_ids = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(json_tag_ids, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(json_stored_client, "tag_ids", json_tag_ids);

        cJSON_AddItemToArray(json_stored_clients, json_stored_client);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    cJSON_AddItemToObject(message, "stored_clients", json_stored_clients);

    server_msg__delivery__send_json_to_client(receiver, root);

    cJSON_Delete(root);
}
