// This file receives uploaded pictures and files and sends them to the intended clients or music bot.
#include "channel.h"
#include "attachments.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"
#include "../msg/server/server_msg.h"
#include "../memory_manager.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"
#include "../util.h"
#include "musicbot.h"
#include "../../third-party/zhicheng/base64.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"

// declarations
static void* _file_transfer_internal__add_song_thread(void* argument);
static void _file_transfer_internal__start_file_download(data_for_file_send_thread_t* arg);
static boole _file_transfer_internal__file_client_is_current(uint64 client_id, const crypto_key_t* authentication_key);
static int _file_transfer_internal__queue_file_chunk(data_for_file_send_thread_t* arg, uint64 receiver_id, const crypto_key_t* receiver_key, const char* plaintext);
static uint64 _file_transfer_internal__get_max_chat_picture_encrypted_length(void);
static void* _file_transfer_internal__file_download_thread(void* argument);

/**
 * @brief Checks that a file recipient or sender still uses the original connection.
 * @param uint64 client_id -> client slot to check while holding the clients lock
 * @param const crypto_key_t* authentication_key -> key copied when delivery began
 * @return boole -> TRUE for the original authenticated connection
 */
static boole _file_transfer_internal__file_client_is_current(uint64 client_id, const crypto_key_t* authentication_key)
{
    if (client_id >= g_server_settings.max_client_count || base__is_client_valid_and_not_music_bot(client_id) == FALSE)
    {
        return FALSE;
    }
    return g_clients_array[client_id].session_keys.is_ready && g_clients_array[client_id].session_keys.authentication.type == authentication_key->type && memcmp(&g_clients_array[client_id].session_keys.authentication.material, &authentication_key->material, sizeof(authentication_key->material)) == 0;
}

/**
 * @brief Checks the original connections and tries to queue a prepared file piece.
 * @param data_for_file_send_thread_t* arg -> file and original sender connection
 * @param uint64 receiver_id -> receiving client slot
 * @param const crypto_key_t* receiver_key -> recipient's original connection key
 * @param const char* plaintext -> prepared message to copy into the queue
 * @return int -> 0 queued, 1 retry later, -1 connection unavailable
 */
static int _file_transfer_internal__queue_file_chunk(data_for_file_send_thread_t* arg, uint64 receiver_id, const crypto_key_t* receiver_key, const char* plaintext)
{
    int result = -1;

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (_file_transfer_internal__file_client_is_current(arg->client_sender_id, &arg->sender_authentication_key) && _file_transfer_internal__file_client_is_current(receiver_id, receiver_key))
    {
        result = server_msg__attachments__queue_file_chunk(&g_clients_array[receiver_id], plaintext);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
    return result;
}

/**
 * @brief Forwards an encrypted upload in chunks to its selected client or channel recipients.
 * @param void* argument -> owned data_for_file_send_thread_t with the buffer, recipients and message ids
 * @return void* -> NULL_POINTER after releasing the upload buffer and arguments
 */
static void* _file_transfer_internal__file_download_thread(void* argument)
{
    data_for_file_send_thread_t* arg = (data_for_file_send_thread_t*)argument;
    uint64 chunk_size = 16384;
    uint64 offset = 0;
    boole is_sender_valid = FALSE;
    boole is_receiver_valid = FALSE;
    uint64 client_receiver_id = 0;
    uint64 remaining = 0;
    uint64 current_size = 0;
    uint64 receiver_count = arg->send_type == FILE_SEND_TYPE_TO_CHANNEL ? arg->receiving_clients_count : 1;
    uint64 waiting_count = 0;
    uint64 started_ms = 0;
    uint64 i = 0;
    int result = 0;
    char* chunk = NULL_POINTER;
    char* plaintext = NULL_POINTER;
    boole* queued = calloc(receiver_count > 0 ? receiver_count : 1, sizeof(boole));

    if (queued == NULL_POINTER)
    {
        goto file_download_end;
    }

    while (offset < arg->size)
    {
        remaining = arg->size - offset;
        current_size = remaining < chunk_size ? remaining : chunk_size;
        chunk = malloc(current_size + 1);
        if (chunk == NULL_POINTER)
        {
            goto file_download_end;
        }
        clib__copy_memory(arg->buffer + offset, chunk, current_size, current_size + 1);
        chunk[current_size] = '\0';

        // Every recipient receives the same file piece. Build its message once, outside the clients lock.
        plaintext = server_msg__attachments__prepare_file_chunk(arg->client_sender_id, chunk, arg->server_chat_message_id);
        free(chunk);
        chunk = NULL_POINTER;
        if (plaintext == NULL_POINTER)
        {
            goto file_download_end;
        }
        memset(queued, 0, receiver_count * sizeof(boole));
        started_ms = util__get_timestamp_ms();
        do
        {
            waiting_count = 0;
            for (i = 0; i < receiver_count; i++)
            {
                if (queued[i])
                {
                    continue;
                }
                client_receiver_id = arg->send_type == FILE_SEND_TYPE_TO_CHANNEL ? arg->receiving_client_ids[i] : arg->client_receiver_id;
                result = _file_transfer_internal__queue_file_chunk(arg, client_receiver_id, &arg->receiver_authentication_keys[i], plaintext);
                if (result == 1 && util__get_timestamp_ms() - started_ms < 10000)
                {
                    waiting_count++;
                    continue;
                }
                queued[i] = TRUE;
                if (result != 0)
                {
                    if (arg->send_type == FILE_SEND_TYPE_TO_CLIENT)
                    {
                        goto file_download_end;
                    }
                    arg->receiving_client_ids[i] = g_server_settings.max_client_count;
                }
            }
            if (waiting_count > 0)
            {
                // Try every recipient before waiting for those whose queues are full.
                util__sleep_for_milliseconds(5);
            }
        } while (waiting_count > 0);
        cJSON_free(plaintext);
        plaintext = NULL_POINTER;
        offset += current_size;
    }

    if (arg->send_type == FILE_SEND_TYPE_TO_CLIENT)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);

        is_sender_valid = _file_transfer_internal__file_client_is_current(arg->client_sender_id, &arg->sender_authentication_key);
        is_receiver_valid = _file_transfer_internal__file_client_is_current(arg->client_receiver_id, &arg->receiver_authentication_keys[0]);

        if (is_sender_valid == TRUE && is_receiver_valid == TRUE)
        {
            server_msg__attachments__send_file_receive_completed_to_single_client(&g_clients_array[arg->client_receiver_id], arg, &arg->receive_type[0]);
            server_msg__attachments__send_image_status_to_single_client(&g_clients_array[arg->client_sender_id], "success"); // this is for client that sent it
            server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(&g_clients_array[arg->client_sender_id], arg->server_chat_message_id, arg->local_chat_message_id);
        }

        clib__unlock(&g_clients_global_rwlock_guard);
    }
    else if (arg->send_type == FILE_SEND_TYPE_TO_CHANNEL)
    {
        for (i = 0; i < arg->receiving_clients_count; i++)
        {
            clib__read_lock(&g_clients_global_rwlock_guard);

            client_receiver_id = arg->receiving_client_ids[i];

            is_sender_valid = _file_transfer_internal__file_client_is_current(arg->client_sender_id, &arg->sender_authentication_key);
            is_receiver_valid = _file_transfer_internal__file_client_is_current(client_receiver_id, &arg->receiver_authentication_keys[i]);

            if (is_sender_valid == TRUE && is_receiver_valid == TRUE)
            {
                server_msg__attachments__send_file_receive_completed_to_single_client(&g_clients_array[client_receiver_id], arg, &arg->receive_type[0]);
            }

            clib__unlock(&g_clients_global_rwlock_guard);
        }

        // the sender's ack, once. it used to sit inside the loop above: an empty channel never acked
        // (the local card stayed dimmed) and a full one acked once per receiver
        clib__read_lock(&g_clients_global_rwlock_guard);

        if (_file_transfer_internal__file_client_is_current(arg->client_sender_id, &arg->sender_authentication_key) == TRUE)
        {
            server_msg__attachments__send_image_status_to_single_client(&g_clients_array[arg->client_sender_id], "success");
            server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(&g_clients_array[arg->client_sender_id], arg->server_chat_message_id, arg->local_chat_message_id);
        }

        clib__unlock(&g_clients_global_rwlock_guard);
    }
file_download_end:
    free(queued);
    cJSON_free(plaintext);
    mbedtls_platform_zeroize(arg->receiver_authentication_keys, (arg->send_type == FILE_SEND_TYPE_TO_CHANNEL ? arg->receiving_clients_count : 1) * sizeof(crypto_key_t));
    free(arg->receiver_authentication_keys);
    mbedtls_platform_zeroize(&arg->sender_authentication_key, sizeof(crypto_key_t));
    memorymanager__free((nuint)arg->buffer);
    memorymanager__free((nuint)arg);
    return NULL_POINTER;
}

/**
 * @brief Starts sending an uploaded file, or reports server_busy if no worker can be started.
 * @param data_for_file_send_thread_t* arg -> owned upload buffer, recipients and message ids
 * @return void
 * @attention Takes ownership of arg and its buffer even when thread creation fails.
 */
static void _file_transfer_internal__start_file_download(data_for_file_send_thread_t* arg)
{
    uint64 count = arg->send_type == FILE_SEND_TYPE_TO_CHANNEL ? arg->receiving_clients_count : 1;
    uint64 i = 0;
    uint64 receiver_id = 0;

    // If someone changes their receive settings while a file is downloading,
    // let that file finish. The new settings apply to later files.
    if (arg->send_type == FILE_SEND_TYPE_TO_CHANNEL)
    {
        count = 0;
        for (i = 0; i < arg->receiving_clients_count; i++)
        {
            receiver_id = arg->receiving_client_ids[i];
            if (base__is_attachment_blocked(&g_clients_array[receiver_id], arg->receive_type) == FALSE)
            {
                arg->receiving_client_ids[count++] = receiver_id;
            }
        }
        arg->receiving_clients_count = count;
    }

    // A slot may be reused while this worker waits. Remember the connection
    // keys so no remaining part of a file goes to a replacement connection.
    arg->sender_authentication_key = g_clients_array[arg->client_sender_id].session_keys.authentication;
    arg->receiver_authentication_keys = calloc(count > 0 ? count : 1, sizeof(crypto_key_t));
    if (arg->receiver_authentication_keys != NULL_POINTER)
    {
        for (i = 0; i < count; i++)
        {
            receiver_id = arg->send_type == FILE_SEND_TYPE_TO_CHANNEL ? arg->receiving_client_ids[i] : arg->client_receiver_id;
            arg->receiver_authentication_keys[i] = g_clients_array[receiver_id].session_keys.authentication;
        }
    }
    if (arg->receiver_authentication_keys == NULL_POINTER || util__start_detached_thread(_file_transfer_internal__file_download_thread, arg) == FALSE)
    {
        server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[arg->client_sender_id], "server_busy", arg->local_chat_message_id);
        if (arg->receiver_authentication_keys != NULL_POINTER)
        {
            mbedtls_platform_zeroize(arg->receiver_authentication_keys, count * sizeof(crypto_key_t));
            free(arg->receiver_authentication_keys);
        }
        mbedtls_platform_zeroize(&arg->sender_authentication_key, sizeof(crypto_key_t));
        memorymanager__free((nuint)arg->buffer);
        memorymanager__free((nuint)arg);
    }
}

/**
 * @brief This function processes channel chat picture
 *
 * @param uint64 client_sender_id -> id of the client that sent the picture
 * @param uint64 local_message_id -> the client-side message id of the picture
 * @param char* message_value -> the base64 picture data to send to the channel
 *
 * @return void
 */
void file_transfer__process_channel_chat_picture(uint64 client_sender_id, uint64 local_message_id, char* message_value)
{
    boole is_channel_existing = FALSE;
    uint64 channel_id = 0;
    uint64 server_chat_message_id = 0;
    data_for_file_send_thread_t* arg = NULL_POINTER;
    uint64 buffer_to_send_total_size = 0;

    DBG_CLIENT_MESSAGE log_info("%s", "file_transfer__process_channel_chat_picture got here \n");

    channel_id = g_clients_array[client_sender_id].channel_id;
    // bounds check, channel_id is not validated anywhere
    is_channel_existing = (channel_id < g_server_settings.max_channel_count) && g_channel_array[channel_id].is_existing;

    DBG_CLIENT_MESSAGE log_info("%s", "file_transfer__process_channel_chat_picture got here \n");

    if (is_channel_existing == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "file_transfer__process_channel_chat_picture receiving channel exists: ", channel_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        buffer_to_send_total_size = g_clients_array[client_sender_id].file_upload_extension.expected_file_length;
        server_msg__attachments__send_channel_chat_picture_metadata_to_clients_in_same_channel(&g_clients_array[client_sender_id], channel_id, server_chat_message_id, buffer_to_send_total_size);

        arg = (data_for_file_send_thread_t*)memorymanager__allocate(sizeof(data_for_file_send_thread_t) + sizeof(uint64) * g_server_settings.max_client_count, MEMALLOC_FILE_DOWNLOAD_BY_PARTS);
        if (arg == NULL_POINTER)
        {
            return;
        }
        clib__null_memory(arg, sizeof(data_for_file_send_thread_t));
        // Give the worker the completed upload instead of copying the whole picture.
        arg->buffer = message_value;
        g_clients_array[client_sender_id].file_upload_extension.file_upload_buffer = NULL_POINTER;
        arg->client_sender_id = client_sender_id;
        arg->receiving_clients_count = channel__get_other_clients_in_channel(client_sender_id, channel_id, &arg->receiving_client_ids[0]);
        arg->send_type = FILE_SEND_TYPE_TO_CHANNEL;
        arg->size = buffer_to_send_total_size;
        arg->server_chat_message_id = server_chat_message_id;
        arg->local_chat_message_id = local_message_id;
        clib__copy_memory("channel_chat_picture", &arg->receive_type[0], clib__utf8_string_length("channel_chat_picture"), sizeof(arg->receive_type) - 1);

        _file_transfer_internal__start_file_download(arg);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "file_transfer__process_channel_chat_picture receiving channel does not exist: ", channel_id, "\n");
    }
}

/**
 * @brief This function processes direct chat picture
 *
 * @param uint64 sender_client_id -> id of the client that sent the picture
 * @param uint64 receiver_id -> id of the client that receives the picture
 * @param uint64 local_message_id -> the client-side message id of the picture
 * @param char* message_value -> the base64 picture data to send to the receiver
 *
 * @note this function gets called by another processing function, client_msg__attachments__process_file_send_completed_request, that makes it different from the other functions
 *
 * @return void
 */
void file_transfer__process_direct_chat_picture(uint64 sender_client_id, uint64 receiver_id, uint64 local_message_id, char* message_value)
{
    uint64 server_chat_message_id = 0;
    data_for_file_send_thread_t* arg = NULL_POINTER;
    uint64 buffer_to_send_total_size = 0;

    // status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    // if (status == FALSE)
    // {
    // log_info("%s", " base__is_request_allowed_based_on_spam_protection == FALSE \n");
    // return;
    // }
    DBG_CLIENT_MESSAGE log_info("%s", "file_transfer__process_direct_chat_picture got here \n");

    if (base__is_attachment_blocked(&g_clients_array[receiver_id], "direct_chat_picture"))
    {
        server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[sender_client_id], "receiver_blocked_attachment", local_message_id);
        return;
    }

    if (g_clients_array[receiver_id].is_authenticated == TRUE && g_clients_array[receiver_id].is_idle == FALSE && g_clients_array[receiver_id].is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiver is_existing: ", receiver_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        buffer_to_send_total_size = g_clients_array[sender_client_id].file_upload_extension.expected_file_length;
        server_msg__attachments__send_chat_picture_metadata_to_single_client(&g_clients_array[receiver_id], &g_clients_array[sender_client_id], server_chat_message_id, buffer_to_send_total_size);

        arg = (data_for_file_send_thread_t*)memorymanager__allocate(sizeof(data_for_file_send_thread_t), MEMALLOC_FILE_DOWNLOAD_BY_PARTS);
        if (arg == NULL_POINTER)
        {
            return;
        }
        clib__null_memory(arg, sizeof(data_for_file_send_thread_t));
        arg->buffer = message_value;
        g_clients_array[sender_client_id].file_upload_extension.file_upload_buffer = NULL_POINTER;
        arg->client_sender_id = sender_client_id;
        arg->client_receiver_id = receiver_id;
        arg->send_type = FILE_SEND_TYPE_TO_CLIENT;
        arg->size = buffer_to_send_total_size;
        arg->server_chat_message_id = server_chat_message_id;
        arg->local_chat_message_id = local_message_id;
        clib__copy_memory("direct_chat_picture", &arg->receive_type[0], clib__utf8_string_length("direct_chat_picture"), sizeof(arg->receive_type) - 1);

        _file_transfer_internal__start_file_download(arg);
    }
}

/**
 * @brief relays an uploaded chat file to everybody in the sender's channel: the encrypted header goes
 *        out in the metadata right away, the encrypted body follows in chunks from the relay thread
 *
 * @param uint64 client_sender_id -> id of the client that sent the file
 * @param uint64 local_message_id -> the client-side message id of the file
 * @param char* message_value -> the encrypted file body (base64) to send to the channel
 * @param char* file_header -> the encrypted name/size/mime header, forwarded as is
 *
 * @attention called within write lock on clients array, same as the picture version
 *
 * @return void
 */
void file_transfer__process_channel_chat_file(uint64 client_sender_id, uint64 local_message_id, char* message_value, char* file_header)
{
    boole is_channel_existing = FALSE;
    uint64 channel_id = 0;
    uint64 server_chat_message_id = 0;
    data_for_file_send_thread_t* arg = NULL_POINTER;
    uint64 buffer_to_send_total_size = 0;

    channel_id = g_clients_array[client_sender_id].channel_id;
    is_channel_existing = (channel_id < g_server_settings.max_channel_count) && g_channel_array[channel_id].is_existing;

    if (is_channel_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "file_transfer__process_channel_chat_file receiving channel does not exist: ", channel_id, "\n");
        server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[client_sender_id], "receiver_unavailable", local_message_id);
        return;
    }

    buffer_to_send_total_size = g_clients_array[client_sender_id].file_upload_extension.expected_file_length;

    server_chat_message_id = base__get_chat_message_id();
    base__increment_chat_message_id();
    server_msg__attachments__send_channel_chat_file_metadata_to_clients_in_same_channel(&g_clients_array[client_sender_id], channel_id, server_chat_message_id, file_header, buffer_to_send_total_size);

    arg = (data_for_file_send_thread_t*)memorymanager__allocate(sizeof(data_for_file_send_thread_t) + sizeof(uint64) * g_server_settings.max_client_count, MEMALLOC_FILE_DOWNLOAD_BY_PARTS);
    if (arg == NULL_POINTER)
    {
        return;
    }
    clib__null_memory(arg, sizeof(data_for_file_send_thread_t));
    arg->buffer = message_value;
    g_clients_array[client_sender_id].file_upload_extension.file_upload_buffer = NULL_POINTER;
    arg->client_sender_id = client_sender_id;
    arg->receiving_clients_count = channel__get_other_clients_in_channel(client_sender_id, channel_id, &arg->receiving_client_ids[0]);
    arg->send_type = FILE_SEND_TYPE_TO_CHANNEL;
    arg->size = buffer_to_send_total_size;
    arg->server_chat_message_id = server_chat_message_id;
    arg->local_chat_message_id = local_message_id;
    clib__copy_memory("channel_chat_file", &arg->receive_type[0], clib__utf8_string_length("channel_chat_file"), sizeof(arg->receive_type) - 1);

    _file_transfer_internal__start_file_download(arg);
}

/**
 * @brief relays an uploaded chat file to one client. idle receivers are refused (a phone with no ui
 *        attached is idle), and the refusal is reported back to the sender instead of dropped
 *
 * @param uint64 sender_client_id -> id of the client that sent the file
 * @param uint64 receiver_id -> id of the client that receives the file
 * @param uint64 local_message_id -> the client-side message id of the file
 * @param char* message_value -> the encrypted file body (base64) to send to the receiver
 * @param char* file_header -> the encrypted name/size/mime header, forwarded as is
 *
 * @attention called within write lock on clients array, same as the picture version
 *
 * @return void
 */
void file_transfer__process_direct_chat_file(uint64 sender_client_id, uint64 receiver_id, uint64 local_message_id, char* message_value, char* file_header)
{
    uint64 server_chat_message_id = 0;
    data_for_file_send_thread_t* arg = NULL_POINTER;
    uint64 buffer_to_send_total_size = 0;

    if (g_server_settings.is_private_messaging_allowed == FALSE)
    {
        server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[sender_client_id], "private_messages_disabled", local_message_id);
        return;
    }

    if (base__is_attachment_blocked(&g_clients_array[receiver_id], "direct_chat_file"))
    {
        server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[sender_client_id], "receiver_blocked_attachment", local_message_id);
        return;
    }

    if (g_clients_array[receiver_id].is_authenticated == FALSE || g_clients_array[receiver_id].is_idle == TRUE || g_clients_array[receiver_id].is_music_bot == TRUE || receiver_id == sender_client_id)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "file_transfer__process_direct_chat_file receiver unavailable: ", receiver_id, "\n");
        server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[sender_client_id], "receiver_unavailable", local_message_id);
        return;
    }

    buffer_to_send_total_size = g_clients_array[sender_client_id].file_upload_extension.expected_file_length;

    server_chat_message_id = base__get_chat_message_id();
    base__increment_chat_message_id();
    server_msg__attachments__send_chat_file_metadata_to_single_client(&g_clients_array[receiver_id], &g_clients_array[sender_client_id], server_chat_message_id, file_header, buffer_to_send_total_size);

    arg = (data_for_file_send_thread_t*)memorymanager__allocate(sizeof(data_for_file_send_thread_t), MEMALLOC_FILE_DOWNLOAD_BY_PARTS);
    if (arg == NULL_POINTER)
    {
        return;
    }
    clib__null_memory(arg, sizeof(data_for_file_send_thread_t));
    arg->buffer = message_value;
    g_clients_array[sender_client_id].file_upload_extension.file_upload_buffer = NULL_POINTER;
    arg->client_sender_id = sender_client_id;
    arg->client_receiver_id = receiver_id;
    arg->send_type = FILE_SEND_TYPE_TO_CLIENT;
    arg->size = buffer_to_send_total_size;
    arg->server_chat_message_id = server_chat_message_id;
    arg->local_chat_message_id = local_message_id;
    clib__copy_memory("direct_chat_file", &arg->receive_type[0], clib__utf8_string_length("direct_chat_file"), sizeof(arg->receive_type) - 1);

    _file_transfer_internal__start_file_download(arg);
}

/**
 * @brief the longest encrypted chat file body the server accepts, derived from the admin's raw-byte limit
 *
 * @note raw bytes -> base64 data (4/3) -> json wrapper -> aes -> base64 again (4/3). the constant covers
 *       the json field names, the mime/name strings and the rsa-wrapped keys of a direct chat file
 *
 * @return uint64 -> length in base64 chars
 */
uint64 file_transfer__get_max_chat_file_encrypted_length(void)
{
    return (((uint64)g_server_settings.file_upload_max_size_bytes * 16) / 9) + 16384;
}

/**
 * @brief the longest encrypted inline picture the server accepts, derived from the admin's raw-byte limit
 *
 * @note same expansion as a chat file, without the name/mime header
 *
 * @return uint64 -> length in base64 chars
 */
static uint64 _file_transfer_internal__get_max_chat_picture_encrypted_length(void)
{
    return (((uint64)g_server_settings.chat_picture_max_size_bytes * 16) / 9) + 16384;
}

/**
 * @brief the biggest upload a client may declare: a musicbot song, a chat file, or an inline
 *        picture - whichever of the enabled ones is largest
 *
 * @return uint64 -> length in base64 chars
 */
uint64 file_transfer__get_max_file_upload_length(void)
{
    uint64 longest = MAX_CLIENT_FILE_UPLOAD_LENGTH;
    uint64 candidate = 0;

    if (g_server_settings.is_file_upload_allowed == TRUE)
    {
        candidate = file_transfer__get_max_chat_file_encrypted_length();

        if (candidate > longest)
        {
            longest = candidate;
        }
    }

    if (g_server_settings.is_chat_picture_allowed == TRUE)
    {
        candidate = _file_transfer_internal__get_max_chat_picture_encrypted_length();

        if (candidate > longest)
        {
            longest = candidate;
        }
    }

    return longest;
}


/**
 * @brief Decodes an uploaded song outside the clients lock, then adds it to the music bot.
 * @param void* argument -> owned musicbot_add_song_arg_struct_t containing the base64 upload
 * @return void* -> NULL_POINTER when the worker finishes
 */
static void* _file_transfer_internal__add_song_thread(void* argument)
{
    musicbot_add_song_arg_struct_t* upload = argument;
    client_t* bot = upload->music_bot;
    uint64 decoded_length = 0;

    // Decoded bytes take less space than the base64 text. Write them into the
    // same buffer; the decoder reads each character before overwriting it.
    decoded_length = zchg_base64_decode((char*)upload->mp3_data_buffer, upload->mp3_data_buffer_length, upload->mp3_data_buffer);
    if (decoded_length > 0)
    {
        upload->mp3_data_buffer_length = decoded_length;
        musicbot__add_song(upload);
    }
    else
    {
        memorymanager__free((nuint)upload->mp3_data_buffer);
        memorymanager__free((nuint)upload);
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    bot->music_bot_client_extension.pending_song_uploads--;
    clib__unlock(&g_clients_global_rwlock_guard);
    return NULL_POINTER;
}



/**
 * @brief Starts a thread to decode an uploaded song and add it to the music bot.
 * @param musicbot_add_song_arg_struct_t* arguments -> song and buffer owned by this function
 * @attention The caller holds the clients write lock.
 */
void file_transfer__add_uploaded_song(musicbot_add_song_arg_struct_t* arguments)
{
    arguments->music_bot->music_bot_client_extension.pending_song_uploads++;
    if (util__start_detached_thread(_file_transfer_internal__add_song_thread, arguments) == FALSE)
    {
        memorymanager__free((nuint)arguments->mp3_data_buffer);
        arguments->music_bot->music_bot_client_extension.pending_song_uploads--;
        memorymanager__free((nuint)arguments);
    }
}
