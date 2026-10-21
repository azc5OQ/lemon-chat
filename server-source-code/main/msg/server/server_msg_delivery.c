// This file creates message objects and queues encrypted messages for delivery to clients.

#include "server_msg.h"
#include <string.h>
#include "../../base.h"
#include "../../metadata_crypto.h"
#include "../../memory_manager.h"
#include "../../../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"

typedef struct outgoing_metadata
{
    crypto_session_keys_t session;
    size_t text_size;
    char text[];
} outgoing_metadata_t;

// declarations
static char* _server_msg_delivery_internal__encode_queued_text(void* context, size_t* size);
static void _server_msg_delivery_internal__release_queued_text(void* context);
static void _server_msg_delivery_internal__send_text(ws_cli_conn_t* socket, char* dh_shared_secret, char* plaintext);
static void _server_msg_delivery_internal__send_text_to_all_clients(char* plaintext, client_t* except);

/**
 * @brief Encrypts a queued message using the private copy of its text and keys.
 * This lets a worker prepare it after the caller releases the clients lock,
 * even if the client disconnects or reconnects through another socket meanwhile.
 *
 * @param void* context -> outgoing_metadata_t owned by this queued message
 * @param size_t* size -> receives the encrypted message length, excluding the terminating zero
 *
 * @return char* -> allocated encrypted text for the queue to release, or NULL_POINTER on failure
 */
static char* _server_msg_delivery_internal__encode_queued_text(void* context, size_t* size)
{
    outgoing_metadata_t* outgoing = context;
    int64 encoded_size = 0;
    char* encoded = metadata_crypto__encrypt(outgoing->text, &encoded_size, outgoing->session.is_ready ? &outgoing->session : NULL_POINTER);

    *size = encoded_size > 0 ? (size_t)encoded_size - 1 : 0;
    return encoded;
}

/**
 * @brief Erases and frees a queued message's private copy of its text and keys.
 *
 * @param void* context -> outgoing_metadata_t to release, or NULL_POINTER
 *
 * @return void
 */
static void _server_msg_delivery_internal__release_queued_text(void* context)
{
    outgoing_metadata_t* outgoing = context;

    if (outgoing != NULL_POINTER)
    {
        mbedtls_platform_zeroize(outgoing, sizeof(*outgoing) + outgoing->text_size + 1);
        memorymanager__free((nuint)outgoing);
    }
}

/**
 * @brief Frees the encrypted text after the queue has sent it or cancelled delivery.
 *
 * @param char* text -> encrypted message allocated by metadata_crypto__encrypt
 *
 * @return void
 */
void server_msg__delivery__release_encoded_text(char* text)
{
    memorymanager__free((nuint)text);
}

/**
 * @brief Copies a message and its connection keys into the socket's outgoing
 * queue so a worker can encrypt it and the socket writer can send it later.
 *
 * @param ws_cli_conn_t* socket -> the chat connection that will receive the message
 * @param const crypto_session_keys_t* session -> connection keys to copy, or NULL_POINTER before the key exchange
 * @param const char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 * @param boole bulk -> TRUE lets file delivery retry when the queue is busy
 * @param boole keepalive -> TRUE sends a connection-check reply before waiting messages
 *
 * @attention The caller holds the clients lock while choosing the socket and
 * copying its keys. The queue takes ownership of the copy, including on failure.
 * Server-wide keys and the message-size limit cannot change after startup, so
 * workers can safely read them without the clients lock.
 *
 * @return int -> 0 queued, 1 busy for file delivery, -1 failed. Being queued
 * does not mean the message has already reached the client.
 */
int server_msg__delivery__queue_text(ws_cli_conn_t* socket, const crypto_session_keys_t* session, const char* plaintext, boole bulk, boole keepalive)
{
    size_t text_size = 0;
    size_t charged_bytes = 0;
    outgoing_metadata_t* outgoing = NULL_POINTER;

    if (socket == NULL_POINTER || plaintext == NULL_POINTER)
    {
        return -1;
    }
    text_size = strlen(plaintext);
    if (text_size > (size_t)g_server_settings.websocket_message_max_length || text_size > (SIZE_MAX - sizeof(*outgoing) - 8192) / 5)
    {
        ws_abort_client(socket);
        return -1;
    }
    outgoing = (outgoing_metadata_t*)memorymanager__allocate(sizeof(*outgoing) + text_size + 1, MEMALLOC_TYPE_ENCRYPT);
    if (outgoing == NULL_POINTER)
    {
        ws_abort_client(socket);
        return -1;
    }
    outgoing->text_size = text_size;
    memcpy(outgoing->text, plaintext, text_size + 1);
    if (session != NULL_POINTER)
    {
        outgoing->session = *session;
    }
    // Reserve room for the text, encryption buffers, base64 and WebSocket frame.
    // The fixed allowance also covers padding when the message itself is short.
    charged_bytes = sizeof(*outgoing) + text_size * 5 + 8192;
    if (keepalive == TRUE)
    {
        return ws_queue_keepalive(socket, outgoing, charged_bytes, _server_msg_delivery_internal__encode_queued_text, _server_msg_delivery_internal__release_queued_text, server_msg__delivery__release_encoded_text);
    }
    if (bulk == TRUE)
    {
        return ws_try_queue_text(socket, outgoing, charged_bytes, _server_msg_delivery_internal__encode_queued_text, _server_msg_delivery_internal__release_queued_text, server_msg__delivery__release_encoded_text);
    }
    return ws_queue_text(socket, outgoing, charged_bytes, _server_msg_delivery_internal__encode_queued_text, _server_msg_delivery_internal__release_queued_text, server_msg__delivery__release_encoded_text);
}

/**
 * @brief Queues a message for a client's current socket using a private copy of
 * its connection keys and message text for later encryption and delivery.
 *
 * @param client_t* client -> client whose current connection will receive the message
 * @param const char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 *
 * @attention The caller must hold the clients lock until this function returns.
 * The queued message stores its own data and does not keep a client_t pointer.
 *
 * @return boole -> TRUE when the message was queued; FALSE on failure
 */
boole server_msg__delivery__queue_text_to_client(client_t* client, const char* plaintext)
{
    crypto_session_keys_t session = { 0 };
    boole result = FALSE;

    if (client == NULL_POINTER || client->p_ws_connection == NULL_POINTER || plaintext == NULL_POINTER)
    {
        return FALSE;
    }
    if (client->session_keys.is_ready == TRUE)
    {
        return server_msg__delivery__queue_text(client->p_ws_connection, &client->session_keys, plaintext, FALSE, FALSE) == 0;
    }
    if (client->dh_shared_secret[0] != 0)
    {
        if (!metadata_crypto__derive_session_keys(client->dh_shared_secret, &session))
        {
            ws_abort_client(client->p_ws_connection);
            return FALSE;
        }
    }
    result = server_msg__delivery__queue_text(client->p_ws_connection, session.is_ready ? &session : NULL_POINTER, plaintext, FALSE, FALSE) == 0;
    metadata_crypto__clear_session_keys(&session);
    return result;
}

/**
 * @brief starts a message: the root object with a "message" object inside that already carries the type
 *
 * @param const char* type -> the message type the client dispatches on
 * @param cJSON** out_message -> receives the "message" object the fields go into
 *
 * @return cJSON* -> the root, to send and then delete
 */
cJSON* server_msg__delivery__new_message(const char* type, cJSON** out_message)
{
    cJSON* root = cJSON_CreateObject();

    *out_message = cJSON_AddObjectToObject(root, "message");
    cJSON_AddStringToObject(*out_message, "type", type);
    return root;
}

/**
 * @brief Queues a printed message for encrypted delivery to one client's chat socket.
 *
 * @param client_t* client -> client whose current connection will receive the message
 * @param char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 *
 * @attention The caller must hold the clients lock until the message and keys are copied.
 *
 * @return void
 */
void server_msg__delivery__send_text_to_client(client_t* client, char* plaintext)
{
    server_msg__delivery__queue_text_to_client(client, plaintext);
}

/**
 * @brief Queues a message for a particular chat socket. The message and keys are
 * copied now, then encrypted and sent after this function returns.
 *
 * @param ws_cli_conn_t* socket -> the chat connection that will receive the message
 * @param char* dh_shared_secret -> connection's shared secret, or NULL_POINTER before the key exchange;
 * without a shared secret, only the server's configured keys protect the message
 * @param char* plaintext -> zero-terminated message to copy; the caller keeps the original text
 *
 * @attention The caller holds the clients lock while choosing the socket and secret.
 *
 * @return void
 */
static void _server_msg_delivery_internal__send_text(ws_cli_conn_t* socket, char* dh_shared_secret, char* plaintext)
{
    crypto_session_keys_t session = { 0 };

    if (socket == NULL_POINTER || plaintext == NULL_POINTER)
    {
        return;
    }
    if (dh_shared_secret != NULL_POINTER && dh_shared_secret[0] != 0 && !metadata_crypto__derive_session_keys(dh_shared_secret, &session))
    {
        ws_abort_client(socket);
        return;
    }
    server_msg__delivery__queue_text(socket, session.is_ready ? &session : NULL_POINTER, plaintext, FALSE, FALSE);
    metadata_crypto__clear_session_keys(&session);
}

/**
 * @brief Queues a JSON message for encrypted delivery to one socket.
 *
 * @param ws_cli_conn_t* socket -> receiving chat connection
 * @param char* dh_shared_secret -> connection secret, or NULL_POINTER before key exchange
 * @param cJSON* json_root -> message owned by the caller
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void server_msg__delivery__send_json(ws_cli_conn_t* socket, char* dh_shared_secret, cJSON* json_root)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    _server_msg_delivery_internal__send_text(socket, dh_shared_secret, plaintext);
    cJSON_free(plaintext);
}

/**
 * @brief Queues a JSON message for encrypted delivery to one client.
 *
 * @param client_t* client -> receiving client
 * @param cJSON* json_root -> message owned by the caller
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void server_msg__delivery__send_json_to_client(client_t* client, cJSON* json_root)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    server_msg__delivery__send_text_to_client(client, plaintext);
    cJSON_free(plaintext);
}

/**
 * @brief sends a printed message to every authenticated client that is not a music bot
 *
 * @param char* plaintext -> the message text, printed once by the caller
 * @param client_t* except -> one client that does not get it, or NULL_POINTER
 *
 * @attention clients read lock held by the caller
 *
 * @return void
 */
static void _server_msg_delivery_internal__send_text_to_all_clients(char* plaintext, client_t* except)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client == except || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
        {
            continue;
        }
        server_msg__delivery__send_text_to_client(client, plaintext);
    }
}

/**
 * @brief Queues a JSON message for all authenticated clients except music bots.
 *
 * @param cJSON* json_root -> message owned by the caller
 * @param client_t* except -> client to skip, or NULL_POINTER
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void server_msg__delivery__send_json_to_all_clients(cJSON* json_root, client_t* except)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    _server_msg_delivery_internal__send_text_to_all_clients(plaintext, except);
    cJSON_free(plaintext);
}

/**
 * @brief Queues text for authenticated clients in one channel, excluding music bots.
 *
 * @param char* plaintext -> message text to copy
 * @param uint64 channel_id -> receiving channel
 * @param client_t* except -> client to skip, or NULL_POINTER
 * @param cstring receive_type -> attachment type to check, or NULL_POINTER for ordinary messages
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void server_msg__delivery__send_text_to_channel(char* plaintext, uint64 channel_id, client_t* except, cstring receive_type)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];
        if (client == except || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE || client->channel_id != channel_id || (receive_type != NULL_POINTER && base__is_attachment_blocked(client, receive_type)))
        {
            continue;
        }
        server_msg__delivery__send_text_to_client(client, plaintext);
    }
}

/**
 * @brief Queues a JSON message for authenticated clients in one channel, excluding music bots.
 *
 * @param cJSON* json_root -> message owned by the caller
 * @param uint64 channel_id -> receiving channel
 * @param client_t* except -> client to skip, or NULL_POINTER
 *
 * @attention The caller holds the clients lock.
 *
 * @return void
 */
void server_msg__delivery__send_json_to_channel(cJSON* json_root, uint64 channel_id, client_t* except)
{
    char* plaintext = cJSON_PrintUnformatted(json_root);

    server_msg__delivery__send_text_to_channel(plaintext, channel_id, except, NULL_POINTER);
    cJSON_free(plaintext);
}
