// This file sends admin replies, client details, server logs, icons and tags.

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
 * @brief Sends the available tags to a client after login.
 *
 * @param client_t* receiver -> the client that receives it
 *
 * @attention it doesnt need readlock, already called within writelock in msg/client/
 *
 * @return void
 */
void server_msg__administration__send_tag_list_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_tags_array = 0;
    uint64 x = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("tag_list", &message);
    json_tags_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_TAGS; x++)
    {
        cJSON* single_tag_id_object = NULL_POINTER;

        if (g_tags_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_tag_id_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_tag_id_object, "tag_id", (double)g_tags_array[x].id);
        cJSON_AddStringToObject(single_tag_id_object, "tag_name", g_tags_array[x].name);
        cJSON_AddNumberToObject(single_tag_id_object, "tag_linked_icon_id", (double)g_tags_array[x].icon_id);
        cJSON_AddItemToObject(single_tag_id_object, "has_icon", cJSON_CreateBool(g_tags_array[x].has_icon == TRUE));
        cJSON_AddItemToArray(json_tags_array, single_tag_id_object);
    }

    cJSON_AddItemToObject(message, "tags", json_tags_array);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends the admin identity-management list: every stored identity as { public_key_hash,
 *        tag_ids[], is_online }. is_online is TRUE when a currently-connected authenticated client
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
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 c = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("identity_list", &message);
    json_identities_array = cJSON_CreateArray();

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        cJSON* single_identity_object = NULL_POINTER;
        cJSON* identity_tag_ids_array = NULL_POINTER;
        boole is_online = FALSE;

        // an identity is worth listing when it carries ANYTHING: tags, an alias or an avatar. it used
        // to require tags, which hid every registered user from the admin - registration grants an
        // alias, not a tag, so the very people this list is for were the ones missing from it
        if (g_client_stored_data[i].public_key[0] == 0 || (g_client_stored_data[i].tag_id_count == 0 && g_client_stored_data[i].alias[0] == 0 && g_client_stored_data[i].base64_avatar[0] == 0))
        {
            continue;
        }

        single_identity_object = cJSON_CreateObject();
        cJSON_AddStringToObject(single_identity_object, "public_key_hash", &g_client_stored_data[i].public_key[0]);
        cJSON_AddStringToObject(single_identity_object, "username", &g_client_stored_data[i].username[0]);
        // the admin-granted display name; empty means this identity is not registered
        cJSON_AddStringToObject(single_identity_object, "alias", &g_client_stored_data[i].alias[0]);

        identity_tag_ids_array = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(identity_tag_ids_array, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(single_identity_object, "tag_ids", identity_tag_ids_array);

        // mark online if any connected authenticated non-bot receiver hashes to this identity
        for (c = 0; c < g_server_settings.max_client_count; c++)
        {
            client_t* connected_client = &g_clients_array[c];

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
 * @param client_t* receiver -> self explanatory
 * @param char* reason -> optional text the client shows instead of its generic denial line; NULL omits it
 *
 * @attention this function is used within acquired readlock within msg/client/ , multiple functions
 *
 * @return void
 */
void server_msg__administration__send_access_denied_to_single_client(client_t* receiver, char* reason)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("access_denied", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    if (reason != NULL_POINTER && reason[0] != 0)
    {
        cJSON_AddStringToObject(message, "reason", reason);
    }

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief replies to a single client with every admin log line, oldest first (used when an admin
 *        opens or refreshes the log tab). the caller must have checked the client is an admin
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
 * @brief broadcasts a tag-added-to-client event to all clients
 *
 * @param client_t* subject -> the client that received the tag
 * @param uint64 tag_id -> id of the tag added
 *
 * @return void
 */
void server_msg__administration__send_add_tag_to_client_event_to_all_clients(client_t* subject, uint64 tag_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("tag_add_to_client", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a tag-removed-from-client event to all clients
 *
 * @param client_t* subject -> the client that lost the tag
 * @param uint64 tag_id -> id of the tag removed
 *
 * @return void
 */
void server_msg__administration__send_remove_tag_from_client_event_to_all_clients(client_t* subject, uint64 tag_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("remove_tag_from_client", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
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
 * @brief broadcasts a tag-deleted-from-pool event to all clients
 *
 * @param uint64 tag_id -> id of the tag that was deleted
 *
 * @return void
 */
void server_msg__administration__send_remove_tag_event_to_all_clients(uint64 tag_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("tag_delete", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);

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
 * @brief broadcasts a tag-icon-changed event to all clients
 *
 * @param uint64 tag_id -> id of the tag whose icon changed
 * @param boole has_icon -> whether the tag now has an icon
 * @param uint64 icon_id -> the tag's icon id (meaningful only when has_icon is TRUE)
 *
 * @return void
 */
void server_msg__administration__send_tag_icon_changed_event_to_all_clients(uint64 tag_id, boole has_icon, uint64 icon_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("tag_icon_changed", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddItemToObject(message, "has_icon", cJSON_CreateBool(has_icon == TRUE));
    cJSON_AddNumberToObject(message, "tag_linked_icon_id", icon_id);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief broadcasts a new-tag-created event to all clients
 *
 * @param uint64 tag_id -> id of the new tag
 * @param char* tag_name -> name of the new tag
 * @param uint64 tag_linked_icon_id -> id of the icon linked to the tag
 * @param boole has_icon -> TRUE when the tag really has an icon, sent as a bool so the client knows
 *                          whether tag_linked_icon_id is meaningful
 *
 * @return void
 */
void server_msg__administration__send_create_new_tag_event_to_all_clients(uint64 tag_id, char* tag_name, uint64 tag_linked_icon_id, boole has_icon)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("tag_add", &message);

    cJSON_AddNumberToObject(message, "tag_id", tag_id);
    cJSON_AddStringToObject(message, "tag_name", tag_name);
    cJSON_AddNumberToObject(message, "tag_linked_icon_id", tag_linked_icon_id);
    cJSON_AddItemToObject(message, "has_icon", cJSON_CreateBool(has_icon == TRUE));

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);

    cJSON_Delete(root);
}
