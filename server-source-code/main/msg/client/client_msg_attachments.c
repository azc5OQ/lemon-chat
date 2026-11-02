// This file checks uploaded file pieces, completed uploads and attachment receive preferences.
#include "client_msg.h"
#include "../../core/attachments.h"
#include "../../core/permissions.h"
#include "../server/server_msg.h"
#include "../../core/musicbot.h"
#include "../../base.h"
#include "../../memory_manager.h"
#include "../../clib/clib.h"
#include "../../util.h"
#include "../../../third-party/rxi-log/log.h"

typedef enum
{
    ATTACHMENT_INTENT_INVALID,
    ATTACHMENT_INTENT_MUSIC,
    ATTACHMENT_INTENT_DIRECT_PICTURE,
    ATTACHMENT_INTENT_CHANNEL_PICTURE,
    ATTACHMENT_INTENT_DIRECT_FILE,
    ATTACHMENT_INTENT_CHANNEL_FILE
} attachment_intent_t;

typedef struct
{
    attachment_intent_t intent;
    cstring intent_name;
    cstring song_name;
    char* file_header;
    uint64 receiver_id;
    uint64 local_message_id;
} attachment_completion_t;

/**
 * @brief Remembers which new attachments this client does not want to receive.
 * @param cJSON* json_root -> request with all four boolean receive preferences
 * @param uint64 sender_client_id -> client changing its own preferences
 * @param ws_cli_conn_t* socket -> connection that sent the request
 * @return void
 */
void client_msg__attachments__process_attachment_receive_preferences(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket)
{
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    boole status3 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* direct_pictures = cJSON_GetObjectItemCaseSensitive(message, "block_direct_chat_pictures");
    cJSON* direct_files = cJSON_GetObjectItemCaseSensitive(message, "block_direct_chat_files");
    cJSON* channel_pictures = cJSON_GetObjectItemCaseSensitive(message, "block_channel_chat_pictures");
    cJSON* channel_files = cJSON_GetObjectItemCaseSensitive(message, "block_channel_chat_files");
    client_t* client = NULL_POINTER;

    status0 = !cJSON_IsBool(direct_pictures);
    if (status0 == FALSE)
    {
        status0 = !cJSON_IsBool(direct_files);
    }
    if (status0 == FALSE)
    {
        status0 = !cJSON_IsBool(channel_pictures);
    }
    if (status0 == FALSE)
    {
        status0 = !cJSON_IsBool(channel_files);
    }
    if (status0 == FALSE)
    {
        status0 = sender_client_id >= g_server_settings.max_client_count;
    }
    if (status0 == TRUE)
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    client = &g_clients_array[sender_client_id];
    status0 = (base__is_current_client_request(sender_client_id)) != FALSE;
    if (status0 == TRUE)
    {
        status1 = (client->is_existing) != FALSE;
        status2 = (client->is_authenticated) != FALSE;
        status3 = client->p_ws_connection == socket;
        status0 = status1 == TRUE && status2 == TRUE && status3 == TRUE;
    }
    if (status0 == TRUE)
    {
        client->block_direct_chat_pictures = cJSON_IsTrue(direct_pictures);
        client->block_direct_chat_files = cJSON_IsTrue(direct_files);
        client->block_channel_chat_pictures = cJSON_IsTrue(channel_pictures);
        client->block_channel_chat_files = cJSON_IsTrue(channel_files);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Recognizes every supported upload intent independently of server policy.
 */
static attachment_intent_t _client_msg_attachments_internal__read_intent(cstring intent)
{
    if (intent == NULL_POINTER)
    {
        return ATTACHMENT_INTENT_INVALID;
    }
    if (clib__strcmp(intent, "musicbot_file") == 0)
    {
        return ATTACHMENT_INTENT_MUSIC;
    }
    if (clib__strcmp(intent, "direct_chat_picture_file") == 0)
    {
        return ATTACHMENT_INTENT_DIRECT_PICTURE;
    }
    if (clib__strcmp(intent, "channel_chat_picture_file") == 0)
    {
        return ATTACHMENT_INTENT_CHANNEL_PICTURE;
    }
    if (clib__strcmp(intent, "direct_chat_file") == 0)
    {
        return ATTACHMENT_INTENT_DIRECT_FILE;
    }
    if (clib__strcmp(intent, "channel_chat_file") == 0)
    {
        return ATTACHMENT_INTENT_CHANNEL_FILE;
    }
    return ATTACHMENT_INTENT_INVALID;
}

/**
 * @brief Returns the current policy rejection, while the caller holds the clients lock.
 */
static char* _client_msg_attachments_internal__policy_error(attachment_intent_t intent, uint64 length)
{
    uint64 maximum_length = MAX_CLIENT_FILE_UPLOAD_LENGTH;

    if (intent == ATTACHMENT_INTENT_INVALID)
    {
        return "invalid_upload_intent";
    }
    if ((intent == ATTACHMENT_INTENT_DIRECT_PICTURE || intent == ATTACHMENT_INTENT_DIRECT_FILE)
        && g_server_settings.is_private_messaging_allowed == FALSE)
    {
        return "private_messages_disabled";
    }
    if (intent == ATTACHMENT_INTENT_DIRECT_PICTURE || intent == ATTACHMENT_INTENT_CHANNEL_PICTURE)
    {
        if (g_server_settings.is_chat_picture_allowed == FALSE)
        {
            return "chat_pictures_disabled";
        }
        maximum_length = file_transfer__get_max_chat_picture_encrypted_length();
    }
    else if (intent == ATTACHMENT_INTENT_DIRECT_FILE || intent == ATTACHMENT_INTENT_CHANNEL_FILE)
    {
        if (g_server_settings.is_file_upload_allowed == FALSE)
        {
            return "file_uploads_disabled";
        }
        maximum_length = file_transfer__get_max_chat_file_encrypted_length();
    }
    if (length == 0 || length > maximum_length)
    {
        return "file_too_large";
    }
    return NULL_POINTER;
}

/**
 * @brief Releases the current upload and clears its routing and length state.
 * @attention The caller holds the clients write lock.
 */
static void _client_msg_attachments_internal__discard_upload(client_t* client)
{
    if (client->file_upload_extension.file_upload_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)client->file_upload_extension.file_upload_buffer);
    }
    clib__null_memory(&client->file_upload_extension, sizeof(client->file_upload_extension));
}

/**
 * @brief Receives a bounded file chunk after validating its numeric length and current upload policy.
 */
void client_msg__attachments__process_file_send_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* new_file = cJSON_GetObjectItemCaseSensitive(message, "is_new_file");
    cstring data = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "data_part_base64"));
    cstring intent_name = NULL_POINTER;
    char* policy_error = NULL_POINTER;
    client_t* sender = NULL_POINTER;
    attachment_intent_t intent = ATTACHMENT_INTENT_INVALID;
    uint64 total_length = 0;
    uint64 maximum_chunk_length = 0;
    int64 data_length = 0;

    status0 = cJSON_IsObject(message) == FALSE;
    if (status0 == FALSE)
    {
        status0 = cJSON_IsBool(new_file) == FALSE;
    }
    if (status0 == FALSE)
    {
        status0 = data == NULL_POINTER;
    }
    if (status0 == FALSE)
    {
        status0 = util__json_read_uint64(cJSON_GetObjectItemCaseSensitive(message, "total_bytes_length"), 9007199254740991ULL, &total_length) == FALSE;
    }
    if (status0 == FALSE)
    {
        status0 = total_length == 0;
    }
    if (status0 == TRUE)
    {
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (sender_client_id >= g_server_settings.max_client_count || base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto end;
    }
    sender = &g_clients_array[sender_client_id];
    if (sender->is_authenticated == FALSE || sender->is_existing == FALSE || sender->is_idle == TRUE)
    {
        goto end;
    }

    intent_name = cJSON_IsTrue(new_file) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "file_send_intent")) : sender->file_upload_extension.intent;
    intent = _client_msg_attachments_internal__read_intent(intent_name);
    policy_error = _client_msg_attachments_internal__policy_error(intent, total_length);
    if (policy_error != NULL_POINTER)
    {
        server_msg__attachments__send_file_send_error_to_single_client(sender, policy_error, 0);
        _client_msg_attachments_internal__discard_upload(sender);
        goto end;
    }
    status = permissions__can_upload(sender, intent_name);
    if (status == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
        server_msg__attachments__send_file_send_error_to_single_client(sender, "insufficient_permissions", 0);
        _client_msg_attachments_internal__discard_upload(sender);
        goto end;
    }

    maximum_chunk_length = file_transfer__get_max_file_upload_length() / 400 + 1;
    if (maximum_chunk_length < 16384)
    {
        maximum_chunk_length = 16384;
    }
    data_length = clib__utf8_string_length_check_max_length(data, maximum_chunk_length);
    if (data_length < 0 || (uint64)data_length > total_length)
    {
        _client_msg_attachments_internal__discard_upload(sender);
        goto end;
    }

    if (cJSON_IsTrue(new_file))
    {
        status = base__is_server_ram_budget_reached();
        if (status != FALSE)
        {
            server_msg__attachments__send_file_send_error_to_single_client(sender, "server_ram_budget_reached", 0);
            goto end;
        }
        _client_msg_attachments_internal__discard_upload(sender);
        sender->file_upload_extension.file_upload_buffer = (ubyte*)memorymanager__allocate(total_length + 1, MEMALLOC_FILE_UPLOAD_BY_PARTS);
        if (sender->file_upload_extension.file_upload_buffer == NULL_POINTER)
        {
            goto end;
        }
        clib__string_copy(sender->file_upload_extension.intent, sizeof(sender->file_upload_extension.intent), intent_name);
        sender->file_upload_extension.expected_file_length = total_length;
    }
    else if (sender->file_upload_extension.file_upload_buffer == NULL_POINTER
        || sender->file_upload_extension.expected_file_length != total_length)
    {
        _client_msg_attachments_internal__discard_upload(sender);
        goto end;
    }

    if (sender->file_upload_extension.buffer_cursor > total_length
        || (uint64)data_length > total_length - sender->file_upload_extension.buffer_cursor)
    {
        _client_msg_attachments_internal__discard_upload(sender);
        goto end;
    }
    clib__copy_memory(data, sender->file_upload_extension.file_upload_buffer + sender->file_upload_extension.buffer_cursor, data_length, data_length);
    sender->file_upload_extension.buffer_cursor += (uint64)data_length;
    if (sender->file_upload_extension.buffer_cursor == total_length)
    {
        server_msg__attachments__send_file_send_completed_status_to_single_client(sender);
    }

end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Validates every required completion field before routing, even when its feature is disabled.
 * @attention The caller holds the clients lock and owns the JSON throughout delivery.
 */
static boole _client_msg_attachments_internal__read_completion(cJSON* json_root, attachment_completion_t* completion)
{
    boole status0 = FALSE;
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* extra = cJSON_GetObjectItemCaseSensitive(message, "file_send_intent_extra_data");
    uint64 receiver_count = g_server_settings.max_client_count;
    int64 string_length = 0;

    if (cJSON_IsObject(message) == FALSE || cJSON_IsObject(extra) == FALSE)
    {
        return FALSE;
    }
    completion->intent_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "file_send_intent"));
    completion->intent = _client_msg_attachments_internal__read_intent(completion->intent_name);
    if (completion->intent == ATTACHMENT_INTENT_INVALID)
    {
        return FALSE;
    }
    if (completion->intent == ATTACHMENT_INTENT_MUSIC)
    {
        completion->song_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(extra, "song_name"));
        status0 = completion->song_name == NULL_POINTER;
        if (status0 == FALSE)
        {
            status0 = receiver_count == 0;
        }
        if (status0 == FALSE)
        {
            status0 = util__json_read_uint64(cJSON_GetObjectItemCaseSensitive(extra, "musicbot_id"), receiver_count - 1, &completion->receiver_id) == FALSE;
        }
        if (status0 == TRUE)
        {
            return FALSE;
        }
        string_length = clib__utf8_string_length_check_max_length(completion->song_name, SONG_NAME_MAX_LENGTH - 1);
        return string_length > 0;
    }
    if (completion->intent == ATTACHMENT_INTENT_CHANNEL_PICTURE || completion->intent == ATTACHMENT_INTENT_CHANNEL_FILE)
    {
        receiver_count = g_server_settings.max_channel_count;
    }
    status0 = receiver_count == 0;
    if (status0 == FALSE)
    {
        status0 = util__json_read_uint64(cJSON_GetObjectItemCaseSensitive(extra, "receiver_id"), receiver_count - 1, &completion->receiver_id) == FALSE;
    }
    if (status0 == FALSE)
    {
        status0 = util__json_read_uint64(cJSON_GetObjectItemCaseSensitive(extra, "local_message_id"), 9007199254740991ULL, &completion->local_message_id) == FALSE;
    }
    if (status0 == TRUE)
    {
        return FALSE;
    }
    if (completion->intent == ATTACHMENT_INTENT_DIRECT_FILE || completion->intent == ATTACHMENT_INTENT_CHANNEL_FILE)
    {
        completion->file_header = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(extra, "file_header"));
        if (completion->file_header == NULL_POINTER)
        {
            return FALSE;
        }
        string_length = clib__utf8_string_length_check_max_length(completion->file_header, FILE_HEADER_MAX_LENGTH);
        if (string_length <= 0)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief Routes a completed upload only after validating metadata and rechecking current policy.
 */
void client_msg__attachments__process_file_send_completed_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    attachment_completion_t completion = {0};
    client_t* sender = NULL_POINTER;
    client_t* music_bot = NULL_POINTER;
    musicbot_add_song_arg_struct_t* arguments = NULL_POINTER;
    char* policy_error = NULL_POINTER;

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (sender_client_id >= g_server_settings.max_client_count || base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto end;
    }
    sender = &g_clients_array[sender_client_id];
    if (sender->is_authenticated == FALSE || sender->is_existing == FALSE || sender->is_idle == TRUE)
    {
        goto end;
    }
    status = _client_msg_attachments_internal__read_completion(json_root, &completion);
    if (status == FALSE)
    {
        goto end;
    }
    status0 = sender->file_upload_extension.file_upload_buffer == NULL_POINTER;
    if (status0 == FALSE)
    {
        status1 = sender->file_upload_extension.expected_file_length == 0;
        status2 = sender->file_upload_extension.buffer_cursor != sender->file_upload_extension.expected_file_length;
        status0 = status1 == TRUE || status2 == TRUE;
    }
    if (status0 == TRUE)
    {
        goto end;
    }
    policy_error = _client_msg_attachments_internal__policy_error(completion.intent, sender->file_upload_extension.expected_file_length);
    if (policy_error != NULL_POINTER)
    {
        server_msg__attachments__send_file_send_error_to_single_client(sender, policy_error, completion.local_message_id);
        goto discard;
    }
    if (clib__strcmp(sender->file_upload_extension.intent, completion.intent_name) != 0
        || permissions__can_upload(sender, completion.intent_name) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
        server_msg__attachments__send_file_send_error_to_single_client(sender, "insufficient_permissions", completion.local_message_id);
        goto discard;
    }

    if (completion.intent == ATTACHMENT_INTENT_MUSIC)
    {
        music_bot = &g_clients_array[completion.receiver_id];
        if (music_bot->is_existing == FALSE || music_bot->is_music_bot == FALSE)
        {
            goto discard;
        }
        arguments = (musicbot_add_song_arg_struct_t*)memorymanager__allocate(sizeof(musicbot_add_song_arg_struct_t), MEMALLOC_MUSICBOT_SONG);
        if (arguments == NULL_POINTER)
        {
            goto discard;
        }
        arguments->music_bot = music_bot;
        arguments->mp3_data_buffer_length = sender->file_upload_extension.expected_file_length;
        arguments->mp3_data_buffer = sender->file_upload_extension.file_upload_buffer;
        sender->file_upload_extension.file_upload_buffer = NULL_POINTER;
        arguments->sender_client_id = sender_client_id;
        clib__memcpy(arguments->sender_identity, sender->public_key, sizeof(arguments->sender_identity));
        clib__string_copy(arguments->song_name, sizeof(arguments->song_name), completion.song_name);
        file_transfer__add_uploaded_song(arguments);
    }
    else if (completion.intent == ATTACHMENT_INTENT_DIRECT_PICTURE)
    {
        file_transfer__process_direct_chat_picture(sender_client_id, completion.receiver_id, completion.local_message_id, sender->file_upload_extension.file_upload_buffer);
    }
    else if (completion.intent == ATTACHMENT_INTENT_DIRECT_FILE)
    {
        file_transfer__process_direct_chat_file(sender_client_id, completion.receiver_id, completion.local_message_id, sender->file_upload_extension.file_upload_buffer, completion.file_header);
    }
    else
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        status0 = sender->channel_id < g_server_settings.max_channel_count;
        if (status0 == TRUE)
        {
            status0 = g_channel_array[sender->channel_id].is_existing == TRUE;
        }
        if (status0 == TRUE)
        {
            status0 = completion.intent != ATTACHMENT_INTENT_CHANNEL_PICTURE;
            if (status0 == FALSE)
            {
                status0 = permissions__can_use_channel(sender, &g_channel_array[sender->channel_id], PERMISSION_CHANNEL_PICTURE_POWER) == TRUE;
            }
        }
        if (status0 == TRUE)
        {
            if (completion.intent == ATTACHMENT_INTENT_CHANNEL_PICTURE)
            {
                file_transfer__process_channel_chat_picture(sender_client_id, completion.local_message_id, sender->file_upload_extension.file_upload_buffer);
            }
            else
            {
                file_transfer__process_channel_chat_file(sender_client_id, completion.local_message_id, sender->file_upload_extension.file_upload_buffer, completion.file_header);
            }
        }
        else
        {
            server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
            server_msg__attachments__send_file_send_error_to_single_client(sender, "insufficient_permissions", completion.local_message_id);
        }
        clib__unlock(&g_channels_global_rwlock_guard);
    }

discard:
    _client_msg_attachments_internal__discard_upload(sender);
end:
    clib__unlock(&g_clients_global_rwlock_guard);
}
