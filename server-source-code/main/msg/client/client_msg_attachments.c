// This file checks uploaded file pieces, requests to deliver completed uploads and attachment receive preferences.
#include "client_msg.h"
#include "../../core/attachments.h"
#include "../server/server_msg.h"
#include "../../core/musicbot.h"
#include "../../base.h"
#include "../../memory_manager.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_attachments_internal__is_client_msg_file_send_request_valid(cJSON* json_root);
static boole _client_msg_attachments_internal__is_file_send_completed_request_valid(cJSON* json_root);

/**
 * @brief Remembers which new attachments this client does not want to receive.
 * @param cJSON* json_root -> request with all four boolean receive preferences
 * @param uint64 sender_client_id -> client changing its own preferences
 * @param ws_cli_conn_t* socket -> connection that sent the request
 * @return void
 */
void client_msg__attachments__process_attachment_receive_preferences(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket)
{
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* direct_pictures = cJSON_GetObjectItemCaseSensitive(message, "block_direct_chat_pictures");
    cJSON* direct_files = cJSON_GetObjectItemCaseSensitive(message, "block_direct_chat_files");
    cJSON* channel_pictures = cJSON_GetObjectItemCaseSensitive(message, "block_channel_chat_pictures");
    cJSON* channel_files = cJSON_GetObjectItemCaseSensitive(message, "block_channel_chat_files");
    client_t* client = NULL_POINTER;

    if (!cJSON_IsBool(direct_pictures) || !cJSON_IsBool(direct_files) || !cJSON_IsBool(channel_pictures) || !cJSON_IsBool(channel_files) || sender_client_id >= g_server_settings.max_client_count)
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    client = &g_clients_array[sender_client_id];
    if (client->is_existing && client->is_authenticated && client->p_ws_connection == socket)
    {
        client->block_direct_chat_pictures = cJSON_IsTrue(direct_pictures);
        client->block_direct_chat_files = cJSON_IsTrue(direct_files);
        client->block_channel_chat_pictures = cJSON_IsTrue(channel_pictures);
        client->block_channel_chat_files = cJSON_IsTrue(channel_files);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a file-send request: total length, base64 data chunk and the is-new-file flag are present and within limits
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_attachments_internal__is_client_msg_file_send_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_message_total_length = 0;
    cJSON* json_message_data_part_base64 = 0;
    cJSON* json_message_is_new_file = 0;
    uint64 maximum_chunk_length = file_transfer__get_max_file_upload_length() / 400 + 1;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_message_total_length = cJSON_GetObjectItemCaseSensitive(json_message_object, "total_bytes_length");
    if (json_message_total_length == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_message_total_length == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_message_total_length) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(json_message_total_length) \n");
        return FALSE;
    }

    if (json_message_total_length->valueint <= 0 || (uint64)json_message_total_length->valueint >= file_transfer__get_max_file_upload_length())
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_message_total_length->valueint out of allowed range \n");
        return FALSE;
    }

    json_message_data_part_base64 = cJSON_GetObjectItemCaseSensitive(json_message_object, "data_part_base64");
    if (json_message_data_part_base64 == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_message_data_part_base64 == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsString(json_message_data_part_base64) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsString(data_part_base64) \n");
        return FALSE;
    }

    // Accept the new 16 KiB pieces and the larger pieces older clients used
    // for big uploads. The declared total length still limits the whole file.
    if (maximum_chunk_length < 16384)
    {
        maximum_chunk_length = 16384;
    }
    if (clib__utf8_string_length(json_message_data_part_base64->valuestring) > maximum_chunk_length)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "data_part_base64->valuestring length too large \n");
        return FALSE;
    }

    json_message_is_new_file = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_new_file");
    if (json_message_is_new_file == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_message_is_new_file == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsBool(json_message_is_new_file) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsBool(json_message_is_new_file) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief receives file from client, but keeps it in memory, this function doenst know what to do with it
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__attachments__process_file_send_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_total_length = 0;
    cJSON* json_message_data_part_base64 = 0;
    cJSON* json_message_is_new_file = 0;

    client_t* client_sender = 0;
    int64 data_part_length = 0;

    // idea, don't provide upload file reason in upload file request, simply send the upload reason to server in separate message, after server signals the client that the file upload is done.
    // "hey client, I'm done receiving the file, what should I do with it"
    // that makes checking it easier and doesn't change much of existing logic

    // a new file over the upload cap is refused with a reason, not just dropped by the validator below
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_message_total_length = cJSON_GetObjectItemCaseSensitive(json_message_object, "total_bytes_length");
    json_message_is_new_file = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_new_file");

    if (cJSON_IsNumber(json_message_total_length) == TRUE && cJSON_IsTrue(json_message_is_new_file) == TRUE && json_message_total_length->valueint > 0 && (uint64)json_message_total_length->valueint >= file_transfer__get_max_file_upload_length())
    {
        clib__read_lock(&g_clients_global_rwlock_guard);

        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__attachments__send_file_send_error_to_single_client(&g_clients_array[sender_client_id], "file_too_large", 0);
        }

        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    status = _client_msg_attachments_internal__is_client_msg_file_send_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__attachments__process_file_send_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client_sender = &g_clients_array[sender_client_id];

    if (client_sender->is_authenticated == FALSE || client_sender->is_existing == FALSE)
    {
        DBG_FILE_UPLOAD log_info("%s", "client->is_authenticated == FALSE || client->is_existing == FALSE end");
        goto label_client_msg__process_file_send_request_end;
    }

    json_message_data_part_base64 = cJSON_GetObjectItemCaseSensitive(json_message_object, "data_part_base64");

    // a new file gets a buffer of exactly its declared length (already checked against the upload cap),
    // so the per-client cost is the file, not the cap. a continuation appends to the upload in progress
    if (json_message_is_new_file->valueint == 1)
    {
        if (base__is_server_ram_budget_reached())
        {
            server_msg__attachments__send_file_send_error_to_single_client(client_sender, "server_ram_budget_reached", 0);
            goto label_client_msg__process_file_send_request_end;
        }
        DBG_FILE_UPLOAD log_info("%s", "json_message_is_new_file->valueint == 1");

        if (client_sender->file_upload_extension.file_upload_buffer != NULL_POINTER)
        {
            memorymanager__free((nuint)client_sender->file_upload_extension.file_upload_buffer);
            client_sender->file_upload_extension.file_upload_buffer = NULL_POINTER;
        }

        // +1 keeps the null terminator the intent handlers read the buffer up to
        client_sender->file_upload_extension.file_upload_buffer = (ubyte*)memorymanager__allocate((uint64)json_message_total_length->valueint + 1, MEMALLOC_FILE_UPLOAD_BY_PARTS);
        client_sender->file_upload_extension.buffer_cursor = 0;
        client_sender->file_upload_extension.expected_file_length = 0;

        if (client_sender->file_upload_extension.file_upload_buffer == NULL_POINTER)
        {
            log_info("%s %d %s", "upload buffer allocation failed for", json_message_total_length->valueint, "bytes \n");
            goto label_client_msg__process_file_send_request_end;
        }

        client_sender->file_upload_extension.expected_file_length = json_message_total_length->valueint;
    }
    else
    {
        if (client_sender->file_upload_extension.file_upload_buffer == NULL_POINTER || client_sender->file_upload_extension.expected_file_length != json_message_total_length->valueint)
        {
            DBG_FILE_UPLOAD log_info("%s", "expected length of file and what user is sending is not the same, aborting upload of this file");

            if (client_sender->file_upload_extension.file_upload_buffer != NULL_POINTER)
            {
                memorymanager__free((nuint)client_sender->file_upload_extension.file_upload_buffer);
                client_sender->file_upload_extension.file_upload_buffer = NULL_POINTER;
            }

            client_sender->file_upload_extension.buffer_cursor = 0;
            client_sender->file_upload_extension.expected_file_length = 0;

            goto label_client_msg__process_file_send_request_end;
        }
    }

    // append this base64 chunk at the cursor, then advance the cursor by its length
    data_part_length = clib__utf8_string_length(json_message_data_part_base64->valuestring);

    // never let the accumulated upload exceed the client-declared length: the buffer is exactly that big
    if (client_sender->file_upload_extension.buffer_cursor + data_part_length > client_sender->file_upload_extension.expected_file_length)
    {
        DBG_FILE_UPLOAD log_info("%s", "upload chunk would exceed declared file length, aborting upload");
        memorymanager__free((nuint)client_sender->file_upload_extension.file_upload_buffer);
        client_sender->file_upload_extension.file_upload_buffer = NULL_POINTER;
        client_sender->file_upload_extension.buffer_cursor = 0;
        client_sender->file_upload_extension.expected_file_length = 0;
        goto label_client_msg__process_file_send_request_end;
    }

    clib__copy_memory(json_message_data_part_base64->valuestring, client_sender->file_upload_extension.file_upload_buffer + client_sender->file_upload_extension.buffer_cursor, data_part_length, data_part_length);

    client_sender->file_upload_extension.buffer_cursor += data_part_length;

    DBG_FILE_UPLOAD log_info("%s %llu %s", "client_msg__attachments__process_file_send_request client_sender->file_upload_extension.buffer_cursor", client_sender->file_upload_extension.buffer_cursor, "\n");
    DBG_FILE_UPLOAD log_info("%s %d %s", "client_msg__attachments__process_file_send_request total bytes length", json_message_total_length->valueint, "\n");

    if (client_sender->file_upload_extension.buffer_cursor == json_message_total_length->valueint)
    {
        server_msg__attachments__send_file_send_completed_status_to_single_client(client_sender);
    }

label_client_msg__process_file_send_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a file-send-completed request: the file-send intent and its matching extra-data fields are present and valid
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_attachments_internal__is_file_send_completed_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_file_send_intent = 0;
    cJSON* json_file_send_intent_extra_data = 0;
    boole is_intent_allowed = FALSE;
    cJSON* json_song_name = 0;
    cJSON* json_musicbot_id = 0;
    int64 song_name_length = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;
    cJSON* json_file_header = 0;
    int64 file_header_length = 0;
    boole is_chat_file_intent = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_file_send_intent = cJSON_GetObjectItemCaseSensitive(json_message_object, "file_send_intent");
    json_file_send_intent_extra_data = cJSON_GetObjectItemCaseSensitive(json_message_object, "file_send_intent_extra_data");

    if (json_file_send_intent == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_file_send_intent == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsString(json_file_send_intent) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsString(json_file_send_intent) \n");
        return FALSE;
    }

    if (json_file_send_intent_extra_data == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_file_send_intent_extra_data == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsObject(json_file_send_intent_extra_data) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "!cJSON_IsObject(json_file_send_intent_extra_data \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_file_send_intent->valuestring) == 0 || clib__utf8_string_length(json_file_send_intent->valuestring) > 30)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_file_send_intent->valuestring has wrong length");
        return FALSE;
    }

    if (clib__is_string_equal(json_file_send_intent->valuestring, "musicbot_file") == TRUE)
    {
        is_intent_allowed = TRUE;
    }

    // inline pictures can be switched off by the admin; honest clients refuse locally before uploading
    if (g_server_settings.is_chat_picture_allowed == TRUE)
    {
        if (clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_picture_file") == TRUE)
        {
            is_intent_allowed = TRUE;
        }

        if (clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_picture_file") == TRUE)
        {
            is_intent_allowed = TRUE;
        }
    }

    if (clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_file") == TRUE)
    {
        is_intent_allowed = TRUE;
        is_chat_file_intent = TRUE;
    }

    if (clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_file") == TRUE)
    {
        is_intent_allowed = TRUE;
        is_chat_file_intent = TRUE;
    }

    // a chat file carries its encrypted name/size header next to the body; opaque here, only bounded
    if (is_chat_file_intent == TRUE)
    {
        json_file_header = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "file_header");

        if (json_file_header == NULL_POINTER || cJSON_IsString(json_file_header) == FALSE || json_file_header->valuestring == NULL_POINTER)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "chat file: file_header missing or not a string \n");
            return FALSE;
        }

        file_header_length = clib__utf8_string_length_check_max_length(json_file_header->valuestring, FILE_HEADER_MAX_LENGTH);
        if (file_header_length == -1 || file_header_length == 0)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "chat file: file_header has wrong length \n");
            return FALSE;
        }
    }

    if (is_intent_allowed == TRUE)
    {
        // check more types than musicbot file in future
        if (clib__is_string_equal(json_file_send_intent->valuestring, "musicbot_file") == TRUE)
        {
            json_song_name = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "song_name");
            json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "musicbot_id");

            if (cJSON_IsString(json_song_name) == FALSE)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsString(json_song_name) \n");
                return FALSE;
            }

            if (json_song_name->valuestring == NULL_POINTER)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "json_song_name->valuestring == NULL_POINTER \n");
                return FALSE;
            }

            song_name_length = clib__utf8_string_length_check_max_length(json_song_name->valuestring, SONG_NAME_MAX_LENGTH - 1);
            if (song_name_length == -1 || song_name_length == 0)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "song_name_length == -1 || song_name_length == 0 \n");
                return FALSE;
            }

            if (json_musicbot_id == NULL_POINTER)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(musicbot_id) \n");
                return FALSE;
            }

            if (cJSON_IsNumber(json_musicbot_id) == FALSE)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(musicbot_id) \n");
                return FALSE;
            }

            if (json_musicbot_id->valueint < 0 || json_musicbot_id->valueint >= g_server_settings.max_client_count)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "json_musicbot_id is invalid");
                return FALSE;
            }
        }
        else if ((clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_picture_file") == TRUE) || clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_picture_file") == TRUE || is_chat_file_intent == TRUE)
        {
            json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "receiver_id");
            json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "local_message_id");

            if (json_receiver_id == NULL_POINTER)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "client json_receiver_id == NULL_POINTER \n");
                return FALSE;
            }
            if (cJSON_IsNumber(json_receiver_id) == FALSE)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "client :cJSON_IsNumber(json_receiver_id) \n");
                return FALSE;
            }

            if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "client :  json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count \n");
                return FALSE;
            }

            if (json_local_message_id == NULL_POINTER)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "client json_local_message_id == NULL_POINTERn");
                return FALSE;
            }

            if (cJSON_IsNumber(json_local_message_id) == FALSE)
            {
                DBG_CLIENT_MESSAGE log_info("%s", "client cJSON_IsNumber(json_local_message_id) \n");
                return FALSE;
            }
        }
    }

    return TRUE;
}

/**
 * @brief tells server what to do with uploaded file. currently implemented only for mp3s for music bots, plan to do it for channel and direct images
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__attachments__process_file_send_completed_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_file_send_intent = 0;
    cJSON* json_file_send_intent_extra_data = 0;

    cJSON* json_musicbot_id = 0;
    client_t* sender_client = 0;
    client_t* music_bot = 0;
    cJSON* json_song_name = 0;
    musicbot_add_song_arg_struct_t* arguments = NULL_POINTER;
    uint64 thread_id = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;
    cJSON* json_file_header = 0;
    boole is_direct_chat_file = FALSE;
    boole is_channel_chat_file = FALSE;

    status = _client_msg_attachments_internal__is_file_send_completed_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__attachments__process_file_send_completed_request is not valid");
        return;
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__attachments__process_file_send_completed_request is valid");
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // pull the file-send intent and its extra data from the message
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_file_send_intent = cJSON_GetObjectItemCaseSensitive(json_message_object, "file_send_intent");
    json_file_send_intent_extra_data = cJSON_GetObjectItemCaseSensitive(json_message_object, "file_send_intent_extra_data");

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client->is_authenticated == FALSE || client->is_existing == FALSE goto label_client_msg__process_file_send_completed_request_end end");
        goto label_client_msg__process_file_send_completed_request_end;
    }

    // an upload must actually be in progress. file_upload_buffer is NULL on a fresh connection and after a
    // prior upload completes (it is freed and zeroed below), so without this guard the intent handlers pass a
    // NULL buffer to clib__utf8_string_length / the picture handlers, dereferencing address 0 and crashing the
    // whole server. the buffer pointer is the single source of truth for whether an upload is in progress.
    if (sender_client->file_upload_extension.file_upload_buffer == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__attachments__process_file_send_completed_request: no upload in progress, ignoring");
        goto label_client_msg__process_file_send_completed_request_end;
    }

    // not all parts arrived
    if (sender_client->file_upload_extension.expected_file_length == 0 || sender_client->file_upload_extension.buffer_cursor != sender_client->file_upload_extension.expected_file_length)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__attachments__process_file_send_completed_request: upload incomplete, discarding");
        goto label_client_msg__process_file_send_completed_request_end;
    }

    if (clib__is_string_equal(json_file_send_intent->valuestring, "musicbot_file") == TRUE)
    {
        json_song_name = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "song_name");
        json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "musicbot_id");

        if (sender_client->is_admin == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "sender_client->is_admin == FALSE");
            goto label_client_msg__process_file_send_completed_request_end;
        }

        music_bot = &g_clients_array[json_musicbot_id->valueint];
        if (music_bot->is_music_bot == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "music_bot->is_music_bot == FALSE goto label_client_msg__process_file_send_completed_request_end end");
            goto label_client_msg__process_file_send_completed_request_end;
        }

        // copy of song name must be initialized here because by the time the add_music_bot thread gets to it
        // main thread deletes the json holding the song name
        arguments = (musicbot_add_song_arg_struct_t*)memorymanager__allocate(sizeof(musicbot_add_song_arg_struct_t), MEMALLOC_MUSICBOT_SONG);
        if (arguments == NULL_POINTER)
        {
            goto label_client_msg__process_file_send_completed_request_end;
        }
        clib__null_memory(arguments, sizeof(musicbot_add_song_arg_struct_t));

        arguments->music_bot = music_bot;
        arguments->mp3_data_buffer_length = sender_client->file_upload_extension.expected_file_length;
        arguments->mp3_data_buffer = sender_client->file_upload_extension.file_upload_buffer;
        sender_client->file_upload_extension.file_upload_buffer = NULL_POINTER;
        arguments->sender_client_id = sender_client_id;
        clib__copy_memory(json_song_name->valuestring, arguments->song_name, clib__utf8_string_length(json_song_name->valuestring), SONG_NAME_MAX_LENGTH - 1);

        file_transfer__add_uploaded_song(arguments);
    }
    else if (clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_picture_file") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "receiver_id");
        json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "local_message_id");
        file_transfer__process_direct_chat_picture(sender_client_id, json_receiver_id->valueint, json_local_message_id->valueint, sender_client->file_upload_extension.file_upload_buffer);
        DBG_CLIENT_MESSAGE log_info("%s", "direct_chat_picture_file success");
    }
    else if (clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_picture_file") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "receiver_id");
        json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "local_message_id");
        file_transfer__process_channel_chat_picture(sender_client_id, json_local_message_id->valueint, sender_client->file_upload_extension.file_upload_buffer);
    }
    else
    {
        is_direct_chat_file = clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_file");
        is_channel_chat_file = clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_file");

        // chat files: the policy gates answer the sender with a reason; the client also checks both
        // before uploading, so these only fire for a changed setting or a tampered client
        if (is_direct_chat_file == TRUE || is_channel_chat_file == TRUE)
        {
            json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "receiver_id");
            json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "local_message_id");
            json_file_header = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "file_header");

            if (g_server_settings.is_file_upload_allowed == FALSE)
            {
                server_msg__attachments__send_file_send_error_to_single_client(sender_client, "file_uploads_disabled", json_local_message_id->valueint);
            }
            else if (sender_client->file_upload_extension.buffer_cursor > file_transfer__get_max_chat_file_encrypted_length())
            {
                server_msg__attachments__send_file_send_error_to_single_client(sender_client, "file_too_large", json_local_message_id->valueint);
            }
            else if (is_direct_chat_file == TRUE)
            {
                file_transfer__process_direct_chat_file(sender_client_id, json_receiver_id->valueint, json_local_message_id->valueint, sender_client->file_upload_extension.file_upload_buffer, json_file_header->valuestring);
            }
            else
            {
                file_transfer__process_channel_chat_file(sender_client_id, json_local_message_id->valueint, sender_client->file_upload_extension.file_upload_buffer, json_file_header->valuestring);
            }
        }
    }

    // no matter what the reason for calling client_msg__attachments__process_file_send_completed_request was
    // it got called, so free the file
    if (sender_client->file_upload_extension.file_upload_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)sender_client->file_upload_extension.file_upload_buffer);
    }
    sender_client->file_upload_extension.buffer_cursor = 0;
    sender_client->file_upload_extension.expected_file_length = 0;
    sender_client->file_upload_extension.file_upload_buffer = 0;

label_client_msg__process_file_send_completed_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}
