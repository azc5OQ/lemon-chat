// This file sends picture and file information, file chunks and transfer results.

#include "server_msg.h"
#include "../../base.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static void _server_msg_attachments_internal__send_attachment_to_channel(cJSON* root, uint64 channel_id, client_t* except, cstring receive_type);

/**
 * @brief Announces a channel attachment only to clients who will receive its data.
 * @param cJSON* root -> attachment details owned by the caller
 * @param uint64 channel_id -> receiving channel
 * @param client_t* except -> sender to skip
 * @param cstring receive_type -> channel_chat_picture or channel_chat_file
 * @return void
 * @attention The caller holds the clients lock.
 */
static void _server_msg_attachments_internal__send_attachment_to_channel(cJSON* root, uint64 channel_id, client_t* except, cstring receive_type)
{
    char* plaintext = cJSON_PrintUnformatted(root);

    server_msg__delivery__send_text_to_channel(plaintext, channel_id, except, receive_type);
    cJSON_free(plaintext);
}

/**
 * @brief sends chat-picture metadata to all clients in the sender's channel
 *
 * @param client_t* sender -> client that sent the picture
 * @param uint64 channel_id -> channel whose other members receive the picture details
 * @param uint64 server_chat_message_id -> server assigned id of the picture
 * @param uint64 encrypted_size -> encrypted picture length in bytes
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__attachments__send_channel_chat_picture_metadata_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_chat_picture_metadata", &message);

    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "picture_id", server_chat_message_id);
    // the encrypted length the receiver counts its chunks against, for the progress ring
    cJSON_AddNumberToObject(message, "size", (double)encrypted_size);

    _server_msg_attachments_internal__send_attachment_to_channel(root, channel_id, sender, "channel_chat_picture");

    cJSON_Delete(root);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param client_t* sender -> the client that sent it
 * @param uint64 channel_id -> the channel whose members receive it
 * @param uint64 server_chat_message_id -> server assigned id of the picture message
 * @param char* chat_message_value -> self explanatory
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__attachments__send_channel_chat_picture_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* chat_message_value)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_chat_picture", &message);

    cJSON_AddStringToObject(message, "value", chat_message_value);
    cJSON_AddStringToObject(message, "username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "picture_id", server_chat_message_id);

    server_msg__delivery__send_json_to_channel(root, channel_id, sender);

    cJSON_Delete(root);
}

/**
 * @brief This function sends the image status to the client that SENT the image, so the client knows that the server received and sent its message to the other clients / client
 *
 * @param client_t* receiver -> self explanatory
 * @param char* status -> status string that is put into the "value" field of the image_sent_status message
 *
 * @attention thanks for your attention
 *
 * @return void
 */
void server_msg__attachments__send_image_status_to_single_client(client_t* receiver, char* status)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("image_sent_status", &message);

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    cJSON_AddStringToObject(message, "value", status);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief send chat picture metadata to a single client
 *
 * @param client_t* receiver -> the client that receives the message
 * @param client_t* sender -> the client that sent it
 * @param uint64 server_chat_message_id -> server assigned id of the picture
 * @param uint64 encrypted_size -> encrypted picture length in bytes
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__attachments__send_chat_picture_metadata_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("direct_chat_picture_metadata", &message);

    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", sender->client_id);
    cJSON_AddNumberToObject(message, "picture_id", server_chat_message_id);
    // the encrypted length the receiver counts its chunks against, for the progress ring
    cJSON_AddNumberToObject(message, "size", (double)encrypted_size);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends chat-file metadata to all clients in the sender's channel: the sender's encrypted
 *        name/size/mime header (opaque here) and the encrypted body length, so receivers can draw
 *        the file card and a progress ring before the chunks arrive
 *
 * @param client_t* sender -> client that sent the file
 * @param uint64 channel_id -> channel whose other members receive the file details
 * @param uint64 server_chat_message_id -> server assigned id of the file
 * @param char* file_header -> the sender's encrypted header, forwarded as is
 * @param uint64 encrypted_size -> length of the encrypted body that follows in chunks
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__attachments__send_channel_chat_file_metadata_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("channel_chat_file_metadata", &message);

    cJSON_AddNumberToObject(message, "sender_id", (double)sender->client_id);
    cJSON_AddNumberToObject(message, "channel_id", channel_id);
    cJSON_AddNumberToObject(message, "file_id", server_chat_message_id);
    cJSON_AddStringToObject(message, "file_header", file_header);
    cJSON_AddNumberToObject(message, "encrypted_size", (double)encrypted_size);

    _server_msg_attachments_internal__send_attachment_to_channel(root, channel_id, sender, "channel_chat_file");

    cJSON_Delete(root);
}

/**
 * @brief sends chat-file metadata to a single client (direct chat): the sender's encrypted
 *        name/size/mime header (opaque here) and the encrypted body length
 *
 * @param client_t* receiver -> the client that receives the message
 * @param client_t* sender -> the client that sent it
 * @param uint64 server_chat_message_id -> server assigned id of the file
 * @param char* file_header -> the sender's encrypted header, forwarded as is
 * @param uint64 encrypted_size -> length of the encrypted body that follows in chunks
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__attachments__send_chat_file_metadata_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("direct_chat_file_metadata", &message);

    cJSON_AddStringToObject(message, "sender_username", sender->username);
    cJSON_AddNumberToObject(message, "sender_id", sender->client_id);
    cJSON_AddNumberToObject(message, "file_id", server_chat_message_id);
    cJSON_AddStringToObject(message, "file_header", file_header);
    cJSON_AddNumberToObject(message, "encrypted_size", (double)encrypted_size);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells the client that SENT a chat file why the server refused it, so the failure is never
 *        silent on its side. reasons: file_uploads_disabled, file_too_large, receiver_unavailable,
 *        private_messages_disabled
 *
 * @param client_t* receiver -> the sender
 * @param char* reason -> one of the reason strings above
 * @param uint64 local_message_id -> the sender's own id of the refused message, so it can mark that card
 *
 * @attention this function is used within acquired lock on clients array
 *
 * @return void
 */
void server_msg__attachments__send_file_send_error_to_single_client(client_t* receiver, char* reason, uint64 local_message_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s %s %s", "server_msg__attachments__send_file_send_error_to_single_client", reason, "\n");

    root = server_msg__delivery__new_message("file_send_error", &message);

    cJSON_AddStringToObject(message, "reason", reason);
    cJSON_AddNumberToObject(message, "local_message_id", (double)local_message_id);
    cJSON_AddNumberToObject(message, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief tells the client its file upload completed successfully, so it can send instructions on what to do with the file
 *
 * @param client_t* receiver -> the client whose file upload completed
 * @todo make image file upload use this function
 *
 * @attention this function is used within acquired readlock within msg/client/ , multiple functions
 *
 * @return void
 */
void server_msg__attachments__send_file_send_completed_status_to_single_client(client_t* receiver)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("file_send_success", &message);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief sends one chunk of a file transfer as a file_receive_chunk message to the single receiving
 *        client, encrypted with that receiver's shared secret.
 *
 *        the receiver is looked up by index in g_clients_array. the chunk is passed through as the
 *        "value" field exactly as given, together with the sender id and the server chat message id so
 *        the client can append the chunk to the right transfer.
 *
 * @param client_t* receiver -> the client this chunk goes to
 * @param client_t* sender -> the client that sends the file
 * @param char* chunk -> the already prepared chunk payload, sent as the "value" field
 * @param uint64 current_size -> size counter of the transfer so far, not written into the message
 * @param uint64 server_chat_message_id -> id of the chat message this file belongs to
 *
 * @return void
 */
int server_msg__attachments__send_file_by_chunk_to_single_client(client_t* receiver, client_t* sender, char* chunk, uint64 current_size, uint64 server_chat_message_id)
{
    char* plaintext = server_msg__attachments__prepare_file_chunk(sender->client_id, chunk, server_chat_message_id);
    int result = -1;

    if (plaintext != NULL_POINTER)
    {
        result = server_msg__attachments__queue_file_chunk(receiver, plaintext);
    }
    cJSON_free(plaintext);
    return result;
}

/**
 * @brief Builds one file-piece message so it can be reused for every receiver.
 * @param uint64 sender_id -> client sending the file
 * @param char* chunk -> encrypted file bytes, ending with a zero byte
 * @param uint64 server_chat_message_id -> message containing the file
 * @return char* -> JSON text to release with cJSON_free, or NULL_POINTER on failure
 */
char* server_msg__attachments__prepare_file_chunk(uint64 sender_id, char* chunk, uint64 server_chat_message_id)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    char* plaintext = NULL_POINTER;

    root = server_msg__delivery__new_message("file_receive_chunk", &message);

    cJSON_AddStringToObject(message, "value", chunk);
    cJSON_AddNumberToObject(message, "sender_id", (double)sender_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", server_chat_message_id);

    plaintext = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return plaintext;
}

/**
 * @brief Tries to queue a file piece without waiting for a slow receiver.
 * @param client_t* receiver -> current connection, protected by the clients lock
 * @param const char* plaintext -> prepared message; the caller keeps its copy
 * @return int -> 0 queued, 1 retry later, -1 connection unavailable
 */
int server_msg__attachments__queue_file_chunk(client_t* receiver, const char* plaintext)
{
    return server_msg__delivery__queue_text(receiver->p_ws_connection, &receiver->session_keys, plaintext, TRUE, FALSE);
}

/**
 * @brief tells one receiving client that a file transfer is finished, as a file_receive_completed
 *        message encrypted with that receiver's shared secret.
 *
 *        sender and receiver are both looked up by index in g_clients_array. the message carries the
 *        receive type, the sender's client id and the server chat message id, so the client can close
 *        the right transfer and render it.
 *
 * @param client_t* receiver -> the client that gets the notification
 * @param data_for_file_send_thread_t* info -> the transfer descriptor, read for client_sender_id and server_chat_message_id
 * @param char* receive_type -> kind of transfer, sent as the "receive_type" field
 *
 * @return void
 */
void server_msg__attachments__send_file_receive_completed_to_single_client(client_t* receiver, data_for_file_send_thread_t* info, char* receive_type)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", __func__);

    root = server_msg__delivery__new_message("file_receive_completed", &message);

    cJSON_AddStringToObject(message, "receive_type", receive_type);
    cJSON_AddNumberToObject(message, "sender_id", (double)info->client_sender_id);
    cJSON_AddNumberToObject(message, "server_chat_message_id", info->server_chat_message_id);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}
