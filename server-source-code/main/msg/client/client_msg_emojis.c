// This file handles requests to view, upload and delete server emojis.
#include "client_msg.h"
#include "../server/server_msg.h"
#include "../../core/emojis.h"
#include "../../base.h"
#include "../../clib/clib_rwlock.h"
#include <string.h>

static void _client_msg_emojis_internal__process(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket, cstring operation);

/**
 * @brief Checks the connection and permissions before handling an emoji request.
 * @param json_root Parsed request.
 * @param client_id Client slot that received the request.
 * @param socket Connection that sent the request.
 * @param operation Request message type.
 */
static void _client_msg_emojis_internal__process(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket, cstring operation)
{
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* id = cJSON_GetObjectItemCaseSensitive(message, "emoji_id");
    cJSON* name = cJSON_GetObjectItemCaseSensitive(message, "name");
    cJSON* base64 = cJSON_GetObjectItemCaseSensitive(message, "base64");
    client_t* client = NULL_POINTER;
    cstring error = NULL_POINTER;
    cstring requested_id = cJSON_IsString(id) ? id->valuestring : NULL_POINTER;
    char new_id[33] = {0};
    boole editing = (boole)(strcmp(operation, "upload_server_emoji") == 0 || strcmp(operation, "delete_server_emoji") == 0);

    if (client_id >= g_server_settings.max_client_count)
    {
        return;
    }
    if (editing)
    {
        clib__write_lock(&g_clients_global_rwlock_guard);
    }
    else
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
    }
    client = &g_clients_array[client_id];
    if (!client->is_existing || !client->is_authenticated || client->p_ws_connection != socket)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    if (editing && !client->is_admin)
    {
        server_msg__administration__send_access_denied_to_single_client(client, NULL_POINTER);
        server_msg__emojis__send_error(client, operation, "Only a server administrator can change emojis.", requested_id);
    }
    else if (strcmp(operation, "load_server_emojis") == 0)
    {
        server_msg__emojis__send_list(client);
    }
    else if (strcmp(operation, "load_server_emoji") == 0)
    {
        server_msg__emojis__send_image(client, requested_id);
    }
    else if (strcmp(operation, "delete_server_emoji") == 0)
    {
        if (emojis__remove(requested_id))
        {
            server_msg__emojis__send_deleted(requested_id);
        }
        else
        {
            server_msg__emojis__send_error(client, operation, "This emoji is no longer available.", requested_id);
        }
    }
    else
    {
        error = emojis__add(cJSON_IsString(name) ? name->valuestring : NULL_POINTER,
            cJSON_IsString(base64) ? base64->valuestring : NULL_POINTER, new_id);
        if (error != NULL_POINTER)
        {
            server_msg__emojis__send_error(client, operation, error, NULL_POINTER);
        }
        else
        {
            server_msg__emojis__send_added(new_id, name->valuestring);
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Handles a request for available server emoji names.
 * @param json_root Parsed request.
 * @param client_id Requesting client slot.
 * @param socket Connection that sent the request.
 */
void client_msg__emojis__process_list(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket)
{
    _client_msg_emojis_internal__process(json_root, client_id, socket, "load_server_emojis");
}

/**
 * @brief Handles a request for one server emoji image.
 * @param json_root Parsed request.
 * @param client_id Requesting client slot.
 * @param socket Connection that sent the request.
 */
void client_msg__emojis__process_image(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket)
{
    _client_msg_emojis_internal__process(json_root, client_id, socket, "load_server_emoji");
}

/**
 * @brief Handles an administrator's named image upload.
 * @param json_root Parsed request.
 * @param client_id Requesting client slot.
 * @param socket Connection that sent the request.
 */
void client_msg__emojis__process_upload(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket)
{
    _client_msg_emojis_internal__process(json_root, client_id, socket, "upload_server_emoji");
}

/**
 * @brief Handles an administrator's request to delete an emoji.
 * @param json_root Parsed request.
 * @param client_id Requesting client slot.
 * @param socket Connection that sent the request.
 */
void client_msg__emojis__process_delete(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket)
{
    _client_msg_emojis_internal__process(json_root, client_id, socket, "delete_server_emoji");
}
