// This file sends chat messages, edits, typing notices, pokes, offline messages and URL previews.

#include "server_msg.h"
#include "../../base.h"
#include "../../core/identity_store.h"
#include "../../memory_manager.h"
#include "../../util.h"
#include "../../clib/clib_memory.h"
#include "../../clib/clib_string.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"

/**
 * @brief metadata are sent, so that the client knows an image is being sent to it.
 *
 * @param client_t* receiver -> the client that sent the message
 * @param uint64 chat_message_id -> id of server message
 * @param uint64 local_message_id -> id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(client_t* receiver, uint64 chat_message_id, uint64 local_message_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("server_chat_message_id_for_local_message_id", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddNumberToObject(message, "server_chat_message_id", chat_message_id);
    cJSON_AddNumberToObject(message, "local_message_id", local_message_id);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief send chat message itself to client
 *
 * @param client_t* receiver -> the client that receives the message
 * @param client_t* sender -> the client that sent it
 * @param uint64 server_chat_message_id -> server assigned id of the chat message
 * @param char* chat_message_value -> text content of the chat message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__chat__send_message_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, char* chat_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("direct_chat_message", &message);

    cJSON_AddStringToObject(message, "value", chat_message_value);
    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", sender->client_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", server_chat_message_id);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param client_t* sender -> client that sent the message
 * @param uint64 channel_id -> channel whose other members receive the message
 * @param uint64 server_chat_message_id -> server assigned id of the chat message
 * @param char* chat_message_value -> message content to forward
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__chat__send_message_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* chat_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_chat_message", &message);

    cJSON_AddStringToObject(message, "value", chat_message_value);
    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", server_chat_message_id);

    server_msg__delivery__send_json_to_channel(root, channel_id, sender);

    cJSON_Delete(root);
}

/**
 * @brief sends a delete/edit action for a chat message to a SINGLE client (private/direct case). the action
 *        carries the target server chat id and the requester's public key + admin flag, so the receiver can
 *        decide for itself whether to honour it (author or admin). for an edit it also carries the new value.
 *
 * @param client_t* receiver -> the client to send the action to
 * @param char* action_type -> "chat_message_delete" or "chat_message_edit"
 * @param uint64 target_chat_message_id -> server chat id of the message to act on
 * @param char* requester_public_key -> public key of the client that requested the action
 * @param boole requester_is_admin -> whether the requester is an admin
 * @param char* new_message_value -> new text for an edit, or NULL_POINTER for a delete
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__chat__send_message_action_to_single_client(client_t* receiver, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message(action_type, &message);

    cJSON_AddNumberToObject(message, "chat_message_id", target_chat_message_id);
    cJSON_AddStringToObject(message, "requester_public_key", requester_public_key);
    cJSON_AddBoolToObject(message, "requester_is_admin", requester_is_admin == TRUE);
    if (new_message_value != NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "new_message_value", new_message_value);
    }

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends a delete/edit action for a chat message to every client in a channel (channel case). unlike a
 *        normal chat broadcast this does NOT skip the requester, so the message updates in their own view too.
 *
 * @param uint64 receiving_channel_id -> the channel whose clients receive the action
 * @param char* action_type -> "chat_message_delete" or "chat_message_edit"
 * @param uint64 target_chat_message_id -> server chat id of the message to act on
 * @param char* requester_public_key -> public key of the client that requested the action
 * @param boole requester_is_admin -> whether the requester is an admin
 * @param char* new_message_value -> new text for an edit, or NULL_POINTER for a delete
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__chat__send_message_action_to_clients_in_same_channel(uint64 receiving_channel_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message(action_type, &message);

    cJSON_AddNumberToObject(message, "chat_message_id", target_chat_message_id);
    cJSON_AddStringToObject(message, "requester_public_key", requester_public_key);
    cJSON_AddBoolToObject(message, "requester_is_admin", requester_is_admin == TRUE);
    if (new_message_value != NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "new_message_value", new_message_value);
    }

    server_msg__delivery__send_json_to_channel(root, receiving_channel_id, NULL_POINTER);

    cJSON_Delete(root);
}

/**
 * @brief sends a poke message to a single client
 *
 * @param client_t* receiver -> the client to poke
 * @param client_t* sender -> the client sending the poke
 * @param char* poke_message -> the poke text
 *
 * @return void
 */
void server_msg__chat__send_poke_to_single_client(client_t* receiver, client_t* sender, char* poke_message)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("poke", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)sender->client_id);
    cJSON_AddStringToObject(message, "poke_message", poke_message);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells the people a client is writing to that the client is typing. carries no message content at
 *        all - only who is typing and which conversation it belongs to, so the receiver can show it
 *        against the right chat and drop it again when it stops being refreshed.
 *
 * @param client_t* subject -> the client that is typing
 * @param char* receiver_type -> "channel" or "user", the kind of conversation being written to
 * @param uint64 receiver_id -> channel id for "channel", target client id for "user"
 *
 * @attention the caller must already hold a read lock on clients_global_rwlock_guard
 *
 * @return void
 */
void server_msg__chat__send_typing_indicator(client_t* subject, char* receiver_type, uint64 receiver_id)
{
    uint64 i = 0;
    boole is_direct_message = FALSE;
    client_t* client_in_loop = 0;
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    is_direct_message = clib__is_string_equal(receiver_type, "user");

    root = server_msg__delivery__new_message("typing_indicator", &message);

    cJSON_AddNumberToObject(message, "client_id", (double)subject->client_id);
    cJSON_AddStringToObject(message, "receiver_type", receiver_type);
    cJSON_AddNumberToObject(message, "receiver_id", (double)receiver_id);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE || client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop == subject)
        {
            continue; // he knows he is typing
        }

        // a direct message goes to that one person, a channel one to whoever stands in the channel
        if (is_direct_message == TRUE)
        {
            if (client_in_loop->client_id != receiver_id)
            {
                continue;
            }
        }
        else if (client_in_loop->channel_id != receiver_id)
        {
            continue;
        }

        server_msg__delivery__send_json_to_client(client_in_loop, root);
    }

    cJSON_Delete(root);
}

/**
 * @brief delivers everything that was said to this client's identity while it was offline, oldest
 *        first, then drops the queue for that identity. each message goes out as its own
 *        offline_chat_message so the client can render them like any other direct message; the
 *        payload is the sender's ciphertext, untouched (the server never could read it).
 *
 * @param client_t* receiver -> the freshly authenticated client
 *
 * @return void
 *
 * @note takes g_offline_messages_mutex (leaf lock). caller holds the clients lock.
 */
void server_msg__chat__send_queued_offline_messages_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    char recipient_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 delivered_count = 0;
    uint64 lowest_sequence_number = 0;
    int64 next_slot = 0;

    if (g_offline_messages == NULL_POINTER || receiver == NULL_POINTER || receiver->public_key[0] == 0)
    {
        return;
    }

    util__hash_password_to_base64(receiver->public_key, recipient_identity_hash, sizeof(recipient_identity_hash));

    // deliver in the order they were sent: repeatedly pick the lowest sequence number still queued
    // for this identity. the queue is small and this runs once per connect
    while (TRUE)
    {
        next_slot = -1;
        lowest_sequence_number = 0;

        pthread_mutex_lock(&g_offline_messages_mutex);

        for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
        {
            if (g_offline_messages[i].is_used == FALSE)
            {
                continue;
            }

            if (clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], recipient_identity_hash) == FALSE)
            {
                continue;
            }

            if (next_slot == -1 || g_offline_messages[i].sequence_number < lowest_sequence_number)
            {
                next_slot = (int64)i;
                lowest_sequence_number = g_offline_messages[i].sequence_number;
            }
        }

        if (next_slot == -1)
        {
            pthread_mutex_unlock(&g_offline_messages_mutex);
            break;
        }

        root = server_msg__delivery__new_message("offline_chat_message", &message);

        cJSON_AddStringToObject(message, "value", g_offline_messages[next_slot].base64_encrypted_message);
        cJSON_AddStringToObject(message, "sender_alias", &g_offline_messages[next_slot].sender_alias[0]);
        cJSON_AddNumberToObject(message, "queued_unix_seconds", (double)g_offline_messages[next_slot].queued_unix_seconds);
        cJSON_AddNumberToObject(message, "sequence_number", (double)g_offline_messages[next_slot].sequence_number);

        // free this one before sending, so a failing send cannot leave it queued forever
        if (g_offline_messages[next_slot].base64_encrypted_message != NULL_POINTER)
        {
            memorymanager__free((nuint)g_offline_messages[next_slot].base64_encrypted_message);
        }
        clib__null_memory(&g_offline_messages[next_slot], sizeof(offline_chat_message_t));

        pthread_mutex_unlock(&g_offline_messages_mutex);

        server_msg__delivery__send_json_to_client(receiver, root);

        cJSON_Delete(root);

        delivered_count++;
    }

    if (delivered_count > 0)
    {
        DBG_SERVER_MESSAGE log_info("%s %llu %s", "delivered", delivered_count, "offline message(s) on connect \n");
    }
}

/**
 * @brief Queues an encrypted URL preview reply for the client that requested it.
 *
 * @param client_t* client -> receiving client
 * @param const char* request_id -> id used to match the reply to its request
 * @param const char* url -> requested website URL
 * @param const cJSON* result -> preview to copy, or NULL_POINTER when unavailable or disabled
 *
 * @attention The caller holds the clients lock and keeps ownership of result.
 *
 * @return void
 */
void server_msg__chat__send_url_resolved_to_single_client(client_t* client, const char* request_id, const char* url, const cJSON* result)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* message = result != NULL_POINTER ? cJSON_Duplicate(result, TRUE) : cJSON_CreateObject();
    cJSON_AddItemToObject(root, "message", message);
    cJSON_AddStringToObject(message, "type", "url_resolved");
    cJSON_AddStringToObject(message, "request_id", request_id);
    if (result == NULL_POINTER)
    {
        cJSON_AddStringToObject(message, "url", url);
        cJSON_AddStringToObject(message, "status", g_server_settings.is_link_preview_allowed ? "unavailable" : "disabled");
    }
    server_msg__delivery__send_json_to_client(client, root);
    cJSON_Delete(root);
}
