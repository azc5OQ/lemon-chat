#include "definitions.h"
#include "dh_primes.h"

#include "../third-party/dave-g-json/cJSON.h"
#include "client_message.h"

#include "clib/clib_string.h"
#include "base.h"
#include "memory_manager.h"
#include "clib/clib_memory.h"

#include "server_message.h"
#include "audio_channel.h"
#include "ip_tools.h"
#include "musicbot.h"

#include "../third-party/eteran-cvector/cvector.h"
#include "../third-party/libtom/libtommath/tommath.h"
#include "../third-party/zhicheng/base64.h"
#include "../third-party/rxi-log/log.h"

#include "util.h"

// static functions are defined first
static void _client_msg_internal__file_download_thread(data_for_file_send_thread_t* arg);

// declarations
static boole _client_msg_internal__is_add_tag_to_client_valid(cJSON* json_root);
static boole _client_msg_internal__is_set_alias_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_remove_tag_from_client_valid(cJSON* json_root);
static boole _client_msg_internal__is_process_server_settings_icon_upload_message_valid(cJSON* json_root);
static boole _client_msg_internal__is_process_server_settings_add_new_tag_message_valid(cJSON* json_root);
static boole _client_msg_internal__is_call_idle_client_message_valid(cJSON* json_root);
static boole _client_msg_internal__is_process_come_back_from_idle_mode_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_go_to_idle_mode_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_save_server_settings_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_kick_ban_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_client_msg__process_create_music_bot_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_client_msg__process_delete_music_bot_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_client_msg_file_send_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_musicbot_get_song_list_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_file_send_completed_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_remove_song_from_music_bot_request_valid(cJSON* json_root);
static boole _client_msg_internal__is_json_start_song_stream_message_valid(cJSON* json_root);
static boole _client_msg_internal__is_admin_password_message_valid(cJSON* json_root);
static boole _client_msg_internal__is_json_process_microphone_usage_valid(cJSON* json_root);
static boole _client_msg_internal__is_ice_candidate_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_json_delete_request_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_json_sdp_answer_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_json_join_channel_request_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_username_taken_by_another_client(char* username, uint64 client_id_to_skip);
static boole _client_msg_internal__is_json_poke_client_request_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_json_chat_message_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_json_edit_channel_request_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_json_create_channel_request_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_public_key_info_message_valid(cJSON* json_root, uint64 client_id);
static char* _client_msg_internal__get_challenge_string(cJSON* json_root);
static boole _client_msg_internal__is_public_key_challenge_response_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_internal__is_change_client_username_message_valid(cJSON* json_root, uint64 client_id);
static void _client_msg_internal__process_chat_message_action(cJSON* json_root, uint64 sender_client_id, char* outbound_action_type, boole is_edit);
static boole _client_msg_internal__is_offline_chat_message_valid(cJSON* json_root);
static boole _client_msg_internal__is_set_identity_alias_request_valid(cJSON* json_root);

/**
 * @brief validates an add-tag-to-client request: client id and tag id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_internal__is_add_tag_to_client_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_add_tag_to_client_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_add_tag_to_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_add_tag_to_client_valid cJSON_IsNumber(tag_id) \n");
        return FALSE;
    }

    if (json_tag_id->valueint < 0 || json_tag_id->valueint >= MAX_TAGS)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_add_tag_to_client_valid json_tag_id->valueint is not valid ", json_tag_id->valueint, "\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a remove-tag-from-client request: client id and tag id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_internal__is_remove_tag_from_client_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_remove_tag_from_client_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_remove_tag_from_client_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_remove_tag_from_client_valid cJSON_IsNumber(tag_id) \n");
        return FALSE;
    }

    if (json_tag_id->valueint < 0 || json_tag_id->valueint >= MAX_TAGS)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_remove_tag_from_client_valid json_tag_id->valueint is not valid ", json_tag_id->valueint, "\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a server-settings icon upload: the base64 icon string is present and within size limits
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_internal__is_process_server_settings_icon_upload_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* base64_icon_value = 0;
    uint64 base64_icon_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    base64_icon_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_icon_value");
    if (cJSON_IsString(base64_icon_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_icon_upload_message_valid cJSON_IsString(base64_icon_value)");
        return FALSE;
    }

    base64_icon_length = clib__utf8_string_length(base64_icon_value->valuestring);

    // don't accept icons too small or too large
    if (base64_icon_length >= ICON_MAX_LENGTH || base64_icon_length < 128)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length(base64_icon_value->valuestring) >= ICON_MAX_LENGTH");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates an add-new-tag request: the tag name and linked icon id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check, it doesnt access any of array structures array_icons or array_tags
 *
 * @return boole
 */
static boole _client_msg_internal__is_process_server_settings_add_new_tag_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_tag_name = 0;
    cJSON* json_linked_icon_id = 0;
    uint64 new_tag_name_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_tag_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_name");

    if (cJSON_IsString(json_tag_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_add_new_tag_message_valid cJSON_IsString(json_tag_name)");
        return FALSE;
    }

    new_tag_name_length = clib__utf8_string_length(json_tag_name->valuestring);

    if (new_tag_name_length >= TAG_MAX_NAME_LENGTH || new_tag_name_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_add_new_tag_message_valid (clib__utf8_string_length(json_tag_name->valuestring) >= TAG_MAX_NAME_LENGTH)");
        return FALSE;
    }

    // the linked icon is optional; a tag may be created without one. only validate it when present
    json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
    if (json_linked_icon_id != NULL_POINTER)
    {
        if (cJSON_IsNumber(json_linked_icon_id) == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_add_new_tag_message_valid cJSON_IsNumber(json_linked_icon_id)");
            return FALSE;
        }

        if (json_linked_icon_id->valueint < 0 || json_linked_icon_id->valueint >= MAX_ICONS)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "icon id is invalid");
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief validates a call-idle-client message: the target client id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_internal__is_call_idle_client_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_call_idle_client_message_valid cJSON_IsNumber(json_client_id)");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_client_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a come-back-from-idle-mode request: the target channel id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_internal__is_process_come_back_from_idle_mode_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_come_back_from_idle_mode_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a go-to-idle-mode request: there are no fields to check, so it always succeeds
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> always TRUE
 */
static boole _client_msg_internal__is_go_to_idle_mode_request_valid(cJSON* json_root)
{
    return TRUE;
}

/**
 * @brief validates a save-server-settings request: it carries no payload beyond message.type,
 *        which the upstream envelope/type check already verified
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> always TRUE
 */
static boole _client_msg_internal__is_save_server_settings_request_valid(cJSON* json_root)
{
    return TRUE;
}

/**
 * @brief validates a kick/ban request: the target client id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_internal__is_kick_ban_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_kick_ban_request_valid cJSON_IsNumber(json_client_id)");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_client_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a create-music-bot request: the channel id and bot username are present and valid
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_internal__is_client_msg__process_create_music_bot_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_music_bot_username = 0;
    uint64 json_music_bot_username_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (json_channel_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_client_msg__process_create_music_bot_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id is invalid");
        return FALSE;
    }

    json_music_bot_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "music_bot_username");

    if (json_music_bot_username == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_music_bot_username == NULL_POINTER \n");
        return FALSE;
    }

    if (json_music_bot_username->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_client_msg__process_create_music_bot_request_valid json_music_bot_username->valuestring == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsString(json_music_bot_username) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_client_msg__process_create_music_bot_request_valid cJSON_IsString(json_music_bot_username)");
        return FALSE;
    }

    json_music_bot_username_length = clib__utf8_string_length(json_music_bot_username->valuestring);

    if (json_music_bot_username_length == 0 || json_music_bot_username_length >= USERNAME_MAX_LENGTH)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a delete-music-bot request: the channel id is present and valid
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_internal__is_client_msg__process_delete_music_bot_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    if (json_channel_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_client_msg__process_delete_music_bot_request_valid json_channel_id == NULL_POINTER \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_client_msg__process_delete_music_bot_request_valid cJSON_IsNumber(json_channel_id)");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_channel_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a start-song-stream message: the song name is present, non-empty and within the length limit
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_start_song_stream_message_valid(cJSON* json_root)
{
    cJSON* json_song_name = 0;
    cJSON* json_message_object = 0;
    int64 song_name_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");
    if (cJSON_IsString(json_song_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_json_start_song_stream_message_valid cJSON_IsString(json_song_name) \n");
        return FALSE;
    }

    if (json_song_name->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_json_start_song_stream_message_valid json_song_name->valuestring == NULL_POINTER \n");
        return FALSE;
    }

    song_name_length = clib__utf8_string_length_check_max_length(json_song_name->valuestring, SONG_NAME_MAX_LENGTH - 1);
    if (song_name_length == -1 || song_name_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_json_start_song_stream_message_valid song_name_length == -1 || song_name_length == 0 \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates an admin-password message: the password value is present in the JSON
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
/**
 * @brief tells whether a username is currently worn by some OTHER connected client. usernames are
 *        the one name a person is shown under, so every path that writes one has to ask this first.
 *
 * @param char* username -> the name to look for (exact compare, like the rename path)
 * @param uint64 client_id_to_skip -> the client the name is meant for, skipped in the scan
 *
 * @attention the caller must already hold a lock on clients_global_rwlock_guard
 *
 * @return boole TRUE when another connected client already uses this username
 */
static boole _client_msg_internal__is_username_taken_by_another_client(char* username, uint64 client_id_to_skip)
{
    uint64 i = 0;

    if (username == NULL_POINTER || username[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_id_to_skip)
        {
            continue;
        }

        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].username, username) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static boole _client_msg_internal__is_admin_password_message_valid(cJSON* json_root)
{
    cJSON* json_admin_password = 0;
    cJSON* json_message_object = 0;
    int64 password_length = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_admin_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsString(json_admin_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_admin_password_message_valid cJSON_IsString(json_admin_password) \n");
        return FALSE;
    }

    if (json_admin_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_admin_password_message_valid json_admin_password->valuestring == NULL_POINTER \n");
        return FALSE;
    }

    password_length = clib__utf8_string_length_check_max_length(json_admin_password->valuestring, ADMIN_PASSWORD_MAX_LENGTH);
    if (password_length == -1 || password_length == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_admin_password_message_valid json_admin_password length is wrong \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a microphone-usage message: the usage value is present and within the 1..3 range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_process_microphone_usage_valid(cJSON* json_root)
{
    cJSON* json_value = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsNumber(json_value) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "_client_msg_internal__is_json_process_microphone_usage_valid cJSON_IsNumber(json_value_object) \n");
        return FALSE;
    }

    if (json_value->valueint < 1 || json_value->valueint > 3)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "_client_msg_internal__is_json_process_microphone_usage_valid json_value->valueint < 1 || json_value->valueint > 3 \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_internal__is_ice_candidate_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_candidate = 0;
    cJSON* json_value_object = 0;
    cJSON* json_sdpMid = 0;
    cJSON* json_sdpMLineIndex = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsObject(json_value_object) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_value_object) \n");
        return FALSE;
    }

    json_candidate = cJSON_GetObjectItemCaseSensitive(json_value_object, "candidate");
    if (cJSON_IsString(json_candidate) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_candidate) \n");
        return FALSE;
    }

    if (json_candidate->valuestring == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_candidate->valuestring == NULL \n");
        return FALSE;
    }

    // if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
    // {
    // DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s","client : ", client_id, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
    // return FALSE;
    // }
    json_sdpMid = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdpMid");
    if (cJSON_IsString(json_sdpMid) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdpMid) \n");
        return FALSE;
    }

    if (json_sdpMid->valuestring == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdpMid->valuestring == NULL \n");
        return FALSE;
    }

    // if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
    // {
    // DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s","client : ", client_id, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
    // return FALSE;
    // }
    json_sdpMLineIndex = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdpMLineIndex");
    if (cJSON_IsNumber(json_sdpMLineIndex) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_sdpMLineIndex) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_delete_request_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_channel_id = 0;
    cJSON* json_message_object = 0;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_channel_id) \n");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)))
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)) \n");
        return FALSE;
    }

    if (json_channel_id->valueint == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " root channel cannot be deleted \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_sdp_answer_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_sdp_message_value = 0;
    cJSON* json_value_object = 0;
    cJSON* json_sdp_message_type = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_value_object = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsObject(json_value_object) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_value_object) \n");
        return FALSE;
    }

    json_sdp_message_type = cJSON_GetObjectItemCaseSensitive(json_value_object, "type");
    if (cJSON_IsString(json_sdp_message_type) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdp_message_type) \n");
        return FALSE;
    }

    if (json_sdp_message_type->valuestring == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdp_message_type->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_sdp_message_type->valuestring) == 0)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_sdp_message_type->valuestring) \n");
        return FALSE;
    }

    json_sdp_message_value = cJSON_GetObjectItemCaseSensitive(json_value_object, "sdp");
    if (cJSON_IsString(json_sdp_message_value) == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_sdp_message_value) \n");
        return FALSE;
    }

    if (json_sdp_message_value->valuestring == NULL_POINTER)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " json_sdp_message_value->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_sdp_message_value->valuestring) == 0)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_sdp_message_value->valuestring) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_join_channel_request_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_channel_id = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_message_object = 0;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    if (cJSON_IsString(json_channel_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_channel_password) \n");
        return FALSE;
    }

    if (json_channel_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_password->valuestring == NULL \n");
        return FALSE;
    }

    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_channel_id) \n");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)))
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates a file-send request: total length, base64 data chunk and the is-new-file flag are present and within limits
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_internal__is_client_msg_file_send_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_message_total_length = 0;
    cJSON* json_message_data_part_base64 = 0;
    cJSON* json_message_is_new_file = 0;

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

    if (json_message_total_length->valueint <= 4096 || json_message_total_length->valueint >= MAX_CLIENT_FILE_UPLOAD_LENGTH)
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

    if (clib__utf8_string_length(json_message_data_part_base64->valuestring) > (MAX_CLIENT_FILE_UPLOAD_LENGTH / 400))
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
 * @brief validates a music-bot song-list request: the music bot id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_internal__is_musicbot_get_song_list_request_valid(cJSON* json_root)
{
    cJSON* json_musicbot_id = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (json_musicbot_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_musicbot_id == NULL_POINTER \n");
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

    return TRUE;
}

/**
 * @brief validates a file-send-completed request: the file-send intent and its matching extra-data fields are present and valid
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_internal__is_file_send_completed_request_valid(cJSON* json_root)
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

    if (clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_picture_file") == TRUE)
    {
        is_intent_allowed = TRUE;
    }

    if (clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_picture_file") == TRUE)
    {
        is_intent_allowed = TRUE;
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
        else if ((clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_picture_file") == TRUE) || clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_picture_file") == TRUE)
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
 * @brief validates a remove-song-from-music-bot request: the song id and music bot id are present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> TRUE when the request is valid, FALSE otherwise
 */
static boole _client_msg_internal__is_remove_song_from_music_bot_request_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_song_id = 0;
    cJSON* json_musicbot_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_id");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    if (json_song_id == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(json_song_id) \n");
        return FALSE;
    }

    if (cJSON_IsNumber(json_song_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "cJSON_IsNumber(json_song_id) \n");
        return FALSE;
    }

    if (json_song_id->valueint < 0 || json_song_id->valueint >= MUSIC_BOT_MAX_FILE_COUNT)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_song_id is invalid");
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

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 * @param char** out_message_type -> receives a pointer to the message type string parsed from the json
 *
 * @note this function is used for processing four different kinds of client messages, direct chat message, channel chat message, direct chat picture, channel chat picture
 *
 * @return boole
 */
boole client_msg__is_message_correct_at_first_sight_and_get_message_type(cJSON* json_root, uint64 client_id, char** out_message_type)
{
    cJSON* json_message_type = 0;
    cJSON* json_message_object = 0;
    int objectCount = 0;

    objectCount = cJSON_GetArraySize(json_root);

    if (objectCount != 1)
    {
        log_info("%s %llu %s", "client : ", client_id, " objectCount = cJSON_GetArraySize(json_root) is not 1");
        return FALSE;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    if (cJSON_IsObject(json_message_object) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_message_object) == false\n");
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
    if (cJSON_IsString(json_message_type) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_message_type) \n");
        return FALSE;
    }

    if (json_message_type->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_message_type->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_message_type->valuestring) \n");
        return FALSE;
    }

    *out_message_type = json_message_type->valuestring;
    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_poke_client_request_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_poke_message = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_message_object = 0;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_receiver_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_receiver_id) \n");
        return FALSE;
    }

    if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count \n");
        return FALSE;
    }

    json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
    if (cJSON_IsString(json_poke_message) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_poke_message) \n");
        return FALSE;
    }

    if (json_poke_message->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_poke_message->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_poke_message->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_poke_message->valuestring) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @note this function is used for processing four different kinds of client messages, direct chat message, channel chat message, direct chat picture, channel chat picture
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_chat_message_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_chat_message_value = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;
    cJSON* json_message_object = 0;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsString(json_chat_message_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_chat_message_value) \n");
        return FALSE;
    }

    if (json_chat_message_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_chat_message_value->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_chat_message_value->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_chat_message_value->valuestring) \n");
        return FALSE;
    }

    // reject an oversized value loudly. the server re-encrypts the forwarded message, and if its base64 output
    // would exceed the websocket frame limit, base__encrypt returns NULL and the send loop silently skips every
    // recipient while the sender still gets a delivery ack. base64 grows ~4/3; leave 2KB for the added sender
    // fields + envelope overhead. derived from the frame limit so it tracks the websocket_message_max_length setting.
    if (clib__utf8_string_length_check_max_length(json_chat_message_value->valuestring, (int)((g_server_settings.websocket_message_max_length * 3) / 4 - 2048)) == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " chat message value too large \n");
        return FALSE;
    }

    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
    if (cJSON_IsNumber(json_receiver_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_receiver_id) \n");
        return FALSE;
    }

    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");
    if (cJSON_IsNumber(json_local_message_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_local_message_id) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_edit_channel_request_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_channel_id = 0;
    cJSON* json_channel_name = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_message_object = 0;
    cJSON* json_is_audio_enabled = 0;

    int64 status = 0;

    // existence of "message" already checked before
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(channel_id) \n");
        return FALSE;
    }

    // cannot edit root channel
    if (json_channel_id->valueint == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(channel_id) \n");
        return FALSE;
    }

    if (json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)))
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_id->valueint < 0 || (json_channel_id->valueint >= (g_server_settings.max_channel_count - 1)) \n");
        return FALSE;
    }

    json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
    if (cJSON_IsString(json_channel_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_name) \n");
        return FALSE;
    }

    if (json_channel_name->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_name->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_channel_name->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(channel_name->valuestring) \n");
        return FALSE;
    }

    json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
    if (cJSON_IsString(json_channel_description) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_description) \n");
        return FALSE;
    }

    if (json_channel_description->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_description->valuestring == NULL \n");
        return FALSE;
    }

    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    if (cJSON_IsString(json_channel_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_channel_password) \n");
        return FALSE;
    }

    if (json_channel_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_password->valuestring == NULL \n");
        return FALSE;
    }

    json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
    if (cJSON_IsBool(json_is_audio_enabled) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsBool(json_is_audio_enabled) \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30); \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention
 *
 * @return boole
 */
static boole _client_msg_internal__is_json_create_channel_request_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_parent_channel_id = 0;
    cJSON* json_channel_name = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_is_audio_enabled = 0;
    cJSON* json_message_object = 0;

    int64 status = 0;

    // existence of "message" already checked before
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
    if (cJSON_IsNumber(json_parent_channel_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_parent_channel_id) \n");
        return FALSE;
    }

    json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
    if (cJSON_IsString(json_channel_name) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_name) \n");
        return FALSE;
    }

    if (json_channel_name->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_name->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_channel_name->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(channel_name->valuestring) \n");
        return FALSE;
    }

    json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
    if (cJSON_IsString(json_channel_description) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(channel_description) \n");
        return FALSE;
    }

    if (json_channel_description->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " channel_description->valuestring == NULL \n");
        return FALSE;
    }

    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
    if (cJSON_IsString(json_channel_password) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_channel_password) \n");
        return FALSE;
    }

    if (json_channel_password->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_channel_password->valuestring == NULL \n");
        return FALSE;
    }

    json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
    if (cJSON_IsBool(json_is_audio_enabled) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsBool(json_is_audio_enabled) \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_name->valuestring, 50); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_description->valuestring, CHANNEL_DESCRIPTION_MAX_LENGTH - 1); \n");
        return FALSE;
    }

    status = clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30);

    if (status == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "clib__utf8_string_length_check_max_length(json_channel_password->valuestring, 30); \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
static boole _client_msg_internal__is_public_key_info_message_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_message_type = 0;
    cJSON* json_message_value = 0;
    cJSON* verification_string = 0;
    cJSON* json_message_object = 0;

    boole status = FALSE;

    // "message" object is fetched from json_root, again
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        // no object is not present under key "message", not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    if (cJSON_IsObject(json_message_object) == FALSE)
    {
        // message key exists but it does not store object in it, not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsObject(json_message_object) == false\n");
        return FALSE;
    }

    // json_message_object exists, continue validating received json
    json_message_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "type");
    if (cJSON_IsString(json_message_type) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_message_type) \n");
        return FALSE;
    }

    if (json_message_type->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_message_type->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_message_type->valuestring) \n");
        return FALSE;
    }

    status = clib__is_string_equal(json_message_type->valuestring, "public_key_info");

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, "clib__is_string_equal(json_message_type->valuestring, \"public_key_info\") \n");
        return FALSE;
    }

    // type is verified, continue validating received json
    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    if (cJSON_IsString(json_message_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type != string \n");
        return FALSE;
    }

    if (json_message_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_value->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_message_value->valuestring) != 344)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_message_value->valuestring) != 344 \n");
        return FALSE;
    }

    // it's verified that json contains client's public key, continue json validation
    verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
    if (cJSON_IsString(verification_string) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type != string \n");
        return FALSE;
    }

    if (verification_string->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " verification_string->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(verification_string->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(verification_string->valuestring) \n");
        return FALSE;
    }

    // it's verified that json contains verification_string, continue json validation
    verification_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "dh_public_mix");
    if (cJSON_IsString(verification_string) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " dh_public_mix != string \n");
        return FALSE;
    }

    if (verification_string->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " dh_public_mix->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(verification_string->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(dh_public_mix->valuestring) \n");
        return FALSE;
    }

    // it's verified that json contains dh_public_mix, json appears to be valid
    return TRUE;
}

/**
 * @brief Helper function. gets challenge string from json.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return char* returns  the challenge string
 *
 * @attention this function assumes json_root is already verified , has correct data, if not this function will cause crash of entire server
 */
static char* _client_msg_internal__get_challenge_string(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* challenge_string = 0;
    char* result = 0;

    // "message" object is fetched from json_root, again
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    challenge_string = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    result = challenge_string->valuestring;

    return result;
}

/**
 * @brief validates a public key challenge response: the message object and its value string are present
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
static boole _client_msg_internal__is_public_key_challenge_response_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_message_value = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == 0)
    {
        // no object is not present under key "message", not valid
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_object is null\n");
        return FALSE;
    }

    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    if (cJSON_IsString(json_message_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_type != string \n");
        return FALSE;
    }

    if (json_message_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_message_value->valuestring == NULL \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function checks if clients username change request json message has all nessecary fields
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @attention this function is static, only used within client_message.c
 *
 * @return boole
 */
static boole _client_msg_internal__is_change_client_username_message_valid(cJSON* json_root, uint64 client_id)
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
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_change_client_username_message_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_change_client_username_message_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
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
 * @brief extracts the client's public key, verification string and dh public mix from the json into the provided output pointers
 *
 * @param cJSON* json_root -> the parsed client request
 * @param char** out_public_key -> receives a pointer to the client's public key string
 * @param char** out_verification_string -> receives a pointer to the verification string
 * @param char** out_dh_mix -> receives a pointer to the client's dh public mix string
 *
 * @return void
 *
 * @attention this function assumes json_root is already verified , has correct data, if not this function will cause crash of entire server
 */
void client_msg__get_public_key_and_verification_string_and_dh_public_mix(cJSON* json_root, char** out_public_key, char** out_verification_string, char** out_dh_mix)
{
    cJSON* public_key_json = 0;
    cJSON* verification_string_json = 0;
    cJSON* json_message_object = 0;
    cJSON* dh_public_mix_json = 0;

    // "message" object is fetched from json_root, again
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    public_key_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    verification_string_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "verification_string");
    dh_public_mix_json = cJSON_GetObjectItemCaseSensitive(json_message_object, "dh_public_mix");

    *out_verification_string = verification_string_json->valuestring;
    *out_public_key = public_key_json->valuestring;
    *out_dh_mix = dh_public_mix_json->valuestring;
}

// safe prime numbers for Diffie-Hellman key exchange , for modulus
// client.html has the same
// replace with your own if you want

// DH_MODULUS_STR is the safe prime for the size chosen by DH_MODULUS_BITS in dh_primes.h (2048, 4096, or 8192)
const char* g_dh_known_modulus_str = DH_MODULUS_STR;

/**
 * @brief processes the first message a new client sends, running the full diffie-hellman key
 *        exchange and issuing the rsa challenge
 *
 *        the function works in this order:
 *          1. validates the incoming message (spam check, json structure, verification string)
 *          2. rejects duplicate public keys (no two connected clients can share one)
 *          3. stores the client's rsa public key
 *          4. performs the diffie-hellman key exchange:
 *             - loads the shared prime modulus (must match the client's)
 *             - generates a random 256-bit server secret exponent
 *             - computes shared_secret = client_public_mix ^ server_exponent mod p
 *             - computes server_public_mix = g ^ server_exponent mod p
 *          5. generates a random challenge string and encrypts it with the client's rsa public key
 *          6. sends server_public_mix + the encrypted challenge to the client
 *          7. stores the plaintext challenge so it can be verified when the client responds
 *
 *        after this function the client must decrypt the challenge and send it back, which is
 *        handled by client_msg__process_public_key_challenge_response().
 *
 * @param cJSON* json_root -> parsed json message from the client
 * @param uint64 sender_client_id -> index into g_clients_array for this client
 *
 * @note takes the global clients write lock for everything after the initial validation, every
 *       early exit past that point goes through the goto label so the lock is released
 *
 * @return void
 */
void client_msg__process_public_key_info(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    mp_err mp_status = 0;
    size_t mp_written = 0;

    // data extracted from JSON
    char* public_key = 0;
    char* verification_string = 0;
    char* dh_received_public_mix_from_client = 0;

    // DH bignums
    mp_int bignum_modulus;
    mp_int bignum_server_exponent;
    mp_int bignum_client_public_mix;
    mp_int bignum_shared_secret;
    mp_int bignum_generator;
    mp_int bignum_server_public_mix;
    mp_int bignum_modulus_minus_one;

    // challenge + server's DH public mix as strings for transmission
    char* challenge_string = 0;
    char* challenge_value_for_client = 0;
    char* dh_public_mix_from_server_string_for_client = 0;

    char exponent_bits[DH_EXPONENT_BITS + 1];
    unsigned char exponent_random_bytes[DH_EXPONENT_BITS / 8];
    uint64 i = 0;

    // ──────────────────────────────────────────────────────────────────
    // STEP 1: Basic validation (before acquiring write lock)
    // ──────────────────────────────────────────────────────────────────
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: rejected by spam protection", sender_client_id);
        return;
    }

    status = _client_msg_internal__is_public_key_info_message_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: invalid message structure", sender_client_id);
        base__close_websocket_connection(sender_client_id, TRUE);
        return;
    }

    // ──────────────────────────────────────────────────────────────────
    // Acquire write lock for the rest of the function.
    // All early exits below use goto to ensure the lock is released.
    // ──────────────────────────────────────────────────────────────────
    clib__write_lock(&g_clients_global_rwlock_guard);

    // Initialize every DH bignum up front
    mp_status = mp_init_multi(&bignum_modulus, &bignum_server_exponent, &bignum_client_public_mix, &bignum_shared_secret, &bignum_generator, &bignum_server_public_mix, &bignum_modulus_minus_one, (mp_int*)NULL_POINTER);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to initialize DH bignums", sender_client_id);
        base__close_websocket_connection(sender_client_id, FALSE);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    if (g_clients_array[sender_client_id].is_existing == FALSE)
    {
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    DBG_AUTHENTICATION log_info("[auth] client %llu: message valid, proceeding with authentication", sender_client_id);

    // ──────────────────────────────────────────────────────────────────
    // STEP 2: Extract and verify fields from JSON
    // - verification_string must be "welcome" (protocol handshake)
    // - public_key must not already be in use by another client
    // ──────────────────────────────────────────────────────────────────
    public_key = 0;
    verification_string = 0;
    dh_received_public_mix_from_client = 0;

    client_msg__get_public_key_and_verification_string_and_dh_public_mix(json_root, &public_key, &verification_string, &dh_received_public_mix_from_client);

    if (clib__is_string_equal(verification_string, "welcome") == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: verification string is not 'welcome'", sender_client_id);
        base__close_websocket_connection(sender_client_id, TRUE);
        goto _label_client_msg__process_public_key_info_end;
    }

    if (base__is_there_a_client_with_same_public_key(public_key) == TRUE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: duplicate public key, rejecting", sender_client_id);
        base__close_websocket_connection(sender_client_id, TRUE);
        goto _label_client_msg__process_public_key_info_end;
    }

    // ──────────────────────────────────────────────────────────────────
    // STEP 3: Store client's RSA public key
    // ──────────────────────────────────────────────────────────────────
    DBG_AUTHENTICATION log_info("[auth] client %llu: storing public key", sender_client_id);
    clib__copy_memory(public_key, &g_clients_array[sender_client_id].public_key[0], clib__utf8_string_length(public_key), 1000);

    // ──────────────────────────────────────────────────────────────────
    // STEP 4: Diffie-Hellman key exchange
    //
    // Both client and server know:
    // p = dh_known_modulus_str  (shared prime, defined at top of file)
    // g = 2                    (generator)
    //
    // Client sent:   A = g^a mod p   (dh_received_public_mix_from_client)
    // Server picks:  b              (random 256-bit secret exponent)
    // Server computes:
    // shared_secret = A^b mod p   (same value as g^(a*b) mod p)
    // B = g^b mod p               (sent back to client)
    // Client will compute:
    // shared_secret = B^a mod p   (same value as g^(a*b) mod p)
    // ──────────────────────────────────────────────────────────────────

    // 4a. load the shared prime modulus
    mp_status = mp_read_radix(&bignum_modulus, g_dh_known_modulus_str, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to parse DH modulus", sender_client_id);
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 4b. generate the server's secret exponent (DH_EXPONENT_BITS random bits, sized to the modulus)
    // first bit forced to 1 to guarantee the full length, the rest from a cryptographically secure
    // RNG. rand() must never be used here: it is a predictable PRNG seeded from the clock, so an
    // exponent built from it could be reconstructed by an attacker, breaking the key exchange.
    {
        if (base__fill_secure_random_bytes(exponent_random_bytes, sizeof(exponent_random_bytes)) == FALSE)
        {
            DBG_AUTHENTICATION log_info("[auth] client %llu: failed to generate DH exponent randomness", sender_client_id);
            base__close_websocket_connection(sender_client_id, FALSE);
            goto _label_client_msg__process_public_key_info_end;
        }

        clib__null_memory(exponent_bits, sizeof(exponent_bits));

        exponent_bits[0] = '1';
        for (i = 1; i < DH_EXPONENT_BITS; i++)
        {
            exponent_bits[i] = (((exponent_random_bytes[i / 8] >> (7 - (i % 8))) & 1) == 0) ? '0' : '1';
        }
        exponent_bits[DH_EXPONENT_BITS] = '\0';

        mp_status = mp_read_radix(&bignum_server_exponent, exponent_bits, 2);
        if (mp_status != MP_OKAY)
        {
            DBG_AUTHENTICATION log_info("[auth] client %llu: failed to create DH exponent", sender_client_id);
            base__close_websocket_connection(sender_client_id, FALSE);
            goto _label_client_msg__process_public_key_info_end;
        }
    }
    DBG_AUTHENTICATION log_info("[auth] client %llu: generated DH exponent", sender_client_id);

    // 4c. parse client's public mix: A = g^a mod p (received as decimal string)
    mp_status = mp_read_radix(&bignum_client_public_mix, (char*)dh_received_public_mix_from_client, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to parse client's DH public mix", sender_client_id);
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 4c-validate: reject a degenerate client public mix before exponentiating. A in {0, 1, p-1} (or outside
    // [2, p-2]) forces a known / tiny shared secret; for a safe prime the only dangerous small-order elements
    // are 1 and p-1, so requiring 2 <= A <= p-2 is sufficient.
    mp_status = mp_sub_d(&bignum_modulus, 1uL, &bignum_modulus_minus_one);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to compute p-1 for DH validation", sender_client_id);
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    if (mp_cmp_d(&bignum_client_public_mix, 1uL) != MP_GT || mp_cmp(&bignum_client_public_mix, &bignum_modulus_minus_one) != MP_LT)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: rejected degenerate DH public mix (not in [2, p-2])", sender_client_id);
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 4d. compute shared_secret = A^b mod p
    mp_status = mp_exptmod(&bignum_client_public_mix, &bignum_server_exponent, &bignum_modulus, &bignum_shared_secret);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: mp_exptmod failed for shared secret", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 4e. store shared secret as decimal string in client slot
    mp_status = mp_to_radix(&bignum_shared_secret, g_clients_array[sender_client_id].dh_shared_secret, SHARED_SECRET_LENGTH, &mp_written, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to convert shared secret to string", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    g_clients_array[sender_client_id].is_dh_shared_secret_agreed_upon = TRUE;
    DBG_AUTHENTICATION log_info("[auth] client %llu: shared secret computed and stored", sender_client_id);

    // 4f. compute server's public mix: B = g^b mod p.
    // bignum_generator was initialized to 0 above; set it to the generator g = 2.
    mp_set_i64(&bignum_generator, 2);
    mp_status = mp_exptmod(&bignum_generator, &bignum_server_exponent, &bignum_modulus, &bignum_server_public_mix);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: mp_exptmod failed for server public mix", sender_client_id);
        goto _label_client_msg__process_public_key_info_end;
    }

    // 4g. convert B to decimal string for JSON transmission
    dh_public_mix_from_server_string_for_client = (char*)memorymanager__allocate(SHARED_SECRET_LENGTH, MEMALLOC_DHPROCESS);
    mp_status = mp_to_radix(&bignum_server_public_mix, dh_public_mix_from_server_string_for_client, SHARED_SECRET_LENGTH, &mp_written, 10);
    if (mp_status != MP_OKAY)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to convert server public mix to string", sender_client_id);
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    DBG_AUTHENTICATION log_info("[auth] client %llu: server public mix computed", sender_client_id);

    // ──────────────────────────────────────────────────────────────────
    // STEP 5: RSA challenge - prove the client owns the public key
    //
    // Generate a random string, encrypt it with client's RSA public key,
    // and send it along with the server's DH public mix.
    // Only the real owner of the private key can decrypt the challenge.
    // ──────────────────────────────────────────────────────────────────
    challenge_string = (char*)memorymanager__allocate(128, MEMALLOC_TYPE_CHALLENGE);
    if (base__fill_block_of_data_with_ascii_characters(challenge_string, CHALLENGE_STRING_SIZE) == FALSE)
    {
        DBG_AUTHENTICATION log_info("[auth] client %llu: failed to generate RSA challenge randomness", sender_client_id);
        memorymanager__free((nuint)challenge_string);
        memorymanager__free((nuint)dh_public_mix_from_server_string_for_client);
        base__close_websocket_connection(sender_client_id, FALSE);
        goto _label_client_msg__process_public_key_info_end;
    }

    DBG_AUTHENTICATION log_info("[auth] client %llu: generated %d-byte challenge", sender_client_id, CHALLENGE_STRING_SIZE);

    // encrypt challenge with client's RSA public key
    challenge_value_for_client = base__encrypt_string_with_public_key(public_key, (unsigned char*)challenge_string, (uint64)clib__utf8_string_length(challenge_string));

    // send server's DH public mix + encrypted challenge to client
    server_msg__send_public_key_challenge_to_single_client(g_clients_array[sender_client_id].p_ws_connection, challenge_value_for_client, dh_public_mix_from_server_string_for_client);

    memorymanager__free((nuint)challenge_value_for_client);

    // ──────────────────────────────────────────────────────────────────
    // STEP 6: Store plaintext challenge for verification when client responds
    // ──────────────────────────────────────────────────────────────────
    clib__copy_memory(challenge_string, &g_clients_array[sender_client_id].challenge_string[0], CHALLENGE_STRING_SIZE, 128);
    g_clients_array[sender_client_id].is_public_key_challenge_sent = TRUE;

    DBG_AUTHENTICATION log_info("[auth] client %llu: challenge stored, waiting for response", sender_client_id);

    memorymanager__free((nuint)challenge_string);
    memorymanager__free((nuint)dh_public_mix_from_server_string_for_client);

_label_client_msg__process_public_key_info_end:
    // Release every DH bignum. All goto paths land here only after the
    // mp_init_multi above succeeded, so all six are guaranteed initialized.
    mp_clear_multi(&bignum_modulus, &bignum_server_exponent, &bignum_client_public_mix, &bignum_shared_secret, &bignum_generator, &bignum_server_public_mix, &bignum_modulus_minus_one, (mp_int*)NULL_POINTER);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes public key challenge response from client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_public_key_challenge_response(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    char* received_challenge_string = 0;
    uint64 client_count_in_root_channel = 0;
    channel_t* root_channel = 0;
    client_t* current_client = NULL_POINTER;

    // status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    // if (status == FALSE)
    // {
    // DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_public_key_challenge_response base__is_request_allowed_based_on_spam_protection == FALSE \n");
    // return;
    // }
    status = _client_msg_internal__is_public_key_challenge_response_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        base__close_websocket_connection(sender_client_id, TRUE);
        DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__process_public_key_challenge_response deleting client because challenge response not valid: client index ", sender_client_id, " \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // client sends public key to server at the time of authentication
    // server generates random string, encrypts that string with the client's public key
    // server then verifies if the client really is the owner of public key by sending client a little challenge
    // "if the public key is really yours, client, please, decrypt and then send back this randomly generated string that I will send you. you will have no problem telling me what I sent you, if it's really your key"
    // something like that
    current_client = &g_clients_array[sender_client_id];
    if (current_client->is_public_key_challenge_sent == FALSE)
    {
        base__close_websocket_connection(sender_client_id, FALSE);
        DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__process_public_key_challenge_response deleting client because !current_client->is_public_key_challenge_sent : client index ", sender_client_id, " \n");
        goto _label_client_msg__process_public_key_challenge_response_end;
    }

    // compare the key
    received_challenge_string = _client_msg_internal__get_challenge_string(json_root);

    status = clib__is_string_equal(g_clients_array[sender_client_id].challenge_string, received_challenge_string);

    if (status == TRUE)
    {
        DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__process_public_key_challenge_response challenge response string match : client index ", sender_client_id, " \n");

        current_client->channel_id = 0;
        current_client->has_pending_maintainer_reset_vote = FALSE; // fresh session - no vote can be pending
        current_client->is_admin = FALSE;
        current_client->is_authenticated = TRUE;
        current_client->timestamp_last_maintain_connection_message_received = base__get_timestamp_ms();

        if (g_server_settings.is_display_country_flags_active == TRUE)
        {
            ip_tools_load_iso_country_code(current_client->ip_address, current_client->country_iso_code);
        }

        status = base__assign_username_for_newly_joined_client(sender_client_id, g_server_settings.default_client_name);

        if (status == FALSE)
        {
            base__close_websocket_connection(sender_client_id, FALSE);
            DBG_AUTHENTICATION log_info("%s %llu %s", "client_msg__process_public_key_challenge_response deleting client base__assign_username_for_newly_joined_client returned false : client index ", sender_client_id, " \n");
            goto _label_client_msg__process_public_key_challenge_response_end;
        }

        // it's better when readlock is placed here instead of it being placed directly in server_msg__send_channel_list_to_single_client function
        clib__write_lock(&g_channels_global_rwlock_guard);
        clib__read_lock(&g_tags_global_rwlock_guard);
        clib__read_lock(&g_icons_global_rwlock_guard);

        status = util__is_client_valid(current_client->client_id);

        if (status == TRUE)
        {
            // restore this client's saved tags from the identity store (matched by public-key hash); the
            // admin tag re-grants admin. runs before the connect broadcast so every client sees the tags.
            // the tags read lock (held above) and clients write lock (held for this handler) cover it
            if (g_server_settings.are_identities_enabled == TRUE)
            {
                DBG_IDENTITIES log_info("%s %llu %s", "identities: enabled -> restoring tags for authenticated client_id", current_client->client_id, "\n");
                base__restore_identity_tags(current_client);
            }
            else
            {
                DBG_IDENTITIES log_info("%s", "identities: DISABLED on this server (are_identities_enabled=false) -> no tags will be restored on connect \n");
            }

            // restore this identity's persisted avatar into the live client so others can load it. gated on
            // allow_avatars only (avatars may be enabled without tag-identities)
            if (g_server_settings.allow_avatars == TRUE)
            {
                base__restore_identity_avatar(current_client);
            }

            // restore the admin-registered alias (display name) for this identity
            if (g_server_settings.are_identities_enabled == TRUE && g_server_settings.allow_alias_registrations == TRUE)
            {
                base__restore_identity_alias(current_client);
            }

            // an identity the admin gave a registered name to is a REGISTERED user of this server. only
            // those may list the stored clients - otherwise any guest could join and harvest everyone's
            // name and avatar. avatars are self-service so they prove nothing; a registered name is
            // admin-granted
            current_client->is_registered = (boole)(current_client->alias[0] != 0);

            // the registered name IS the username: pin it over whatever this session connected with,
            // so a registered person always appears under their one admin-set name (and cannot keep an
            // old self-chosen username after being registered)
            // registered names are reserved (the rename path refuses them and registration refuses a
            // name in use), so this is normally free - but never overwrite into a duplicate if some
            // older state left the name occupied: he keeps the deduped name assigned on join instead
            if (current_client->is_registered == TRUE && _client_msg_internal__is_username_taken_by_another_client(current_client->alias, current_client->client_id) == FALSE)
            {
                clib__null_memory(&current_client->username[0], USERNAME_MAX_LENGTH);
                clib__copy_memory(current_client->alias, &current_client->username[0], clib__utf8_string_length(current_client->alias), USERNAME_MAX_LENGTH - 1);
            }

            // offline messages need peers to be able to encrypt to this identity while it is away,
            // which means the server must keep its RAW public key (only ever while the feature is on,
            // and only for REGISTERED identities - unregistered ones cannot be messaged offline)
            if (g_server_settings.allow_offline_messages == TRUE && current_client->is_registered == TRUE && current_client->public_key[0] != 0 && current_client->is_music_bot == FALSE)
            {
                char offline_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
                base__hash_password_to_base64(current_client->public_key, offline_identity_hash, sizeof(offline_identity_hash));
                base__store_identity_raw_public_key(offline_identity_hash, current_client->public_key);
            }

            server_msg__send_authentication_status_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
            server_msg__send_channel_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
            server_msg__send_client_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret, current_client->username, current_client->client_id);
            server_msg__send_icon_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
            server_msg__send_tag_list_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret);
            server_msg__send_active_microphone_usage_for_current_channel_to_single_client(current_client->p_ws_connection, current_client->dh_shared_secret, current_client->channel_id);
            server_msg__send_client_connect_message_to_all_clients(current_client->client_id);

            // hand over anything that was said to this identity while it was away, then forget it.
            // after the client list, so the receiving client already knows who everybody is
            if (g_server_settings.allow_offline_messages == TRUE && current_client->is_registered == TRUE && current_client->public_key[0] != 0)
            {
                server_msg__send_queued_offline_messages_to_single_client(current_client);
            }

            client_count_in_root_channel = base__get_client_count_for_channel(ROOT_CHANNEL_ID);

            root_channel = &g_channel_array[ROOT_CHANNEL_ID];

            if (client_count_in_root_channel == 1)
            {
                root_channel->maintainer_id = current_client->client_id;
                root_channel->is_channel_maintainer_present = TRUE;
                root_channel->maintainer_generation++;
                server_msg__send_maintainer_id_to_single_client(current_client, ROOT_CHANNEL_ID, current_client->client_id);
            }
            else
            {
                server_msg__send_maintainer_id_to_single_client(current_client, ROOT_CHANNEL_ID, root_channel->maintainer_id);
            }

            clib__unlock(&g_icons_global_rwlock_guard);
            clib__unlock(&g_tags_global_rwlock_guard);
            clib__unlock(&g_channels_global_rwlock_guard);
        }
    }
    else
    {
        DBG_AUTHENTICATION log_info("%s", " challenge string not equal \n");
        base__close_websocket_connection(sender_client_id, FALSE);
    }

_label_client_msg__process_public_key_challenge_response_end:
    clib__unlock(&g_clients_global_rwlock_guard);
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
void client_msg__process_client_connection_check(cJSON* json_root, uint64 sender_client_id)
{
    clib__write_lock(&g_clients_global_rwlock_guard);

    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        g_clients_array[sender_client_id].timestamp_last_maintain_connection_message_received = base__get_timestamp_ms();
        server_msg__send_connection_check_response_to_single_client(&g_clients_array[sender_client_id]);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
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
void client_msg__process_change_client_username(cJSON* json_root, uint64 sender_client_id)
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
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_client_username base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_change_client_username_message_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_client_username client_msg__is_change_client_username_message_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_username");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client has permission to change the username
    client_to_alter = &g_clients_array[json_client_id->valueint];

    // client that is requesting change for own username, allow it - UNLESS he is registered.
    // a registered user's name is admin-controlled (set through the register-username action), so
    // he cannot rename himself out of it; an admin still can, via that action
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
    // while that user is offline, and the moment he came back there would be two of him. refusing
    // the registration of a name in use (see the register handler) only holds if this holds too
    if (is_username_taken == FALSE && client_to_alter->public_key[0] != 0)
    {
        char requester_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
        base__hash_password_to_base64(client_to_alter->public_key, requester_identity_hash, sizeof(requester_identity_hash));

        if (base__is_alias_taken_by_another_identity(json_message_value->valuestring, requester_identity_hash) == TRUE)
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
        DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__send_client_rename_message_to_all_clients \n");

        clib__read_lock(&g_clients_global_rwlock_guard);
        server_msg__send_client_rename_message_to_all_clients(client_to_alter->client_id, client_to_alter->username);
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief This function processes create channel request message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @attention this function aquires read and write locks for channel_array and clients_array
 *
 * @return void
 */
void client_msg__process_create_channel_request(cJSON* json_root, uint64 sender_client_id)
{
    boole is_channel_create_allowed = FALSE;
    boole is_parent_channel_id_existing = FALSE;
    boole creating_temp_channel = FALSE;
    boole parent_is_temp_channel = FALSE;
    cJSON* json_parent_channel_id = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_channel_name = 0;
    cJSON* json_is_audio_enabled = 0;
    cJSON* json_is_client_limit_active = 0;
    cJSON* json_max_client_count = 0;
    boole is_password_used = FALSE;
    boole is_channel_created_successfully = FALSE;
    cJSON* json_message_object = 0;
    uint64 i = 0;
    uint64 created_channel_index = 0;
    uint64 channel_creator_client_id = 0;
    channel_t* channel = 0;
    boole status = FALSE;

    // check timestamp first
    // check if json is valid
    // check access rights
    // check if parent channel id is valid, if parent channel is is_existing
    // finally, create channel
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_json_create_channel_request_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request client_msg__is_json_create_channel_request_valid == FALSE \n");
        return;
    }

    // decide whether this is a normal channel or a guest temp channel:
    // - an admin always creates a normal channel
    // - a non-admin creates a temp channel, but only when temp channels are enabled server-wide and the
    // non-admin does not already own one (a temp channel is destroyed when its creator leaves it - see
    // the join / disconnect / delete paths)
    // - otherwise the request is refused: creating a normal channel is admin-only
    is_channel_create_allowed = FALSE;
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
    {
        if (g_clients_array[sender_client_id].is_admin == TRUE)
        {
            is_channel_create_allowed = TRUE;
        }
        else if (g_server_settings.is_temp_channel_creation_allowed == TRUE
            && g_clients_array[sender_client_id].is_temp_admin_channel == FALSE)
        {
            is_channel_create_allowed = TRUE;
            creating_temp_channel = TRUE;
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_channel_create_allowed == TRUE)
    {
        // what is done here:
        // checking if parent channel exists,
        // creating the child channel
        // both need to be wrapper within same write lock because they are tied to one another,
        clib__write_lock(&g_channels_global_rwlock_guard);
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_parent_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "parent_channel_id");
        json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
        json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
        json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
        json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
        json_is_client_limit_active = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_client_limit_active");
        json_max_client_count = cJSON_GetObjectItemCaseSensitive(json_message_object, "max_client_count");

        for (i = 0; i < g_server_settings.max_channel_count; i++)
        {
            if (g_channel_array[i].is_existing == FALSE)
            {
                continue;
            }

            if (g_channel_array[i].channel_id == (int64)json_parent_channel_id->valuedouble)
            {
                is_parent_channel_id_existing = TRUE;
                parent_is_temp_channel = g_channel_array[i].is_temp_channel;
                break;
            }
        }

        if (is_parent_channel_id_existing == TRUE && parent_is_temp_channel == TRUE)
        {
            // no channel (not even an admin's) may be created under a temp channel
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request parent is a temp channel, refusing to create a child under it \n");
        }
        else if (is_parent_channel_id_existing == TRUE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request is_parent_channel_id_existing == TRUE \n");

            for (i = 0; i < g_server_settings.max_channel_count; i++)
            {
                channel = &g_channel_array[i];

                if (channel->is_existing == FALSE)
                {
                    // everything is in order, now create the channel
                    // set is_channel_created_successfully to TRUE so message about channel creation will be sent to all clients
                    // will be initiated later in code
                    channel->is_existing = TRUE;
                    channel->channel_id = i; // a channel's id is its index in the channel array
                    channel->parent_channel_id = (int64)json_parent_channel_id->valuedouble;
                    channel->is_root_channel = FALSE;
                    clib__copy_memory(json_channel_name->valuestring, channel->name, clib__utf8_string_length(json_channel_name->valuestring), CHANNEL_NAME_MAX_LENGTH);
                    clib__null_memory(channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                    if (clib__utf8_string_length(json_channel_password->valuestring) > 0)
                    {
                        base__hash_password_to_base64(json_channel_password->valuestring, channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
                    }
                    clib__copy_memory(json_channel_description->valuestring, channel->description, clib__utf8_string_length(json_channel_description->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH);
                    channel->maintainer_id = -1;
                    channel->is_channel_maintainer_present = FALSE;
                    channel->maintainer_generation++;
                    is_password_used = (boole)(clib__utf8_string_length(json_channel_password->valuestring) > 0);
                    channel->is_using_password = is_password_used;
                    channel->is_audio_enabled = (boole)cJSON_IsTrue(json_is_audio_enabled);
                    channel->is_temp_channel = creating_temp_channel;
                    channel->is_client_limit_active = (boole)cJSON_IsTrue(json_is_client_limit_active);
                    channel->max_client_count = 0;
                    // clamp the client-supplied capacity before the double->uint64 cast, which is undefined
                    // for negative / NaN / huge values. NaN fails both comparisons, so the range check
                    // rejects it too. a channel can never hold more clients than the server allows
                    if (cJSON_IsNumber(json_max_client_count)
                        && json_max_client_count->valuedouble >= 0
                        && json_max_client_count->valuedouble <= (double)g_server_settings.max_client_count)
                    {
                        channel->max_client_count = (uint64)json_max_client_count->valuedouble;
                    }

                    DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_create_channel_request is_parent_channel_id_existing ", i, "\n");
                    is_channel_created_successfully = TRUE;
                    created_channel_index = i;
                    channel_creator_client_id = sender_client_id;

                    break;
                }
            }
        }
        else
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_channel_request is_parent_channel_id_existing == FALSE \n");
        }
        clib__unlock(&g_channels_global_rwlock_guard);

        // okay, channel created successfully, acquire read lock for channels and clients
        if (is_channel_created_successfully == TRUE)
        {
            // announce the new channel to everyone first, so it exists in every client's UI before the
            // temp creator is moved into it below
            clib__read_lock(&g_clients_global_rwlock_guard);
            clib__read_lock(&g_channels_global_rwlock_guard);

            server_msg__send_channel_create_message_to_all_clients(created_channel_index, channel_creator_client_id);

            clib__unlock(&g_channels_global_rwlock_guard);
            clib__unlock(&g_clients_global_rwlock_guard);

            // a temp channel: mark the creator as its admin/owner (so a second temp channel is refused, and
            // so the join / disconnect / delete paths know whose departure destroys it) and move them into
            // it right away, the same server-side way the delete path moves clients to root
            if (creating_temp_channel == TRUE)
            {
                clib__write_lock(&g_clients_global_rwlock_guard);
                clib__write_lock(&g_channels_global_rwlock_guard);

                g_clients_array[channel_creator_client_id].is_temp_admin_channel = TRUE;
                g_clients_array[channel_creator_client_id].temp_channel_id = created_channel_index;
                base__move_client_into_channel(channel_creator_client_id, created_channel_index);

                clib__unlock(&g_channels_global_rwlock_guard);
                clib__unlock(&g_clients_global_rwlock_guard);
            }
        }
    }
    else
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__send_access_denied_to_single_client(&g_clients_array[sender_client_id]);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief This function processes edit channel request message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @attention this function aquires read and write locks for channel_array and clients_array
 *
 * @return void
 */
void client_msg__process_edit_channel_request(cJSON* json_root, uint64 sender_client_id)
{
    boole is_channel_edit_allowed = FALSE;
    cJSON* json_channel_id = 0;
    cJSON* json_channel_description = 0;
    cJSON* json_channel_password = 0;
    cJSON* json_channel_name = 0;
    boole is_password_used = FALSE;
    boole is_channel_edited_successfully = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_is_audio_enabled = 0;
    cJSON* json_is_client_limit_active = 0;
    cJSON* json_max_client_count = 0;
    uint64 channel_index_to_edit = 0;
    channel_t* channel = 0;
    boole status = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_json_edit_channel_request_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request client_msg__is_json_edit_channel_request_valid == FALSE \n");
        return;
    }

    // editing a channel is admin-only, except a temp admin may edit (rename / set password / toggle audio)
    // their own temp channel without admin rights
    is_channel_edit_allowed = FALSE;
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
    {
        if (g_clients_array[sender_client_id].is_admin == TRUE)
        {
            is_channel_edit_allowed = TRUE;
        }
        else if (g_clients_array[sender_client_id].is_temp_admin_channel == TRUE
            && g_clients_array[sender_client_id].temp_channel_id == (uint64)json_channel_id->valueint)
        {
            is_channel_edit_allowed = TRUE;
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_channel_edit_allowed == TRUE)
    {
        clib__write_lock(&g_channels_global_rwlock_guard);
        json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
        json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
        json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");
        json_channel_description = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_description");
        json_channel_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_name");
        json_is_audio_enabled = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_audio_enabled");
        json_is_client_limit_active = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_client_limit_active");
        json_max_client_count = cJSON_GetObjectItemCaseSensitive(json_message_object, "max_client_count");

        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request got here \n");

        channel_index_to_edit = json_channel_id->valueint;

        channel = &g_channel_array[channel_index_to_edit];

        if (channel->is_existing == TRUE)
        {
            clib__null_memory(channel->name, CHANNEL_NAME_MAX_LENGTH);
            clib__null_memory(channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
            clib__null_memory(channel->description, CHANNEL_DESCRIPTION_MAX_LENGTH);

            clib__copy_memory(json_channel_name->valuestring, channel->name, clib__utf8_string_length(json_channel_name->valuestring), CHANNEL_NAME_MAX_LENGTH);
            if (clib__utf8_string_length(json_channel_password->valuestring) > 0)
            {
                base__hash_password_to_base64(json_channel_password->valuestring, channel->password, CHANNEL_PASSWORD_MAX_LENGTH);
            }
            clib__copy_memory(json_channel_description->valuestring, channel->description, clib__utf8_string_length(json_channel_description->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH);

            is_password_used = (boole)(clib__utf8_string_length(json_channel_password->valuestring) > 0);
            channel->is_using_password = is_password_used;
            channel->is_audio_enabled = (boole)cJSON_IsTrue(json_is_audio_enabled);
            channel->is_client_limit_active = (boole)cJSON_IsTrue(json_is_client_limit_active);
            channel->max_client_count = 0;
            // clamp the client-supplied capacity before the double->uint64 cast, which is undefined
            // for negative / NaN / huge values. NaN fails both comparisons, so the range check
            // rejects it too. a channel can never hold more clients than the server allows
            if (cJSON_IsNumber(json_max_client_count)
                && json_max_client_count->valuedouble >= 0
                && json_max_client_count->valuedouble <= (double)g_server_settings.max_client_count)
            {
                channel->max_client_count = (uint64)json_max_client_count->valuedouble;
            }

            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_edit_channel_request channel is_existing TRUE \n");
            is_channel_edited_successfully = TRUE;
        }

        clib__unlock(&g_channels_global_rwlock_guard);

        // channel edited successfully, acquire read lock for channels and clients
        if (is_channel_edited_successfully == TRUE)
        {
            clib__read_lock(&g_clients_global_rwlock_guard);
            clib__read_lock(&g_channels_global_rwlock_guard);

            server_msg__send_channel_edit_message_to_all_clients(channel_index_to_edit, sender_client_id);

            clib__unlock(&g_channels_global_rwlock_guard);
            clib__unlock(&g_clients_global_rwlock_guard);
        }
    }
    else
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__send_access_denied_to_single_client(&g_clients_array[sender_client_id]);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief This function processes direct chat message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_direct_chat_message(cJSON* json_root, uint64 sender_client_id)
{
    boole is_message_valid = FALSE;
    boole is_receiver_existing = FALSE;
    boole is_receiver_idle = FALSE;
    boole is_receiver_music_bot = FALSE;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;
    cJSON* json_chat_message_value = 0;
    cJSON* json_message_object = 0;

    uint64 server_chat_message_id = 0;

    // because maintainer of channel sends out channel keys to each client individually,
    // and a maintainer needs to do that as quickly as possible, repeatedly, possibly for hundred clients in everyone is in his channel
    // spam prevention cannot be placed here like in other places, needs to be improved
    is_message_valid = _client_msg_internal__is_json_chat_message_format_valid(json_root, sender_client_id);

    if (is_message_valid == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
        return;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_message got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

    if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_direct_chat_message client : ", sender_client_id, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count \n");
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // if sender has idle mode active, stop it
    is_receiver_existing = g_clients_array[json_receiver_id->valueint].is_authenticated;
    is_receiver_idle = g_clients_array[json_receiver_id->valueint].is_idle;
    is_receiver_music_bot = g_clients_array[json_receiver_id->valueint].is_music_bot;

    if (is_receiver_existing == TRUE && is_receiver_idle == FALSE && is_receiver_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_direct_chat_message receiver is_existing: ", json_receiver_id->valueint, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender_client_id, server_chat_message_id, json_local_message_id->valueint);
        server_msg__send_chat_message_to_single_client(sender_client_id, json_receiver_id->valueint, server_chat_message_id, json_chat_message_value->valuestring);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__process_direct_chat_message receiver is_existing not is_existing or is in idle mode", json_receiver_id->valueint, "\n");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes a channel chat message, broadcasting it to clients in the sender's channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_channel_chat_message(cJSON* json_root, uint64 sender_client_id)
{
    boole is_channel_existing = FALSE;
    // cJSON* json_receiver_id = 0; json contains this ID, but it is not needed. Id of channel is taken from sender's client struct
    cJSON* json_local_message_id = 0;
    cJSON* json_chat_message_value = 0;
    cJSON* json_message_object = 0;
    uint64 channel_id = 0;
    uint64 server_chat_message_id = 0;

    boole status = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_message base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_json_chat_message_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
        return;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_message got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_channels_global_rwlock_guard);

    channel_id = g_clients_array[sender_client_id].channel_id;
    is_channel_existing = g_channel_array[channel_id].is_existing;

    if (is_channel_existing == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiving channel exists: ", channel_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender_client_id, server_chat_message_id, json_local_message_id->valueint);
        server_msg__send_chat_message_to_clients_in_same_channel(sender_client_id, channel_id, server_chat_message_id, json_chat_message_value->valuestring);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiving channel does not exist: ", channel_id, "\n");
    }

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief shared worker for delete/edit chat message requests, it rate-limits, stamps the
 *        requester's identity and rebroadcasts the action to the right audience
 *
 *        the client sends only the target message id (plus the new value for an edit) and the
 *        receiver context. the server keeps no message state, so the action goes back out either
 *        to the sender's channel, or to the private counterpart plus the requester. each receiving
 *        client then decides for itself whether to honour it, based on the requester's public key
 *        and admin flag.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 * @param char* outbound_action_type -> "chat_message_delete" or "chat_message_edit"
 * @param boole is_edit -> TRUE for edit (reads and validates new_message_value), FALSE for delete
 *
 * @return void
 */
static void _client_msg_internal__process_chat_message_action(cJSON* json_root, uint64 sender_client_id, char* outbound_action_type, boole is_edit)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_id = 0;
    cJSON* json_receiver_type = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_new_message_value = 0;
    char* receiver_type = 0;
    char* new_message_value = NULL_POINTER;
    char* requester_public_key = 0;
    uint64 target_chat_message_id = 0;
    uint64 channel_id = 0;
    int64 receiver_id = 0;
    boole requester_is_admin = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "message_id");
    json_receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");

    if (cJSON_IsNumber(json_message_id) == FALSE || cJSON_IsString(json_receiver_type) == FALSE)
    {
        return;
    }

    target_chat_message_id = (uint64)json_message_id->valuedouble;
    receiver_type = json_receiver_type->valuestring;

    if (is_edit == TRUE)
    {
        json_new_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_message_value");
        if (cJSON_IsString(json_new_message_value) == FALSE || json_new_message_value->valuestring == NULL_POINTER)
        {
            return;
        }

        if (clib__utf8_string_length(json_new_message_value->valuestring) == 0)
        {
            return;
        }

        // same oversize guard a normal chat message gets (see _client_msg_internal__is_json_chat_message_format_valid)
        if (clib__utf8_string_length_check_max_length(json_new_message_value->valuestring, (int)((g_server_settings.websocket_message_max_length * 3) / 4 - 2048)) == -1)
        {
            return;
        }

        new_message_value = json_new_message_value->valuestring;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    requester_public_key = g_clients_array[sender_client_id].public_key;
    requester_is_admin = g_clients_array[sender_client_id].is_admin;

    if (clib__is_string_equal(receiver_type, "user") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
        if (cJSON_IsNumber(json_receiver_id) == TRUE)
        {
            receiver_id = json_receiver_id->valueint;
            if (receiver_id >= 0 && receiver_id < g_server_settings.max_client_count && g_clients_array[receiver_id].is_authenticated == TRUE)
            {
                // deliver to the private counterpart and back to the requester so both views update
                server_msg__send_chat_message_action_to_single_client((uint64)receiver_id, outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
                server_msg__send_chat_message_action_to_single_client(sender_client_id, outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
            }
        }
    }
    else
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        channel_id = g_clients_array[sender_client_id].channel_id;
        server_msg__send_chat_message_action_to_clients_in_same_channel(channel_id, outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
        clib__unlock(&g_channels_global_rwlock_guard);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a request to delete a chat message (rebroadcasts a delete action to the right audience)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_delete_chat_message_request(cJSON* json_root, uint64 sender_client_id)
{
    _client_msg_internal__process_chat_message_action(json_root, sender_client_id, "chat_message_delete", FALSE);
}

/**
 * @brief processes a request to edit a chat message (rebroadcasts an edit action to the right audience)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_edit_chat_message_request(cJSON* json_root, uint64 sender_client_id)
{
    _client_msg_internal__process_chat_message_action(json_root, sender_client_id, "chat_message_edit", TRUE);
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
void client_msg__process_channel_chat_picture(uint64 client_sender_id, uint64 local_message_id, char* message_value)
{
    boole is_channel_existing = FALSE;
    uint64 channel_id = 0;
    uint64 server_chat_message_id = 0;
    data_for_file_send_thread_t* arg = NULL_POINTER;
    uint64 buffer_to_send_total_size = 0;
    char* message_copy = NULL_POINTER;
    uint64 thread_id = 0;

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_picture got here \n");

    channel_id = g_clients_array[client_sender_id].channel_id;
    // bounds check, channel_id is not validated anywhere
    is_channel_existing = (channel_id < g_server_settings.max_channel_count) && g_channel_array[channel_id].is_existing;

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_channel_chat_picture got here \n");

    if (is_channel_existing == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_channel_chat_picture receiving channel exists: ", channel_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(client_sender_id, channel_id, server_chat_message_id);

        arg = (data_for_file_send_thread_t*)memorymanager__allocate(sizeof(data_for_file_send_thread_t), MEMALLOC_FILE_DOWNLOAD_BY_PARTS);
        clib__null_memory(arg, sizeof(data_for_file_send_thread_t));
        buffer_to_send_total_size = clib__utf8_string_length(message_value);
        message_copy = (char*)memorymanager__allocate(buffer_to_send_total_size + 1, MEMALLOC_FILE_DOWNLOAD_BY_PARTS); // old buffer points to file upload buffer, it's freed before download thread finishes sending this, need new one
        clib__copy_memory(message_value, message_copy, buffer_to_send_total_size, buffer_to_send_total_size);
        message_copy[buffer_to_send_total_size] = 0; // null terminator
        arg->buffer = message_copy;
        arg->client_sender_id = client_sender_id;
        arg->receiving_clients_count = base__get_other_clients_in_channel(client_sender_id, channel_id, &arg->receiving_client_ids[0]);
        arg->send_type = FILE_SEND_TYPE_TO_CHANNEL;
        arg->size = clib__utf8_string_length(message_value);
        arg->server_chat_message_id = server_chat_message_id;
        arg->local_chat_message_id = local_message_id;

        pthread_create((pthread_t*)&thread_id, 0, (void*)&_client_msg_internal__file_download_thread, arg);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_channel_chat_picture receiving channel does not exist: ", channel_id, "\n");
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
 * @note this function gets called by another processing function, client_msg__process_file_send_completed_request, that makes it different from the other functions
 *
 * @return void
 */
void client_msg__process_direct_chat_picture(uint64 sender_client_id, uint64 receiver_id, uint64 local_message_id, char* message_value)
{
    uint64 server_chat_message_id = 0;
    data_for_file_send_thread_t* arg = NULL_POINTER;
    uint64 buffer_to_send_total_size = 0;
    char* message_copy = NULL_POINTER;
    uint64 thread_id = 0;

    // status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    // if (status == FALSE)
    // {
    // log_info("%s", " base__is_request_allowed_based_on_spam_protection == FALSE \n");
    // return;
    // }
    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_direct_chat_picture got here \n");

    if (g_clients_array[receiver_id].is_authenticated == TRUE && g_clients_array[receiver_id].is_idle == FALSE && g_clients_array[receiver_id].is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiver is_existing: ", receiver_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__send_chat_picture_metadata_to_single_client(sender_client_id, receiver_id, server_chat_message_id);

        arg = (data_for_file_send_thread_t*)memorymanager__allocate(sizeof(data_for_file_send_thread_t), MEMALLOC_FILE_DOWNLOAD_BY_PARTS);
        clib__null_memory(arg, sizeof(data_for_file_send_thread_t));
        buffer_to_send_total_size = clib__utf8_string_length(message_value);
        message_copy = (char*)memorymanager__allocate(buffer_to_send_total_size + 1, MEMALLOC_FILE_DOWNLOAD_BY_PARTS); // old buffer points to file upload buffer, it's freed before download thread finishes sending this, need new one
        clib__copy_memory(message_value, message_copy, buffer_to_send_total_size, buffer_to_send_total_size);
        message_copy[buffer_to_send_total_size] = 0; // null terminator

        arg->buffer = message_copy;
        arg->client_sender_id = sender_client_id;
        arg->client_receiver_id = receiver_id;
        arg->send_type = FILE_SEND_TYPE_TO_CLIENT;
        arg->size = clib__utf8_string_length(message_value);
        arg->server_chat_message_id = server_chat_message_id;
        arg->local_chat_message_id = local_message_id;

        pthread_create((pthread_t*)&thread_id, 0, (void*)&_client_msg_internal__file_download_thread, arg);
    }
}

/**
 * @brief This function processes join channel request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_join_channel_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_channel_password = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_message_object = 0;
    boole is_client_that_is_leaving_channel_maintainer_of_that_channel = FALSE;
    channel_t* new_channel = 0;
    channel_t* old_channel = 0;
    client_t* client_that_is_joining_channel = 0;
    uint64 new_maintainer_index = 0;
    boole is_maintainer_found = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    client_t* existing_channel_member = 0;
    uint64 x = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_json_join_channel_request_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request client_msg__is_json_join_channel_request_format_valid == FALSE \n");
        return;
    }

    // whole function is wrapped within write locks.
    // because I do not know how to handle multithreaded environment properly
    // so I wrap code in write locks to avoid any unpredictable behaviour of this server
    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    is_authenticated = g_clients_array[sender_client_id].is_authenticated;
    is_existing = g_clients_array[sender_client_id].is_existing;

    if (is_existing == FALSE || is_authenticated == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request is_existing == FALSE || is_authenticated == FALSE \n");
        goto client_msg__process_join_channel_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_channel_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_password");

    new_channel = &g_channel_array[json_channel_id->valueint];

    if (new_channel->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request channel not is_existing \n");
        goto client_msg__process_join_channel_request_end;
    }

    client_that_is_joining_channel = &g_clients_array[sender_client_id];
    old_channel = &g_channel_array[client_that_is_joining_channel->channel_id];

    status = (boole)(client_that_is_joining_channel->channel_id == json_channel_id->valueint);

    if (status == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request client is already in that channel \n");
        goto client_msg__process_join_channel_request_end;
    }

    // channel capacity: a full channel rejects non-admins. the count excludes music bots already (see
    // base__get_client_count_for_channel); admins bypass the limit
    if (new_channel->is_client_limit_active == TRUE
        && client_that_is_joining_channel->is_admin == FALSE
        && (new_channel->max_client_count == 0
            || base__get_client_count_for_channel(new_channel->channel_id) >= new_channel->max_client_count))
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request channel is full, or admin-only (limit 0) \n");
        server_msg__send_channel_full_to_single_client(client_that_is_joining_channel, new_channel->channel_id);
        goto client_msg__process_join_channel_request_end;
    }

    // check if password is valid
    if (new_channel->is_using_password == TRUE)
    {
        status = base__password_matches(json_channel_password->valuestring, new_channel->password) || client_that_is_joining_channel->is_admin;
        if (status == TRUE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request correct password \n");
            goto client_msg__process_join_channel_request_continue;
        }
        else
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request wrong password \n");
            server_msg__send_access_denied_to_single_client(client_that_is_joining_channel);
            goto client_msg__process_join_channel_request_end;
        }
    }

client_msg__process_join_channel_request_continue:
    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue  \n");

    // change channel in client struct
    client_that_is_joining_channel->channel_id = json_channel_id->valueint;
    client_that_is_joining_channel->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

    // if the client owns this temp channel and is now leaving it, destroy it: any remaining members move
    // to root and the channel is freed. old_channel is then zeroed, so the maintainer-handoff code below
    // naturally takes the no-old-maintainer path
    if (old_channel->is_temp_channel == TRUE
        && client_that_is_joining_channel->is_temp_admin_channel == TRUE
        && client_that_is_joining_channel->temp_channel_id == old_channel->channel_id)
    {
        client_that_is_joining_channel->is_temp_admin_channel = FALSE;
        client_that_is_joining_channel->temp_channel_id = 0;
        base__destroy_temp_channel(old_channel->channel_id);
    }

    if (old_channel->is_channel_maintainer_present == TRUE)
    {
        is_client_that_is_leaving_channel_maintainer_of_that_channel = (boole)(old_channel->maintainer_id == client_that_is_joining_channel->client_id);
    }

    if (is_client_that_is_leaving_channel_maintainer_of_that_channel == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue is_client_that_is_leaving_channel_maintainer_of_that_channel TRUE  \n");

        is_maintainer_found = base__find_new_maintainer_for_channel(&new_maintainer_index, old_channel->channel_id, sender_client_id, TRUE);
        if (is_maintainer_found == TRUE)
        {
            // client that left channel was maintainer of that channel, choose new maintainer
            // then broadcast channel join message
            // then send new maintainer id to clients in that channel so they know who new maintainer is
            DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_join_channel_request_continue maintainer found ", new_maintainer_index, "\n");
            old_channel->is_channel_maintainer_present = TRUE;
            old_channel->maintainer_id = new_maintainer_index;
            old_channel->maintainer_generation++;

            // first send join message, then maintainer message for clients in that channel
            server_msg__send_channel_join_message_to_all_clients(client_that_is_joining_channel, new_channel);

            // client is joining channel that was hidden and clients in it were not visible to him
            // send clients to him using client_join_channel message
            if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE && new_channel->is_using_password == TRUE)
            {
                for (x = 0; x < g_server_settings.max_client_count; x++)
                {
                    existing_channel_member = &g_clients_array[x];

                    if (existing_channel_member->is_existing == FALSE)
                    {
                        continue;
                    }

                    if (existing_channel_member->is_authenticated == FALSE)
                    {
                        continue;
                    }

                    if (existing_channel_member->is_music_bot == TRUE)
                    {
                        continue;
                    }

                    if (existing_channel_member->channel_id != new_channel->channel_id)
                    {
                        continue;
                    }

                    server_msg__send_channel_join_message_to_single_client(existing_channel_member, new_channel, client_that_is_joining_channel);
                }
            }

            server_msg__send_maintainer_id_to_clients_in_same_channel(old_channel->channel_id, old_channel->maintainer_id);
        }
        else
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue maintainer found  FALSE \n");
            old_channel->is_channel_maintainer_present = FALSE;
            old_channel->maintainer_id = 0;
            old_channel->maintainer_generation++;
            server_msg__send_channel_join_message_to_all_clients(client_that_is_joining_channel, new_channel);

            // client is joining channel that was hidden and clients in it were not visible to him
            // send clients to him using client_join_channel message
            if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE && new_channel->is_using_password == TRUE)
            {
                for (x = 0; x < g_server_settings.max_client_count; x++)
                {
                    existing_channel_member = &g_clients_array[x];

                    if (existing_channel_member->is_existing == FALSE)
                    {
                        continue;
                    }

                    if (existing_channel_member->is_authenticated == FALSE)
                    {
                        continue;
                    }

                    if (existing_channel_member->is_music_bot == TRUE)
                    {
                        continue;
                    }

                    if (existing_channel_member->channel_id != new_channel->channel_id)
                    {
                        continue;
                    }

                    server_msg__send_channel_join_message_to_single_client(existing_channel_member, new_channel, client_that_is_joining_channel);
                }
            }
        }
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue is_client_that_is_leaving_channel_maintainer_of_that_channel FALSE  \n");
        server_msg__send_channel_join_message_to_all_clients(client_that_is_joining_channel, new_channel);

        // client is joining channel that was hidden and clients in it were not visible to him
        // send clients to him using client_join_channel message
        if (g_server_settings.is_hide_clients_in_password_protected_channels_active && new_channel->is_using_password)
        {
            for (x = 0; x < g_server_settings.max_client_count; x++)
            {
                existing_channel_member = &g_clients_array[x];

                if (existing_channel_member->is_existing == FALSE)
                {
                    continue;
                }

                if (existing_channel_member->is_authenticated == FALSE)
                {
                    continue;
                }

                if (existing_channel_member->is_music_bot == TRUE)
                {
                    continue;
                }

                if (existing_channel_member->channel_id != new_channel->channel_id)
                {
                    continue;
                }

                server_msg__send_channel_join_message_to_single_client(existing_channel_member, new_channel, client_that_is_joining_channel);
            }
        }
    }

    audio_channel__process_client_channel_join(client_that_is_joining_channel);

    // at this point channel is joined
    // but there is still some work to do, find out how many clients are there in newly joined channel,
    // if there is only one client, the newly joined client, he must be the maintainer of it
    if (base__get_client_count_for_channel(new_channel->channel_id) == 1)
    {
        new_channel->maintainer_id = client_that_is_joining_channel->client_id;
        new_channel->is_channel_maintainer_present = TRUE;
        new_channel->maintainer_generation++;
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_join_channel_request_continue client that joined channel is the only one in the channel, he is maintainer of it \n");
    }

    server_msg__send_maintainer_id_to_single_client(client_that_is_joining_channel, new_channel->channel_id, new_channel->maintainer_id);

    server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_that_is_joining_channel->p_ws_connection, client_that_is_joining_channel->dh_shared_secret, client_that_is_joining_channel->channel_id);

client_msg__process_join_channel_request_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes delete channel request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note hmmm
 *
 * @return void
 */
void client_msg__process_delete_channel_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    cJSON* json_channel_id = 0;
    cJSON* json_message_object = 0;
    uint64* channel_ids_to_delete = 0;
    uint64 channels_to_delete_count = 0;
    client_t* client_to_move_maybe = 0;
    uint64 channel_id_to_delete = 0;
    uint64 client_count_in_channel = 0;
    uint64 index_of_new_maintainer = 0;
    boole is_channel_delete_allowed = FALSE;
    uint64 marked_channel_index = 0;
    uint64 client_id = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_json_delete_request_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request _client_msg_internal__is_json_delete_request_format_valid == FALSE \n");
        return;
    }

    // add admin rights check later
    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    is_authenticated = g_clients_array[sender_client_id].is_authenticated;
    is_existing = g_clients_array[sender_client_id].is_existing;

    if (is_existing == FALSE || is_authenticated == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request is_existing == FALSE || is_authenticated == FALSE \n");
        goto label_client_msg__process_delete_channel_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    status = g_channel_array[json_channel_id->valueint].is_existing;

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_channel_request channel does not exist \n");
        goto label_client_msg__process_delete_channel_request_end;
    }

    // deleting a channel is admin-only, except a temp admin may delete their own temp channel
    is_channel_delete_allowed = FALSE;
    if (g_clients_array[sender_client_id].is_admin == TRUE)
    {
        is_channel_delete_allowed = TRUE;
    }
    else if (g_clients_array[sender_client_id].is_temp_admin_channel == TRUE
        && g_clients_array[sender_client_id].temp_channel_id == (uint64)json_channel_id->valueint
        && g_channel_array[json_channel_id->valueint].is_temp_channel == TRUE)
    {
        is_channel_delete_allowed = TRUE;
    }

    if (is_channel_delete_allowed == TRUE)
    {
        channel_ids_to_delete = (uint64*)memorymanager__allocate(g_server_settings.max_channel_count * sizeof(uint64), MEMALLOC_MARKED_CHANNEL_INDICES);
        channel_ids_to_delete[channels_to_delete_count] = json_channel_id->valueint;
        channels_to_delete_count += 1;

        // json_channel_id->valueint channel id to start marking other child channels from
        // channels_to_delete_count count of marked channels to delete
        // array that stores ids of channels that should be deleted
        base__mark_channels_for_deletion(json_channel_id->valueint, &channels_to_delete_count, channel_ids_to_delete);

        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_delete_channel_request channel ids to delete count ", channels_to_delete_count, "\n");

        // at this point the channels are marked, loop through marked channel ids, but do not delete them yet, just find out if there are clients there
        for (marked_channel_index = 0; marked_channel_index < channels_to_delete_count; marked_channel_index++)
        {
            channel_id_to_delete = channel_ids_to_delete[marked_channel_index];
            client_count_in_channel = base__get_client_count_for_channel(channel_id_to_delete);

            DBG_CLIENT_MESSAGE log_info("%s %llu %s %llu %s", "client_msg__process_delete_channel_request client count in channel ", channel_id_to_delete, " is ", client_count_in_channel, "\n");

            for (client_id = 0; client_id < g_server_settings.max_client_count; client_id++)
            {
                client_to_move_maybe = &g_clients_array[client_id];

                if (client_to_move_maybe->is_existing == FALSE)
                {
                    continue;
                }
                if (client_to_move_maybe->is_authenticated == FALSE)
                {
                    continue;
                }
                if (client_to_move_maybe->channel_id != channel_id_to_delete)
                {
                    continue;
                }

                // client found
                DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__process_delete_channel_request moving client ", client_to_move_maybe->client_id, "to root channel \n");

                client_to_move_maybe->channel_id = ROOT_CHANNEL_ID;
                client_to_move_maybe->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

                // keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this
                // client on the channel-mismatch check after the move to root
                audio_channel__process_client_channel_join(client_to_move_maybe);

                server_msg__send_channel_join_message_to_all_clients(client_to_move_maybe, &g_channel_array[ROOT_CHANNEL_ID]);

                if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == TRUE)
                {
                    DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_delete_channel_request also sending maintainer id of root channel to moving client ", g_channel_array[ROOT_CHANNEL_ID].maintainer_id, "\n");
                    server_msg__send_maintainer_id_to_single_client(client_to_move_maybe, ROOT_CHANNEL_ID, g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
                }

                server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move_maybe->p_ws_connection, client_to_move_maybe->dh_shared_secret, ROOT_CHANNEL_ID);
            }
        }

        // clients are moved, now delete channels
        for (marked_channel_index = 0; marked_channel_index < channels_to_delete_count; marked_channel_index++)
        {
            channel_id_to_delete = channel_ids_to_delete[marked_channel_index];

            clib__null_memory(&g_channel_array[channel_id_to_delete], sizeof(channel_t));
            server_msg__send_channel_delete_message_to_all_clients(channel_id_to_delete, sender_client_id);
        }
        // if there is no maintainer in root channel, find new maintainer now
        if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
        {
            status = base__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE);
            if (status == TRUE)
            {
                g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present = TRUE;
                g_channel_array[ROOT_CHANNEL_ID].maintainer_id = index_of_new_maintainer;
                g_channel_array[ROOT_CHANNEL_ID].maintainer_generation++;
                server_msg__send_maintainer_id_to_clients_in_same_channel(ROOT_CHANNEL_ID, g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
            }
        }

        // if the sender just deleted their own temp channel, they are no longer a temp admin
        if (g_clients_array[sender_client_id].is_temp_admin_channel == TRUE
            && g_clients_array[sender_client_id].temp_channel_id == (uint64)json_channel_id->valueint)
        {
            g_clients_array[sender_client_id].is_temp_admin_channel = FALSE;
            g_clients_array[sender_client_id].temp_channel_id = 0;
        }
    }
    else
    {
        if (g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_existing == TRUE)
        {
            server_msg__send_access_denied_to_single_client(&g_clients_array[sender_client_id]);
        }
    }

label_client_msg__process_delete_channel_request_end:

    memorymanager__free((nuint)channel_ids_to_delete);

    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes a poke-client request, forwarding the poke message to the target client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_poke_client_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_poke_message = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_poke_client_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_json_poke_client_request_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_poke_client_request client_msg__is_json_poke_client_request_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__read_lock(&g_clients_global_rwlock_guard);

    if (g_clients_array[json_receiver_id->valueint].is_authenticated == TRUE)
    {
        server_msg__send_poke_to_single_client(&g_clients_array[json_receiver_id->valueint], sender_client_id, json_poke_message->valuestring);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief passes on "this client is typing" to the conversation he is writing to. the request carries
 *        no message content and nothing is stored - it is forwarded to whoever is on the other side
 *        and forgotten. the whole feature is off unless the server allows it.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that is typing
 *
 * @attention this function uses read lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__process_typing_indicator_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_direct_message = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_receiver_type = 0;
    cJSON* json_receiver_id = 0;

    if (g_server_settings.allow_typing_indicator == FALSE)
    {
        return;
    }

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_typing_indicator_request spam protection == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");

    if (cJSON_IsString(json_receiver_type) == FALSE || json_receiver_type->valuestring == NULL_POINTER)
    {
        return;
    }

    if (cJSON_IsNumber(json_receiver_id) == FALSE || json_receiver_id->valueint < 0)
    {
        return;
    }

    is_direct_message = clib__is_string_equal(json_receiver_type->valuestring, "user");

    if (is_direct_message == FALSE && clib__is_string_equal(json_receiver_type->valuestring, "channel") == FALSE)
    {
        return;  // only those two kinds of conversation exist
    }

    if (is_direct_message == TRUE && (uint64)json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // a channel indicator may only be sent into the channel the sender actually stands in, so nobody
    // can announce himself into a room he is not in
    if (is_direct_message == FALSE && g_clients_array[sender_client_id].channel_id != (uint64)json_receiver_id->valueint)
    {
        goto label_client_msg__process_typing_indicator_request_end;
    }

    server_msg__send_typing_indicator(sender_client_id, json_receiver_type->valuestring, (uint64)json_receiver_id->valueint);

label_client_msg__process_typing_indicator_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes sdp answer for offer that server sent to client as part of process of establishing webrtc datachannel connection
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_sdp_answer(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_value = 0;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_sdp_answer");

    status = _client_msg_internal__is_json_sdp_answer_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_sdp_answer client_msg__is_json_sdp_answer_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_message_value = cJSON_GetObjectItem(json_message_object, "value");

    clib__read_lock(&g_clients_global_rwlock_guard);

    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_sdp_answer got here");

        audio_channel__process_sdp_answer_from_remote_peer(&g_clients_array[sender_client_id], json_message_value);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes ice candidate. When I tried this on local network, this was not needed. Still, good to send and process
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_ice_candidate(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_value = 0;

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_ice_candidate");

    status = _client_msg_internal__is_ice_candidate_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_ice_candidate client_msg__is_ice_candidate_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_message_value = cJSON_GetObjectItem(json_message_object, "value");

    clib__read_lock(&g_clients_global_rwlock_guard);

    if (g_clients_array[sender_client_id].is_authenticated == TRUE)
    {
        DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "client_msg__process_ice_candidate got here");

        audio_channel__process_ice_candidate_from_remote_peer(&g_clients_array[sender_client_id], json_message_value);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's microphone-usage change and broadcasts the resulting audio state to the channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_microphone_usage(cJSON* json_root, uint64 sender_client_id)
{
    cJSON* json_value = 0;
    cJSON* json_message_object = 0;
    boole status = FALSE;
    int received_microphone_usage = 0;
    client_t* client = 0;

    status = _client_msg_internal__is_json_process_microphone_usage_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_microphone_usage _client_msg_internal__is_json_process_microphone_usage_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_microphone_usage_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    received_microphone_usage = json_value->valueint;

    // only allow changing audio state if audio is not completely disabled
    if (client->audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
    {
        if (received_microphone_usage == MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO)
        {
            if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_ENABLED)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
                audio_channel__set_is_client_sending_audio(client->client_id, FALSE);
                server_msg__send_audio_state_of_client_to_all_clients(sender_client_id, client->audio_state);
            }
        }
        else if (received_microphone_usage == MICROPHONE_USAGE__DISABLE_PUSH_TO_TALK)
        {
            if (client->audio_state != AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
            {
                client->audio_state = AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;

                if (client->is_streaming_song == TRUE)
                {
                    client->is_streaming_song = FALSE;
                    server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client);
                }

                audio_channel__set_is_client_sending_audio(client->client_id, FALSE);
                server_msg__send_audio_state_of_client_to_all_clients(sender_client_id, client->audio_state);
            }
        }
        else if (received_microphone_usage == MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
        {
            if (client->audio_state != MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO)
            {
                client->audio_state = MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO;
                audio_channel__set_is_client_sending_audio(client->client_id, TRUE);
                server_msg__send_audio_state_of_client_to_all_clients(sender_client_id, client->audio_state);
            }
        }
    }

label_client_msg__process_microphone_usage_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request to start streaming a song to its channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_start_song_stream_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* json_song_name = 0;
    cJSON* json_message_object = 0;

    status = _client_msg_internal__is_json_start_song_stream_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_start_song_stream_message client_msg__is_json_start_song_stream_message_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_start_song_stream_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_name");

    client->is_streaming_song = TRUE;
    clib__null_memory(client->song_name, SONG_NAME_MAX_LENGTH);
    clib__copy_memory(json_song_name->valuestring, client->song_name, clib__utf8_string_length(json_song_name->valuestring), SONG_NAME_MAX_LENGTH);

    server_msg__send_start_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_start_song_stream_end:

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request to stop streaming its song
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_stop_song_stream_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE)
    {
        goto label_client_msg__process_stop_song_stream_end;
    }

    server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client);

label_client_msg__process_stop_song_stream_end:

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin-password message, granting the client admin rights if the password matches
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_admin_password_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* admin_password = 0;
    cJSON* json_message_object = 0;
    uint64 cvector_loop_index = 0;
    boole is_tag_found = FALSE;

    status = _client_msg_internal__is_admin_password_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_admin_password_message _client_msg_internal__is_admin_password_message_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_admin_password_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    admin_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    status = base__password_matches(admin_password->valuestring, g_server_settings.admin_password);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", sender_client_id, "admin password is not correct \n");
        goto label_client_msg__process_admin_password_message_end;
    }

    if (client->is_admin == FALSE)
    {
        client->is_admin = TRUE;

        if (g_server_settings.is_display_admin_tag_active == TRUE)
        {
            // only add admin tag to client if he doesn't have it
            if (client->tag_ids != NULL_POINTER)
            {
                for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
                {
                    if (client->tag_ids[cvector_loop_index] == ADMIN_TAG_ID)
                    {
                        is_tag_found = TRUE;
                        break;
                    }
                }
            }

            if (is_tag_found == FALSE)
            {
                cvector_push_back(client->tag_ids, ADMIN_TAG_ID);
            }

            server_msg__send_add_tag_to_client_event_to_all_clients(client->client_id, ADMIN_TAG_ID);
        }

        // tie this identity to its tags in ram right away, so a reconnect restores admin within this
        // server run even before any disk save. persistence to disk still needs a settings save
        base__sync_client_identity_in_store(client);

        // the setup admin password was typed in cleartext; on the first admin login, ask for a one-time change
        if (g_server_settings.admin_password_is_initial == TRUE)
        {
            server_msg__send_force_admin_password_change_to_single_client(client);
            g_server_settings.admin_password_is_initial = FALSE;
            base__save_server_settings_to_file();
        }
    }

    // send admin status to other clients possibly, or not, do they have to know you are an admin
label_client_msg__process_admin_password_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a change-admin-password request from an authenticated admin: hashes and stores the new password
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_change_admin_password_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    cJSON* new_password = 0;
    cJSON* json_message_object = 0;

    status = _client_msg_internal__is_admin_password_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_admin_password_message invalid message \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    if (client->is_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_admin_password_message sender is not an admin \n");
        goto label_client_msg__process_change_admin_password_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    new_password = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    base__hash_password_to_base64(new_password->valuestring, &g_server_settings.admin_password[0], ADMIN_PASSWORD_MAX_LENGTH);
    g_server_settings.admin_password_is_initial = FALSE;
    base__save_server_settings_to_file();

    DBG_CLIENT_MESSAGE log_info("%s", "admin password changed by admin \n");

label_client_msg__process_change_admin_password_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to add a tag to a client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_add_tag_to_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_add_tag = FALSE;
    client_t* client = 0;
    client_t* client_to_add_tag_to = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id_to_add_tag_to = 0;
    cJSON* json_tag_id = 0;

    status = _client_msg_internal__is_add_tag_to_client_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_add_tag_to_client_message _client_msg_internal__is_add_tag_to_client_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // check if client that is sending request has permission to add tags
    does_sender_have_permissions_to_add_tag = client->is_admin;

    if (does_sender_have_permissions_to_add_tag == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to add tag \n");
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id_to_add_tag_to = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

    if (g_server_settings.is_display_admin_tag_active == FALSE && json_tag_id->valueint == ADMIN_TAG_ID)
    {
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // check if client whose tag is about to be added exists
    client_to_add_tag_to = &g_clients_array[json_client_id_to_add_tag_to->valueint];

    if (client_to_add_tag_to->is_authenticated == FALSE || client_to_add_tag_to->is_existing == FALSE)
    {
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // check if the tag itself that is about to be added exists
    status = base__is_tag_id_real(json_tag_id->valueint);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "tag id : ", json_tag_id->valueint, "does not exist \n");
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // now check if the client already has the tag id about to be added
    status = base__is_client_already_assigned_this_tag_id(json_client_id_to_add_tag_to->valueint, json_tag_id->valueint);
    if (status == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "tag id : ", json_tag_id->valueint, " already is assigned to client ", json_client_id_to_add_tag_to->valueint, "\n");
        goto label_client_msg__process_add_tag_to_client_message_end;
    }

    // at this point following is clear
    // - client has a permission to add this tag id
    // - receiving client exists and doesn't have that tag id yet
    // - tag id is valid (exists)
    // - all that is left is to add that tag id to client, and make him admin if tag id happens to be admin
    cvector_push_back(client_to_add_tag_to->tag_ids, json_tag_id->valueint);

    if (json_tag_id->valueint == ADMIN_TAG_ID)
    {
        client_to_add_tag_to->is_admin = TRUE;
    }

    server_msg__send_add_tag_to_client_event_to_all_clients(client_to_add_tag_to->client_id, json_tag_id->valueint);

    // mirror the change into the ram identity store immediately, so the tag survives a reconnect
    // within this server run without needing an admin disk save
    base__sync_client_identity_in_store(client_to_add_tag_to);

    // send admin status to other clients possibly, or not, do they have to know you are an admin
label_client_msg__process_add_tag_to_client_message_end:
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
static boole _client_msg_internal__is_set_alias_request_valid(cJSON* json_root)
{
    cJSON* json_client_id = 0;
    cJSON* json_alias = 0;
    cJSON* json_message_object = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    if (cJSON_IsNumber(json_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_set_alias_request_valid cJSON_IsNumber(client_id) \n");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "_client_msg_internal__is_set_alias_request_valid json_client_id->valueint is not valid ", json_client_id->valueint, "\n");
        return FALSE;
    }

    json_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "alias");
    if (cJSON_IsString(json_alias) == FALSE || json_alias->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_set_alias_request_valid cJSON_IsString(alias) \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_alias->valuestring) >= USERNAME_MAX_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_set_alias_request_valid alias too long \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief validates an offline chat message request: a registered recipient alias and a non-empty
 *        ciphertext within the queue's per-message cap.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_internal__is_offline_chat_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_recipient_alias = 0;
    cJSON* json_value = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return FALSE;
    }

    json_recipient_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "recipient_alias");
    if (cJSON_IsString(json_recipient_alias) == FALSE || json_recipient_alias->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_offline_chat_message_valid recipient_alias \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_recipient_alias->valuestring) == 0 || clib__utf8_string_length(json_recipient_alias->valuestring) >= USERNAME_MAX_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_offline_chat_message_valid recipient_alias length \n");
        return FALSE;
    }

    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsString(json_value) == FALSE || json_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_offline_chat_message_valid value \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_value->valuestring) == 0 || clib__utf8_string_length(json_value->valuestring) >= MAX_OFFLINE_MESSAGE_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_offline_chat_message_valid value length \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief parks a text message addressed to a registered identity that is not connected, and hands
 *        it over when that identity comes back
 *
 *        the payload arrives already encrypted, so the server never sees plaintext: the sender
 *        encrypted it with the recipient's public key, which it took from the stored-clients list.
 *        the message is parked in ram, keyed by identity hash rather than by alias.
 *
 *        the request is refused unless the feature is on, identities and the stored-clients list
 *        are on, the SENDER is registered, the recipient alias belongs to a registered identity,
 *        and that identity is not connected right now (if it is, the client should send a normal
 *        direct message instead).
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_offline_chat_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole queued = FALSE;
    boole is_recipient_connected = FALSE;
    client_t* sender = 0;
    cJSON* json_message_object = 0;
    cJSON* json_recipient_alias = 0;
    cJSON* json_value = 0;
    char sender_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    char recipient_identity_hash[IDENTITY_HASH_MAX_LENGTH];
    char sender_alias[USERNAME_MAX_LENGTH];
    char* message_value = 0;
    uint64 i = 0;

    if (g_server_settings.are_identities_enabled == FALSE || g_server_settings.allow_stored_clients_list == FALSE || g_server_settings.allow_offline_messages == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_offline_chat_message offline messages are not enabled \n");
        return;
    }

    status = _client_msg_internal__is_offline_chat_message_valid(json_root);
    if (status == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_recipient_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "recipient_alias");
    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    clib__null_memory(&sender_identity_hash[0], sizeof(sender_identity_hash));
    clib__null_memory(&recipient_identity_hash[0], sizeof(recipient_identity_hash));
    clib__null_memory(&sender_alias[0], sizeof(sender_alias));

    // read the sender out of the clients array, then release: queueing takes its own leaf lock
    clib__read_lock(&g_clients_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];

    if (sender->is_existing == FALSE || sender->is_authenticated == FALSE || sender->is_music_bot == TRUE || sender->public_key[0] == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // only a registered user may leave messages, so an anonymous guest cannot spam the queue
    if (sender->is_registered == FALSE || sender->alias[0] == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_offline_chat_message sender", sender_client_id, "is not registered \n");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    base__hash_password_to_base64(sender->public_key, sender_identity_hash, sizeof(sender_identity_hash));
    clib__copy_memory(&sender->alias[0], &sender_alias[0], clib__utf8_string_length(&sender->alias[0]), USERNAME_MAX_LENGTH - 1);

    // if the recipient is connected right now there is nothing to queue - the sender should have
    // used a normal direct message. checked while the clients lock is held
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (g_clients_array[i].alias[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(&g_clients_array[i].alias[0], json_recipient_alias->valuestring) == TRUE)
        {
            is_recipient_connected = TRUE;
            break;
        }
    }

    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_recipient_connected == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_offline_chat_message recipient is online, not queueing \n");
        return;
    }

    // resolve the alias to the identity that owns it: the queue is keyed by identity, so moving an
    // alias to somebody else later cannot hand them messages meant for the original owner
    if (base__get_identity_hash_by_alias(json_recipient_alias->valuestring, &recipient_identity_hash[0], sizeof(recipient_identity_hash)) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_offline_chat_message no identity carries that alias \n");
        return;
    }

    message_value = json_value->valuestring;

    queued = base__queue_offline_message(&recipient_identity_hash[0], &sender_identity_hash[0], &sender_alias[0], message_value);

    if (queued == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_offline_chat_message could not queue the message \n");
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
static boole _client_msg_internal__is_set_identity_alias_request_valid(cJSON* json_root)
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
 *        that is the difference from client_msg__process_set_alias_request: this one works while
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
void client_msg__process_set_identity_alias_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_sender_admin = FALSE;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_public_key_hash = 0;
    cJSON* json_alias = 0;
    char connected_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;

    if (g_server_settings.are_identities_enabled == FALSE || g_server_settings.allow_alias_registrations == FALSE)
    {
        return;
    }

    status = _client_msg_internal__is_set_identity_alias_request_valid(json_root);
    if (status == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_public_key_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");
    json_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "alias");

    clib__read_lock(&g_clients_global_rwlock_guard);
    is_sender_admin = util__is_client_valid_admin(sender_client_id);
    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_sender_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_identity_alias_request sender is not an admin \n");
        return;
    }

    // aliases are the handle offline entries are paired by, so two identities must never share one
    if (json_alias->valuestring[0] != 0 && base__is_alias_taken_by_another_identity(json_alias->valuestring, json_public_key_hash->valuestring) == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_identity_alias_request alias is already taken \n");
        return;
    }

    base__set_identity_alias_by_hash(json_public_key_hash->valuestring, json_alias->valuestring);

    // the owner may be connected right now: keep their live row and everyone else's view in step
    clib__write_lock(&g_clients_global_rwlock_guard);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE || client->public_key[0] == 0)
        {
            continue;
        }

        base__hash_password_to_base64(client->public_key, connected_identity_hash, sizeof(connected_identity_hash));

        if (clib__is_string_equal(connected_identity_hash, json_public_key_hash->valuestring) == FALSE)
        {
            continue;
        }

        clib__null_memory(&client->alias[0], USERNAME_MAX_LENGTH);
        clib__copy_memory(json_alias->valuestring, &client->alias[0], clib__utf8_string_length(json_alias->valuestring), USERNAME_MAX_LENGTH - 1);
        client->is_registered = (boole)(client->alias[0] != 0);

        server_msg__send_client_alias_changed_to_all_clients(client->client_id, &client->alias[0]);

        // just became registered: hand over the offline roster without making them reconnect
        if (client->is_registered == TRUE && g_server_settings.allow_stored_clients_list == TRUE)
        {
            server_msg__send_stored_clients_to_single_client(client->p_ws_connection, client->dh_shared_secret);
        }
        break;
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin registers (or clears, with an empty string) an alias - a display name - on another
 *        connected client's identity
 *
 *        the alias lands on the live client and in the identity store, so it survives reconnects
 *        within the run; disk persistence follows on the next settings save. the change is then
 *        broadcast to everyone. requires identities and alias registrations to be enabled.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_alias_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* client = 0;
    client_t* client_to_alias = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    cJSON* json_alias = 0;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];

    if (g_server_settings.are_identities_enabled == FALSE || g_server_settings.allow_alias_registrations == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_alias_request alias registrations are not enabled \n");
        return;
    }

    status = _client_msg_internal__is_set_alias_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_alias_request _client_msg_internal__is_set_alias_request_valid == FALSE \n");
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
        goto label_client_msg__process_set_alias_request_end;  // no identity to attach the alias to
    }

    base__hash_password_to_base64(client_to_alias->public_key, identity_hash, sizeof(identity_hash));

    // aliases are the handle the stored-clients list is keyed by and what clients pair offline
    // entries against, so two identities must never share one (compared ignoring ascii case)
    if (base__is_alias_taken_by_another_identity(json_alias->valuestring, identity_hash) == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__process_set_alias_request alias [", json_alias->valuestring, "] is already taken by another identity \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    // a registered name BECOMES the username, so it must be free among the people connected right
    // now too - the check above only sees stored identities and would happily hand out a name a
    // guest is wearing, leaving two people on screen under one name. refuse instead
    if (_client_msg_internal__is_username_taken_by_another_client(json_alias->valuestring, client_to_alias->client_id) == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__process_set_alias_request username [", json_alias->valuestring, "] is in use by a connected client \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    // the registered name is a username as well, so hold it to the same length limit the rename
    // path enforces - it used to accept anything up to the buffer size
    if (clib__utf8_string_length_check_max_length(json_alias->valuestring, 50) == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__process_set_alias_request alias [", json_alias->valuestring, "] length is not acceptable \n");
        goto label_client_msg__process_set_alias_request_end;
    }

    // live client
    clib__null_memory(&client_to_alias->alias[0], USERNAME_MAX_LENGTH);
    clib__copy_memory(json_alias->valuestring, &client_to_alias->alias[0], clib__utf8_string_length(json_alias->valuestring), USERNAME_MAX_LENGTH - 1);

    // identity store, keyed by the identity hash, so the name survives reconnects
    base__set_identity_alias_by_hash(identity_hash, json_alias->valuestring);

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
        server_msg__send_client_rename_message_to_all_clients(client_to_alias->client_id, client_to_alias->username);
    }

    // this is the moment an identity BECOMES registered, so it is also the moment its raw public key
    // has to be kept - offline messages are encrypted with it while the person is away. it used to be
    // collected only at authentication (skipped here: he was still unregistered when he connected) and
    // at an admin settings save (only sweeps who is connected at that instant). a user aliased mid
    // session and then leaving was therefore listed as an offline contact WITHOUT a key, and everyone
    // trying to write to him got "this server does not keep messages for people who are offline"
    // until he reconnected once. same guards as the authentication path
    if (g_server_settings.allow_offline_messages == TRUE && client_to_alias->is_registered == TRUE && client_to_alias->public_key[0] != 0 && client_to_alias->is_music_bot == FALSE)
    {
        base__store_identity_raw_public_key(identity_hash, client_to_alias->public_key);
    }

    // alias taken away: he is not registered anymore, so nobody may write to him while he is away -
    // drop the key that would allow it instead of leaving it lying in the identity store
    if (client_to_alias->is_registered == FALSE)
    {
        base__clear_identity_raw_public_key(identity_hash);
    }

    server_msg__send_client_alias_changed_to_all_clients(client_to_alias->client_id, &client_to_alias->alias[0]);

    // just became registered: hand him the roster now instead of making him reconnect for it
    if (client_to_alias->is_registered == TRUE && g_server_settings.allow_stored_clients_list == TRUE)
    {
        server_msg__send_stored_clients_to_single_client(client_to_alias->p_ws_connection, client_to_alias->dh_shared_secret);
    }

label_client_msg__process_set_alias_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief sends the requester the identities this server has stored, so it can show people that are
 *        registered here even while they are offline
 *
 *        the response carries only alias, avatar and tags - never public keys, identity hashes or
 *        usernames. it is gated by identities + allow_stored_clients_list, and the requester must
 *        itself be REGISTERED (carry an admin-granted alias), so a random guest cannot join and
 *        harvest everyone's name and face. every client sends this request on connect and a denial
 *        is simply silence. the request itself has no payload.
 *
 * @param cJSON* json_root -> the parsed client request (unused, the request carries no fields)
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_request_stored_clients(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;

    (void)json_root; // the request has no fields to read

    if (g_server_settings.are_identities_enabled == FALSE || g_server_settings.allow_stored_clients_list == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_request_stored_clients stored clients list is not enabled \n");
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
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_request_stored_clients client_id", sender_client_id, "is not registered, denying \n");
        goto label_client_msg__process_request_stored_clients_end;
    }

    server_msg__send_stored_clients_to_single_client(client->p_ws_connection, client->dh_shared_secret);

label_client_msg__process_request_stored_clients_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client sets its OWN avatar, a base64 image data-url, no admin rights needed
 *
 *        gated by allow_avatars. empty and oversize uploads (per the server's configured max, and
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
void client_msg__process_avatar_upload(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_base64_avatar = 0;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 base64_length = 0;
    uint64 max_base64_length = 0;

    if (g_server_settings.allow_avatars == FALSE)
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

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    base__set_identity_avatar_by_hash(identity_hash, json_base64_avatar->valuestring);

    server_msg__send_avatar_changed_event_to_all_clients(client->client_id);

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client deletes its OWN avatar
 *
 *        frees the live avatar on the client_t, clears the entry in the identity store (an empty
 *        string is what clears it) and broadcasts avatar_changed so everyone drops their copy.
 *        gated by allow_avatars. the request carries no fields.
 *
 * @param cJSON* json_root -> the parsed client request (unused, the request carries no fields)
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock while the live avatar and the identity store are updated
 *
 * @return void
 */
void client_msg__process_delete_avatar(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];

    (void)json_root;

    if (g_server_settings.allow_avatars == FALSE)
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

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));
    base__set_identity_avatar_by_hash(identity_hash, "");  // empty string clears it

    server_msg__send_avatar_changed_event_to_all_clients(client->client_id);

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client requests ONE other client's avatar, as the profile pane needs
 *
 *        responds with a single client_avatar message, served straight from the target's live
 *        client_t->base64_avatar, or an empty string when the target has none. gated by
 *        allow_avatars, and the requested client id must be in range.
 *
 * @param cJSON* json_root -> the parsed client request, carries message.client_id
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock while the target is read and the response is sent
 *
 * @return void
 */
void client_msg__process_request_avatar_for_client(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender = 0;
    client_t* target = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    uint64 target_client_id = 0;

    if (g_server_settings.allow_avatars == FALSE)
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

    server_msg__send_client_avatar_to_single_client(sender->p_ws_connection, sender->dh_shared_secret, target_client_id, (target->base64_avatar != NULL_POINTER) ? target->base64_avatar : "");

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief a client requests a CHUNK of avatars, a client_ids array, for lazy-loading
 *
 *        the server sends one client_avatar message per target that actually has a live avatar.
 *        targets with none are skipped so the client keeps its placeholder. no more than 100
 *        avatars go out per request, so a single call cannot blast unbounded work. gated by
 *        allow_avatars.
 *
 * @param cJSON* json_root -> the parsed client request, carries the message.client_ids array
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @note takes the clients write lock for the whole batch loop
 *
 * @return void
 */
void client_msg__process_request_avatars_batch(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender = 0;
    client_t* target = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_ids = 0;
    cJSON* json_id = 0;
    uint64 target_client_id = 0;
    uint64 sent = 0;

    if (g_server_settings.allow_avatars == FALSE)
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
            continue;  // skip clients without a live avatar; the client keeps its placeholder
        }

        server_msg__send_client_avatar_to_single_client(sender->p_ws_connection, sender->dh_shared_secret, target_client_id, target->base64_avatar);
        sent++;
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to remove a tag from a client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_remove_tag_from_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_remove_tag = FALSE;
    client_t* client = 0;
    client_t* client_to_remove_tag_id_from = 0;
    cJSON* json_message_object = 0;
    cJSON* json_client_id_to_remove_tag_id_from = 0;
    cJSON* json_tag_id = 0;
    int64 tag_id_index = 0;

    status = _client_msg_internal__is_remove_tag_from_client_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_remove_tag_from_client_message _client_msg_internal__is_remove_tag_from_client_valid == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // check if client that is sending request has permission to remove tags
    does_sender_have_permissions_to_remove_tag = client->is_admin;

    if (does_sender_have_permissions_to_remove_tag == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to remove tag \n");
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id_to_remove_tag_id_from = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

    // check if tag happens to be admin tag
    if (g_server_settings.is_display_admin_tag_active == FALSE && json_tag_id->valueint == ADMIN_TAG_ID)
    {
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // check if client whose tag is about to be removed exists
    client_to_remove_tag_id_from = &g_clients_array[json_client_id_to_remove_tag_id_from->valueint];

    if (client_to_remove_tag_id_from->is_authenticated == FALSE || client_to_remove_tag_id_from->is_existing == FALSE)
    {
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // check if the tag itself that is about to be removed exists
    status = base__is_tag_id_real(json_tag_id->valueint);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "tag id : ", json_tag_id->valueint, "does not exist \n");
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // now check if the client actually has the tag id about to be removed
    status = base__is_client_already_assigned_this_tag_id(json_client_id_to_remove_tag_id_from->valueint, json_tag_id->valueint);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s %d %s", "tag id : ", json_tag_id->valueint, " already is not assigned to client ", json_client_id_to_remove_tag_id_from->valueint, "\n");
        goto label_client_msg__process_remove_tag_from_client_message_end;
    }

    // at this point following is clear
    // - client has a permission to remove this tag id
    // - receiving client exists and has that tag id
    // - tag id is valid (exists)
    // - all that is left is to remove that tag id from client, and revoke admin if tag id happens to be admin
    tag_id_index = base__get_index_of_tag_id_of_client(json_client_id_to_remove_tag_id_from->valueint, json_tag_id->valueint);

    if (tag_id_index != -1)
    {
        cvector_erase(client_to_remove_tag_id_from->tag_ids, tag_id_index);

        if (json_tag_id->valueint == ADMIN_TAG_ID)
        {
            client_to_remove_tag_id_from->is_admin = FALSE;
        }

        server_msg__send_remove_tag_from_client_event_to_all_clients(client_to_remove_tag_id_from->client_id, json_tag_id->valueint);

        // mirror the removal into the ram identity store immediately (drops the identity entirely
        // if it now wears no tags), so a reconnect does not resurrect a tag that was just removed
        base__sync_client_identity_in_store(client_to_remove_tag_id_from);
    }

label_client_msg__process_remove_tag_from_client_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to upload a new server icon
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_server_settings_icon_upload(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_upload_icon = FALSE;
    uint64 icon_index = 0;
    icon_t* found_icon = 0;
    cJSON* base64_icon_value = 0;
    cJSON* json_message_object = 0;

    status = _client_msg_internal__is_process_server_settings_icon_upload_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_internal__is_process_server_settings_icon_upload_message_valid is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    base64_icon_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "base64_icon_value");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_icons_global_rwlock_guard);

    does_sender_have_permissions_to_upload_icon = util__is_client_valid_admin(sender_client_id);

    if (does_sender_have_permissions_to_upload_icon == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "sender with sender_client_id", sender_client_id, "does not have permission to upload icon \n");
        goto label_client_msg__process_server_settings_icon_upload_end;
    }

    // sender has necessary permission to upload new icon.
    // find out if there is free slot for icon in the array
    for (icon_index = 0; icon_index < MAX_ICONS; icon_index++)
    {
        icon_t* icon = &g_icons_array[icon_index];
        if (icon->is_existing == FALSE)
        {
            found_icon = icon;
            found_icon->is_existing = TRUE;
            clib__copy_memory((void*)base64_icon_value->valuestring, (void*)&found_icon->base64[0], clib__utf8_string_length(base64_icon_value->valuestring), ICON_MAX_LENGTH);
            found_icon->id = icon_index;

            break;
        }
    }

    if (found_icon == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "found icon is null");
        goto label_client_msg__process_server_settings_icon_upload_end;
    }

    // everything is checked
    // write new icon and notify users
    server_msg__send_add_new_icon_event_to_all_clients(found_icon->id, &found_icon->base64[0]);

label_client_msg__process_server_settings_icon_upload_end:
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to create a new tag with a linked icon
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_server_settings_add_new_tag(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permissions_to_add_new_tag_to_server = FALSE;
    cJSON* json_linked_icon_id = 0;
    cJSON* json_tag_name = 0;
    cJSON* json_message_object = 0;
    tag_t* newly_added_tag = 0;
    tag_t* tag_in_loop = 0;
    uint64 tag_index = 0;
    boole is_same_tag_name_found = FALSE;

    status = _client_msg_internal__is_process_server_settings_add_new_tag_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_server_settings_add_new_tag is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_linked_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "linked_icon_id");
    json_tag_name = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_name");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_tags_global_rwlock_guard);

    // the linked icon is optional. when one is given it must reference an existing icon (id equals icon index,
    // range already checked in the validator); when none is given the tag is created without an icon
    if (json_linked_icon_id != NULL_POINTER)
    {
        status = g_icons_array[json_linked_icon_id->valueint].is_existing;

        if (status == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_server_settings_add_new_tag linked icon id does not exist \n");
            goto label_client_msg__process_server_settings_add_new_tag_end;
        }
    }

    // check if client that is sending request has permission to create new server tags
    does_sender_have_permissions_to_add_new_tag_to_server = util__is_client_valid_admin(sender_client_id);

    if (does_sender_have_permissions_to_add_new_tag_to_server == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_set_server_settings_add_new_tag sender with sender_client_id", sender_client_id, "does not have permission to add newly_added_tag \n");
        goto label_client_msg__process_server_settings_add_new_tag_end;
    }

    // do not allow creation of new tag if tag with same name already exists
    for (tag_index = 0; tag_index < MAX_TAGS; tag_index++)
    {
        tag_in_loop = &g_tags_array[tag_index];
        if (tag_in_loop->is_existing == TRUE)
        {
            status = clib__is_string_equal(json_tag_name->valuestring, tag_in_loop->name);

            if (status == TRUE)
            {
                is_same_tag_name_found = TRUE;
                break;
            }
        }
    }

    if (is_same_tag_name_found == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %s %s", " is_same_tag_name_found == TRUE", json_tag_name->valuestring, "\n");
        goto label_client_msg__process_server_settings_add_new_tag_end;
    }

    // at this point client is verified, icon id is correct, newly_added_tag length is correct
    // start at index 1: id 0 is ADMIN_TAG_ID (reserved for the admin tag). a user tag at id 0 would collide
    // with it - adding it would grant admin, and adding/removing it would add/strip the admin tag.
    for (tag_index = 1; tag_index < MAX_TAGS; tag_index++)
    {
        tag_in_loop = &g_tags_array[tag_index];

        if (tag_in_loop->is_existing == FALSE)
        {
            tag_in_loop->is_existing = TRUE;
            if (json_linked_icon_id != NULL_POINTER)
            {
                tag_in_loop->has_icon = TRUE;
                tag_in_loop->icon_id = (uint64)json_linked_icon_id->valueint;
            }
            else
            {
                tag_in_loop->has_icon = FALSE;
                tag_in_loop->icon_id = 0;
            }
            clib__copy_memory((void*)&json_tag_name->valuestring[0], (void*)&tag_in_loop->name[0], clib__utf8_string_length(json_tag_name->valuestring), TAG_MAX_NAME_LENGTH);
            tag_in_loop->id = tag_index;

            newly_added_tag = tag_in_loop;
            break;
        }
    }

    if (newly_added_tag == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "newly_added_tag == NULL_POINTER");
        goto label_client_msg__process_server_settings_add_new_tag_end;
    }

    server_msg__send_create_new_tag_event_to_all_clients(tag_in_loop->id, tag_in_loop->name, tag_in_loop->icon_id, tag_in_loop->has_icon);

label_client_msg__process_server_settings_add_new_tag_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to delete a tag from the server's tag pool. the tag is stripped from every
 *        client that currently holds it, its pool slot is freed, and all clients are told to remove it. the
 *        admin tag (ADMIN_TAG_ID) can never be deleted this way.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_server_settings_delete_tag(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    client_t* client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_tag_id = 0;
    uint64 tag_id = 0;
    uint64 i = 0;
    int64 tag_id_index = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");

    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_server_settings_delete_tag: tag_id missing or not a number");
        return;
    }

    tag_id = (uint64)json_tag_id->valueint;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_tags_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_tag_end;
    }

    // the admin tag lives in slot 0 and is never a deletable pool tag
    if (tag_id == ADMIN_TAG_ID)
    {
        goto label_client_msg__process_set_server_settings_delete_tag_end;
    }

    if (base__is_tag_id_real(tag_id) == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_tag_end;
    }

    // strip the tag from every client that currently holds it, so server state stays consistent
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE)
        {
            continue;
        }

        tag_id_index = base__get_index_of_tag_id_of_client(client->client_id, tag_id);
        if (tag_id_index != -1)
        {
            cvector_erase(client->tag_ids, tag_id_index);
        }
    }

    // free the tag's pool slot (a tag id equals its index in g_tags_array)
    g_tags_array[tag_id].is_existing = FALSE;
    g_tags_array[tag_id].icon_id = 0;
    clib__null_memory(&g_tags_array[tag_id].name[0], TAG_MAX_NAME_LENGTH);

    server_msg__send_remove_tag_event_to_all_clients(tag_id);

label_client_msg__process_set_server_settings_delete_tag_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin asks for the identity-management list; replies with every stored identity (hash, its
 *        tags, and whether it is currently online). admin-only.
 *
 * @param cJSON* json_root -> the parsed request (no fields needed beyond type)
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__process_request_identity_list(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_request_identity_list_end;
    }

    server_msg__send_identity_list_to_single_client(sender_client->p_ws_connection, sender_client->dh_shared_secret);

label_client_msg__process_request_identity_list_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin-only, deletes one stored identity addressed by its public-key hash
 *
 *        the identity leaves the RAM store. if its holder is connected right now, their tags are
 *        stripped and admin is revoked on the live client, and each removal is broadcast one event
 *        at a time because there is no multi-tag-removal event. the on-disk copy is only dropped on
 *        the next "save server settings". the requesting admin then gets a refreshed identity list.
 *
 * @param cJSON* json_root -> the parsed request; message.public_key_hash identifies the identity
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__process_delete_identity(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    client_t* holder = 0;
    cJSON* json_message_object = 0;
    cJSON* json_hash = 0;
    char target_hash[BASE64_ENCODE_OUT_SIZE(32)];
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 tags_to_remove[MAX_TAGS_FOR_SINGLE_CLIENT];
    uint64 tags_to_remove_count = 0;
    uint64 holder_client_id = 0;
    uint64 c = 0;
    uint64 t = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");

    if (cJSON_IsString(json_hash) == FALSE || json_hash->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_identity: public_key_hash missing or not a string \n");
        return;
    }

    clib__null_memory(target_hash, sizeof(target_hash));
    clib__copy_memory(json_hash->valuestring, target_hash, clib__utf8_string_length(json_hash->valuestring), sizeof(target_hash) - 1);

    clib__write_lock(&g_clients_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_delete_identity_end;
    }

    // if this identity is worn by a connected client right now, strip every tag off them
    for (c = 0; c < g_server_settings.max_client_count; c++)
    {
        holder = &g_clients_array[c];

        if (holder->is_existing == FALSE || holder->is_authenticated == FALSE || holder->is_music_bot == TRUE)
        {
            continue;
        }
        if (holder->public_key[0] == 0)
        {
            continue;
        }

        base__hash_password_to_base64(holder->public_key, client_hash, sizeof(client_hash));
        if (clib__is_string_equal(client_hash, target_hash) == FALSE)
        {
            continue;
        }

        holder_client_id = holder->client_id;

        // snapshot the tag ids first, then clear them off the client and revoke admin
        tags_to_remove_count = 0;
        for (t = 0; t < cvector_size(holder->tag_ids) && tags_to_remove_count < MAX_TAGS_FOR_SINGLE_CLIENT; t++)
        {
            tags_to_remove[tags_to_remove_count] = (uint64)holder->tag_ids[t];
            tags_to_remove_count++;
        }

        while (cvector_size(holder->tag_ids) > 0)
        {
            cvector_erase(holder->tag_ids, 0);
        }
        holder->is_admin = FALSE;

        break;
    }

    // drop the identity from the ram store (disk copy stays until the next settings save)
    base__delete_identity_from_store_by_hash(target_hash);

    // tell everyone about each stripped tag (one event per tag; there is no bulk-removal event)
    for (t = 0; t < tags_to_remove_count; t++)
    {
        server_msg__send_remove_tag_from_client_event_to_all_clients(holder_client_id, tags_to_remove[t]);
    }

    // refresh the requesting admin's identity list so the deleted row disappears
    server_msg__send_identity_list_to_single_client(sender_client->p_ws_connection, sender_client->dh_shared_secret);

label_client_msg__process_delete_identity_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief admin-only, adds or removes a single tag on a stored identity addressed by its hash,
 *        working on offline identities too
 *
 *        the store is updated, and if the identity's holder is connected right now the tag is
 *        added or removed live on them and broadcast. persistence to disk waits for the next
 *        "save server settings". the requesting admin then gets a refreshed identity list.
 *
 * @param cJSON* json_root -> message.public_key_hash, message.tag_id, message.add (bool)
 * @param uint64 sender_client_id -> the requesting client
 *
 * @return void
 */
void client_msg__process_modify_identity_tag(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    client_t* holder = 0;
    cJSON* json_message_object = 0;
    cJSON* json_hash = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_add = 0;
    char target_hash[BASE64_ENCODE_OUT_SIZE(32)];
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 tag_id = 0;
    boole add = FALSE;
    uint64 c = 0;
    int64 tag_index = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "public_key_hash");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    json_add = cJSON_GetObjectItemCaseSensitive(json_message_object, "add");

    if (cJSON_IsString(json_hash) == FALSE || json_hash->valuestring == NULL_POINTER || cJSON_IsNumber(json_tag_id) == FALSE || cJSON_IsBool(json_add) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_modify_identity_tag: bad fields \n");
        return;
    }

    tag_id = (uint64)json_tag_id->valueint;
    add = (cJSON_IsTrue(json_add) == TRUE) ? TRUE : FALSE;

    clib__null_memory(target_hash, sizeof(target_hash));
    clib__copy_memory(json_hash->valuestring, target_hash, clib__utf8_string_length(json_hash->valuestring), sizeof(target_hash) - 1);

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_tags_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_modify_identity_tag_end;
    }

    // adding requires a real tag; the admin tag (0) is real and may be granted/revoked this way
    if (add == TRUE && base__is_tag_id_real(tag_id) == FALSE)
    {
        goto label_client_msg__process_modify_identity_tag_end;
    }

    // update the stored identity
    base__modify_identity_tag_in_store(target_hash, tag_id, add);

    // if the identity is connected right now, mirror the change on the live client + broadcast it
    for (c = 0; c < g_server_settings.max_client_count; c++)
    {
        holder = &g_clients_array[c];

        if (holder->is_existing == FALSE || holder->is_authenticated == FALSE || holder->is_music_bot == TRUE)
        {
            continue;
        }
        if (holder->public_key[0] == 0)
        {
            continue;
        }

        base__hash_password_to_base64(holder->public_key, client_hash, sizeof(client_hash));
        if (clib__is_string_equal(client_hash, target_hash) == FALSE)
        {
            continue;
        }

        tag_index = base__get_index_of_tag_id_of_client(holder->client_id, tag_id);

        if (add == TRUE)
        {
            if (tag_index == -1)
            {
                cvector_push_back(holder->tag_ids, (int)tag_id);
                if (tag_id == ADMIN_TAG_ID) { holder->is_admin = TRUE; }
                server_msg__send_add_tag_to_client_event_to_all_clients(holder->client_id, tag_id);
            }
        }
        else
        {
            if (tag_index != -1)
            {
                cvector_erase(holder->tag_ids, tag_index);
                if (tag_id == ADMIN_TAG_ID) { holder->is_admin = FALSE; }
                server_msg__send_remove_tag_from_client_event_to_all_clients(holder->client_id, tag_id);
            }
        }

        break;
    }

    // refresh the requesting admin's identity list
    server_msg__send_identity_list_to_single_client(sender_client->p_ws_connection, sender_client->dh_shared_secret);

label_client_msg__process_modify_identity_tag_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to delete an icon from the server's icon pool. tags that still reference the
 *        deleted icon keep their icon_id, but their image no longer resolves.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_server_settings_delete_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_icon_id = 0;
    uint64 icon_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_icon_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_set_server_settings_delete_icon: icon_id missing or not a number");
        return;
    }

    icon_id = (uint64)json_icon_id->valueint;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_icons_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_icon_end;
    }

    if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
    {
        goto label_client_msg__process_set_server_settings_delete_icon_end;
    }

    // free the icon's pool slot (an icon id equals its index in g_icons_array)
    g_icons_array[icon_id].is_existing = FALSE;
    clib__null_memory(&g_icons_array[icon_id].base64[0], ICON_MAX_LENGTH);

    server_msg__send_remove_icon_event_to_all_clients(icon_id);

label_client_msg__process_set_server_settings_delete_icon_end:
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to set or clear a tag's linked icon. an icon_id in the message assigns that
 *        icon (it must exist); an absent icon_id clears the tag's icon. the change is broadcast to all clients.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_server_settings_set_tag_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_tag_id = 0;
    cJSON* json_icon_id = 0;
    uint64 tag_id = 0;
    uint64 icon_id = 0;
    boole wants_icon = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_tag_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "tag_id");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_tag_id) == FALSE)
    {
        return;
    }

    tag_id = (uint64)json_tag_id->valueint;
    wants_icon = cJSON_IsNumber(json_icon_id);
    if (wants_icon == TRUE)
    {
        icon_id = (uint64)json_icon_id->valueint;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_tags_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_set_server_settings_set_tag_icon_end;
    }

    if (base__is_tag_id_real(tag_id) == FALSE)
    {
        goto label_client_msg__process_set_server_settings_set_tag_icon_end;
    }

    if (wants_icon == TRUE)
    {
        // the assigned icon must exist (an icon id equals its index)
        if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
        {
            goto label_client_msg__process_set_server_settings_set_tag_icon_end;
        }

        g_tags_array[tag_id].has_icon = TRUE;
        g_tags_array[tag_id].icon_id = icon_id;
    }
    else
    {
        // no icon_id in the request -> clear the tag's icon
        g_tags_array[tag_id].has_icon = FALSE;
        g_tags_array[tag_id].icon_id = 0;
    }

    server_msg__send_tag_icon_changed_event_to_all_clients(tag_id, g_tags_array[tag_id].has_icon, g_tags_array[tag_id].icon_id);

label_client_msg__process_set_server_settings_set_tag_icon_end:
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to set (or clear) a channel's icon. an icon_id in the request assigns
 *        that icon (which must exist); a request without icon_id clears the channel's icon. broadcasts the
 *        change so every client updates its channel row live. only an authenticated admin may do this.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_set_channel_icon(cJSON* json_root, uint64 sender_client_id)
{
    client_t* sender_client = 0;
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_icon_id = 0;
    uint64 channel_id = 0;
    uint64 icon_id = 0;
    boole wants_icon = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_icon_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_id");

    if (cJSON_IsNumber(json_channel_id) == FALSE)
    {
        return;
    }

    if (json_channel_id->valueint < 0 || json_channel_id->valueint >= g_server_settings.max_channel_count)
    {
        return;
    }

    channel_id = (uint64)json_channel_id->valueint;
    wants_icon = cJSON_IsNumber(json_icon_id);
    if (wants_icon == TRUE)
    {
        icon_id = (uint64)json_icon_id->valueint;
    }

    // lock order: clients -> channels -> icons
    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);

    sender_client = &g_clients_array[sender_client_id];

    if (sender_client->is_authenticated == FALSE || sender_client->is_existing == FALSE || sender_client->is_admin == FALSE)
    {
        goto label_client_msg__process_set_channel_icon_end;
    }

    if (g_channel_array[channel_id].is_existing == FALSE)
    {
        goto label_client_msg__process_set_channel_icon_end;
    }

    if (wants_icon == TRUE)
    {
        // the assigned icon must exist (an icon id equals its index)
        if (icon_id >= MAX_ICONS || g_icons_array[icon_id].is_existing == FALSE)
        {
            goto label_client_msg__process_set_channel_icon_end;
        }

        g_channel_array[channel_id].has_channel_icon = TRUE;
        g_channel_array[channel_id].icon_id = icon_id;
    }
    else
    {
        // no icon_id in the request -> clear the channel's icon
        g_channel_array[channel_id].has_channel_icon = FALSE;
        g_channel_array[channel_id].icon_id = 0;
    }

    server_msg__send_channel_icon_changed_event_to_all_clients(channel_id, g_channel_array[channel_id].has_channel_icon, g_channel_array[channel_id].icon_id);

label_client_msg__process_set_channel_icon_end:
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to apply and persist server settings, only an authenticated
 *        admin may do this
 *
 *        the general-settings toggles the admin sent (country flags, server-wide audio, music bot
 *        audio, hide-in-password-channels, temp channels) are applied to g_server_settings, each
 *        only when present and boolean. then everything persistable - those toggles plus the
 *        channel layout, the tags/icons and the identity snapshot - is written into
 *        server_settings.json. music bots and channel maintainers are runtime state and are never
 *        written. if the file cannot be written the toggles are rolled back to their previous
 *        values, so the running state stays consistent with what is on disk.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_save_server_settings_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permission_to_save_settings = FALSE;
    boole save_succeeded = FALSE;
    boole previous_display_country_flags = FALSE;
    boole previous_voice_chat_active = FALSE;
    boole previous_music_bot_audio_active = FALSE;
    boole previous_hide_clients_in_password_channels = FALSE;
    boole previous_temp_channel_creation_allowed = FALSE;
    cJSON* json_message_object = NULL_POINTER;
    cJSON* json_field = NULL_POINTER;

    status = _client_msg_internal__is_save_server_settings_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_save_server_settings_request is not valid");
        return;
    }

    // admin check only reads the sender's client slot, so a read lock is enough and it is released
    // before applying/persisting
    clib__read_lock(&g_clients_global_rwlock_guard);
    does_sender_have_permission_to_save_settings = util__is_client_valid_admin(sender_client_id);
    clib__unlock(&g_clients_global_rwlock_guard);

    if (does_sender_have_permission_to_save_settings == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_save_server_settings_request sender with sender_client_id", sender_client_id, "does not have permission to save server settings");
        return;
    }

    // snapshot the current toggles so they can be rolled back if the persist fails
    previous_display_country_flags = g_server_settings.is_display_country_flags_active;
    previous_voice_chat_active = g_server_settings.is_voice_chat_active;
    previous_music_bot_audio_active = g_server_settings.is_music_bot_audio_active;
    previous_hide_clients_in_password_channels = g_server_settings.is_hide_clients_in_password_protected_channels_active;
    previous_temp_channel_creation_allowed = g_server_settings.is_temp_channel_creation_allowed;

    // apply the general-settings toggles the admin sent (each only when present and boolean)
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "display_country_flags");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_display_country_flags_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "enable_audio");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_voice_chat_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "enable_music_bot_audio");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_music_bot_audio_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "hide_clients_in_password_channels");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_hide_clients_in_password_protected_channels_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "allow_typing_indicator");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.allow_typing_indicator = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "allow_temp_channels");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_temp_channel_creation_allowed = cJSON_IsTrue(json_field);
    }

    // persist everything into server_settings.json. the save reads channels, icons, tags and bans, so take
    // those read locks in lock order (bans is always last). the clients read lock is taken first (clients
    // before channels, matching the auth path) so the identity snapshot can read each client's tag list
    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_tags_global_rwlock_guard);
    clib__read_lock(&g_bans_global_rwlock_guard);

    base__snapshot_connected_clients_into_identity_store();
    save_succeeded = base__save_server_settings_to_file();

    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);

    // if the file could not be written, roll the toggles back so the running state stays consistent with
    // what is on disk (which is what the next restart will load)
    if (save_succeeded == FALSE)
    {
        g_server_settings.is_display_country_flags_active = previous_display_country_flags;
        g_server_settings.is_voice_chat_active = previous_voice_chat_active;
        g_server_settings.is_music_bot_audio_active = previous_music_bot_audio_active;
        g_server_settings.is_hide_clients_in_password_protected_channels_active = previous_hide_clients_in_password_channels;
        g_server_settings.is_temp_channel_creation_allowed = previous_temp_channel_creation_allowed;
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_save_server_settings_request: save failed, rolled back the general-settings toggles");
    }
}

/**
 * @brief processes an admin request to read the current server settings: replies to the sender with the
 *        current general-settings toggle values so the client can reflect them (e.g. tick the checkboxes
 *        when the admin opens the general-settings tab). only an authenticated admin gets a reply.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_load_server_settings_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permission_to_read_settings = FALSE;

    status = _client_msg_internal__is_save_server_settings_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_load_server_settings_request is not valid");
        return;
    }

    // the reply is sent under the clients read lock, which also covers reading the sender's slot
    clib__read_lock(&g_clients_global_rwlock_guard);

    does_sender_have_permission_to_read_settings = util__is_client_valid_admin(sender_client_id);
    if (does_sender_have_permission_to_read_settings == TRUE)
    {
        server_msg__send_server_settings_to_single_client(&g_clients_array[sender_client_id]);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_load_server_settings_request sender with sender_client_id", sender_client_id, "does not have permission to read server settings");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a request to call an idle client (notify them so they can come back)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_call_idle_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* caller = 0;
    client_t* callee = 0;

    cJSON* json_message_object = 0;
    cJSON* json_callee_client_id = 0;

    status = _client_msg_internal__is_call_idle_client_message_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_call_idle_client_message is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_callee_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    caller = &g_clients_array[sender_client_id];

    if (caller->is_authenticated == FALSE || caller->is_existing == FALSE)
    {
        goto label_client_msg__process_call_idle_client_message_end;
    }

    // check if he is idle too later
    callee = &g_clients_array[json_callee_client_id->valueint];

    if (callee->is_authenticated == FALSE || callee->is_existing == FALSE)
    {
        goto label_client_msg__process_call_idle_client_message_end;
    }

    server_msg__send_call_event_to_idle_client(caller, callee);

label_client_msg__process_call_idle_client_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes one reset_channel_maintainer vote, the last-resort recovery path for a channel
 *        whose announced maintainer never delivered usable channel keys
 *
 *        that happens with a faulty or modified client, or a connection that half-works. every
 *        keyless client keeps re-sending this vote every few seconds, and once MORE THAN HALF of
 *        the channel's clients have voted against the CURRENT maintainer generation, the server
 *        deposes the maintainer, picks a new one (excluding the deposed client), bumps the
 *        generation and broadcasts the new maintainer id - upon which the new maintainer's client
 *        distributes fresh channel keys exactly like after any other maintainer change.
 *
 *        the request carries NO payload - the client only says "i want the current maintainer of my
 *        channel replaced". all bookkeeping is server-internal: the vote is stamped with the
 *        channel's CURRENT maintainer_generation when it is recorded, so a vote recorded against a
 *        previous maintainer can never count against the newly appointed one, and votes die by
 *        themselves when the voter switches channel or the generation moves on - no vote cleanup
 *        anywhere else. music bots are skipped on both sides of the count, they never hold channel
 *        keys and never vote, so counting them would only inflate the quorum denominator.
 *
 * @param cJSON* json_root -> the parsed client request (unused beyond the already-checked type)
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_reset_channel_maintainer_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    uint64 channel_id = 0;
    channel_t* channel = 0;
    client_t* sender = 0;
    client_t* client_in_loop = 0;
    uint64 channel_member_count = 0;
    uint64 vote_count = 0;
    uint64 deposed_maintainer_id = 0;
    uint64 new_maintainer_index = 0;
    boole is_maintainer_found = FALSE;
    uint64 i = 0;

    // same per-client cooldown gate as every other request. the client re-votes every few seconds
    // for as long as it stays keyless, so a vote swallowed by the cooldown is simply retried
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_reset_channel_maintainer_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];

    if (sender->is_existing == FALSE || sender->is_authenticated == FALSE || sender->is_idle == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_reset_channel_maintainer_request is_existing == FALSE || is_authenticated == FALSE || is_idle == TRUE \n");
        goto label_reset_channel_maintainer_end;
    }

    // the vote always applies to the channel the sender is CURRENTLY in on the server's books;
    // the request carries no channel id, so a modified client cannot vote into someone else's channel
    channel_id = sender->channel_id;
    if (channel_id >= (uint64)g_server_settings.max_channel_count)
    {
        goto label_reset_channel_maintainer_end;
    }

    channel = &g_channel_array[channel_id];

    if (channel->is_existing == FALSE)
    {
        goto label_reset_channel_maintainer_end;
    }

    // nobody to depose
    if (channel->is_channel_maintainer_present == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_reset_channel_maintainer_request channel has no maintainer \n");
        goto label_reset_channel_maintainer_end;
    }

    // the maintainer cannot vote himself out
    if (channel->maintainer_id == sender_client_id)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_reset_channel_maintainer_request sender is the maintainer \n");
        goto label_reset_channel_maintainer_end;
    }

    // record the vote, stamped with the CURRENT generation - re-voting just overwrites the same
    // values. if the maintainer changes before quorum, the stamp stops matching and the vote is dead
    sender->has_pending_maintainer_reset_vote = TRUE;
    sender->maintainer_reset_vote_channel_id = channel_id;
    sender->maintainer_reset_vote_generation = channel->maintainer_generation;

    // count channel members and valid votes in one pass. music bots are skipped on both sides:
    // they never hold channel keys (bot audio is not channel-key encrypted) and never vote,
    // so counting them would only inflate the quorum denominator
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop->channel_id != channel_id)
        {
            continue;
        }

        channel_member_count++;

        if (client_in_loop->has_pending_maintainer_reset_vote == TRUE
            && client_in_loop->maintainer_reset_vote_channel_id == channel_id
            && client_in_loop->maintainer_reset_vote_generation == channel->maintainer_generation)
        {
            vote_count++;
        }
    }

    DBG_CLIENT_MESSAGE log_info("%s %llu %s %llu %s", "client_msg__process_reset_channel_maintainer_request votes ", vote_count, " of ", channel_member_count, "\n");

    // quorum rule: MORE THAN HALF of all clients present in the channel
    if ((vote_count * 2) <= channel_member_count)
    {
        goto label_reset_channel_maintainer_end;
    }

    deposed_maintainer_id = channel->maintainer_id;

    // pick a replacement, never handing the role right back to the client that was just voted out
    is_maintainer_found = base__find_new_maintainer_for_channel(&new_maintainer_index, channel_id, deposed_maintainer_id, TRUE);
    if (is_maintainer_found == FALSE)
    {
        // cannot really happen while at least one voter is present in the channel; leave state untouched
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_reset_channel_maintainer_request no replacement maintainer found \n");
        goto label_reset_channel_maintainer_end;
    }

    // consume the votes that carried this reset so they cannot count a second time
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->has_pending_maintainer_reset_vote == TRUE && client_in_loop->maintainer_reset_vote_channel_id == channel_id)
        {
            client_in_loop->has_pending_maintainer_reset_vote = FALSE;
        }
    }

    channel->maintainer_id = new_maintainer_index;
    channel->is_channel_maintainer_present = TRUE;
    channel->maintainer_generation++;

    // always-on log: a majority of a channel just declared its maintainer broken - the operator should see that
    log_info("%s %llu %s %llu %s %llu %s", "channel ", channel_id, ": maintainer reset by vote, deposed client ", deposed_maintainer_id, ", new maintainer client ", new_maintainer_index, "\n");

    // same announcement as any other maintainer change; the new maintainer's client reacts to it
    // by generating and distributing fresh channel keys to everyone in the channel
    server_msg__send_maintainer_id_to_clients_in_same_channel(channel_id, channel->maintainer_id);

label_reset_channel_maintainer_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes go to idle more request, its modified version of join channel request
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_go_to_idle_mode_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_client_that_is_leaving_channel_maintainer_of_that_channel = FALSE;
    channel_t* old_channel = 0;
    client_t* client = 0;
    uint64 new_maintainer_index = 0;
    boole is_maintainer_found = FALSE;
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    boole is_idle = FALSE;

    // this is modified version of join channel request
    // no password verification, no checking if idle channel has maintainer (it's not a channel really)
    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_internal__is_go_to_idle_mode_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    client = &g_clients_array[sender_client_id];

    is_authenticated = client->is_authenticated;
    is_existing = client->is_existing;
    is_idle = client->is_idle;

    if (is_existing == FALSE || is_authenticated == FALSE || is_idle == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request is_existing == FALSE || is_authenticated == FALSE || is_idle == TRUE \n");
        goto label_go_to_idle_mode_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request got here \n");

    old_channel = &g_channel_array[client->channel_id];

    // change channel id and idle state at this
    client->is_idle = TRUE;
    client->channel_id = -2; // -2 marks the client as being in idle mode rather than in a real channel
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

    if (old_channel->is_channel_maintainer_present == TRUE)
    {
        is_client_that_is_leaving_channel_maintainer_of_that_channel = (boole)(old_channel->maintainer_id == client->client_id);
    }

    if (is_client_that_is_leaving_channel_maintainer_of_that_channel == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request is_client_that_is_leaving_channel_maintainer_of_that_channel TRUE  \n");

        is_maintainer_found = base__find_new_maintainer_for_channel(&new_maintainer_index, old_channel->channel_id, sender_client_id, TRUE);
        if (is_maintainer_found == TRUE)
        {
            // client that left channel was maintainer of that channel, choose new maintainer
            // then broadcast channel join message
            // then send new maintainer id to clients in that channel so they know who new maintainer is
            DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__process_go_to_idle_mode_request maintainer found ", new_maintainer_index, "\n");
            old_channel->is_channel_maintainer_present = TRUE;
            old_channel->maintainer_id = new_maintainer_index;
            old_channel->maintainer_generation++;

            // first send join message, then maintainer message for clients in that channel
            server_msg__send_client_going_to_idle_mode_info_to_all_clients(sender_client_id);
            server_msg__send_maintainer_id_to_clients_in_same_channel(old_channel->channel_id, old_channel->maintainer_id);
        }
        else
        {
            DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request maintainer found  FALSE \n");
            old_channel->is_channel_maintainer_present = FALSE;
            old_channel->maintainer_id = 0;
            old_channel->maintainer_generation++;
            server_msg__send_client_going_to_idle_mode_info_to_all_clients(sender_client_id);
        }
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_go_to_idle_mode_request is_client_that_is_leaving_channel_maintainer_of_that_channel FALSE  \n");
        server_msg__send_client_going_to_idle_mode_info_to_all_clients(sender_client_id);
    }

    // the client closes its end of the datachannel right after sending this request; drop the server
    // side too - a slot left "connected" on a dead transport made the re-create request on resume be
    // refused until the ICE consent timeout noticed, tens of seconds without working audio
    audio_channel__process_client_disconnect(client);

    // the transport is gone, say so; the fresh peer built on resume re-announces the real state
    client->audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    server_msg__send_audio_state_of_client_to_all_clients(client->client_id, AUDIO_STATE__AUDIO_COMPLETELY_DISABLED);

label_go_to_idle_mode_request_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request to come back from idle mode into a channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_come_back_from_idle_mode_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    client_t* client = 0;
    channel_t* channel_to_join = 0;

    status = _client_msg_internal__is_process_come_back_from_idle_mode_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_come_back_from_idle_mode_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_client_msg__process_come_back_from_idle_mode_request_end;
    }

    channel_to_join = &g_channel_array[json_channel_id->valueint];

    if (channel_to_join->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_come_back_from_idle_mode_request channel not is_existing \n");
        goto label_client_msg__process_come_back_from_idle_mode_request_end;
    }

    client->is_idle = FALSE;
    client->channel_id = channel_to_join->channel_id;
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

    // keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this client on the
    // channel-mismatch check after it returns from idle
    audio_channel__process_client_channel_join(client);

    server_msg__send_client_coming_back_from_idle_mode_info_to_all_clients(sender_client_id, channel_to_join->channel_id);

    // parity with the normal join tail: alone in the channel means the returner IS its maintainer,
    // and the returner must always be told the maintainer id - without both it never received (or
    // never produced) channel keys until it manually switched channels
    if (base__get_client_count_for_channel(channel_to_join->channel_id) == 1)
    {
        channel_to_join->maintainer_id = client->client_id;
        channel_to_join->is_channel_maintainer_present = TRUE;
        channel_to_join->maintainer_generation++;
    }

    server_msg__send_maintainer_id_to_single_client(client, channel_to_join->channel_id, channel_to_join->maintainer_id);

    server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client->p_ws_connection, client->dh_shared_secret, client->channel_id);

label_client_msg__process_come_back_from_idle_mode_request_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request to open a new WebRTC datachannel connection for voice
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_create_new_webrtc_datachannel_connection(cJSON* json_root, uint64 sender_client_id)
{
    client_t* client = 0;
    webrtc_peer_t* peer = 0;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here

    // log_info("%s", "client_msg__process_create_new_webrtc_datachannel_connection");
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    // server will ignore the request only if these 3 are off
    peer = &g_webrtc_muggles_array[sender_client_id];

    if (peer->is_existing == TRUE && peer->connected == TRUE && client->audio_state != AUDIO_STATE__AUDIO_COMPLETELY_DISABLED)
    {
        goto label_process_create_new_webrtc_datachannel_connection_end;
    }

    // log_info("%s", "attempting reconnect");
    audio_channel__initialize_webrtc_datachannel_connection(client);

label_process_create_new_webrtc_datachannel_connection_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to kick a client from the server
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_kick_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    client_t* admin = 0;
    client_t* receiver = 0;

    status = _client_msg_internal__is_kick_ban_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_kick_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        goto label_client_msg__process_kick_request_end;
    }

    receiver = &g_clients_array[json_client_id->valueint];
    if (receiver->is_authenticated == FALSE || receiver->is_existing == FALSE)
    {
        goto label_client_msg__process_kick_request_end;
    }

    ws_close_client(receiver->p_ws_connection);

label_client_msg__process_kick_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to ban a client from the server
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_ban_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    client_t* admin = 0;
    client_t* receiver = 0;
    boole should_ban = FALSE;
    char banned_ip[BAN_IP_MAX_LENGTH];
    char banned_country[COUNTRY_ISO_CODE_LENGTH];
    char banned_identity[MAX_PUBLIC_KEY_LENGTH];

    clib__null_memory(banned_ip, sizeof(banned_ip));
    clib__null_memory(banned_country, sizeof(banned_country));
    clib__null_memory(banned_identity, sizeof(banned_identity));

    status = _client_msg_internal__is_kick_ban_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_ban_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        goto label_client_msg__process_ban_request_end;
    }

    receiver = &g_clients_array[json_client_id->valueint];
    if (receiver->is_authenticated == FALSE || receiver->is_existing == FALSE)
    {
        goto label_client_msg__process_ban_request_end;
    }

    // snapshot the target's identifying data while we hold the clients lock, then disconnect them
    clib__copy_memory(&receiver->ip_address[0], banned_ip, clib__utf8_string_length(&receiver->ip_address[0]), BAN_IP_MAX_LENGTH - 1);
    clib__copy_memory(&receiver->country_iso_code[0], banned_country, clib__utf8_string_length(&receiver->country_iso_code[0]), COUNTRY_ISO_CODE_LENGTH - 1);
    clib__copy_memory(&receiver->public_key[0], banned_identity, clib__utf8_string_length(&receiver->public_key[0]), MAX_PUBLIC_KEY_LENGTH - 1);
    should_ban = TRUE;

    ws_close_client(receiver->p_ws_connection);

label_client_msg__process_ban_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);

    // record + persist the ban outside the clients lock. lock order puts bans last, so take channels,
    // icons and tags (read, for the save) before the bans write lock
    if (should_ban == TRUE)
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        clib__read_lock(&g_icons_global_rwlock_guard);
        clib__read_lock(&g_tags_global_rwlock_guard);
        clib__write_lock(&g_bans_global_rwlock_guard);

        base__add_ban(banned_ip, banned_country, banned_identity, "");
        base__save_server_settings_to_file();

        clib__unlock(&g_bans_global_rwlock_guard);
        clib__unlock(&g_tags_global_rwlock_guard);
        clib__unlock(&g_icons_global_rwlock_guard);
        clib__unlock(&g_channels_global_rwlock_guard);
    }
}

/**
 * @brief processes an admin request to remove one ban (by ip address) from the persisted ban list
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_remove_ban_request(cJSON* json_root, uint64 sender_client_id)
{
    cJSON* json_message_object = 0;
    cJSON* json_ip = 0;
    boole is_admin = FALSE;
    boole removed = FALSE;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_ip = cJSON_GetObjectItemCaseSensitive(json_message_object, "ip_address");
    if (cJSON_IsString(json_ip) == FALSE || json_ip->valuestring == NULL_POINTER)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);
    is_admin = (boole)(g_clients_array[sender_client_id].is_authenticated == TRUE
        && g_clients_array[sender_client_id].is_existing == TRUE
        && g_clients_array[sender_client_id].is_admin == TRUE);
    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_admin == FALSE)
    {
        return;
    }

    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_tags_global_rwlock_guard);
    clib__write_lock(&g_bans_global_rwlock_guard);

    removed = base__remove_ban_by_ip(json_ip->valuestring);
    if (removed == TRUE)
    {
        base__save_server_settings_to_file();
    }

    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_tags_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
}

/**
 * @brief processes an admin request to read another client's info (connected time, ip, country, identity).
 *        only an admin gets a reply; a non-admin's request is silently ignored ("nothing happens for now")
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_get_client_info_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;
    client_t* admin = 0;
    client_t* target = 0;

    status = _client_msg_internal__is_kick_ban_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_get_client_info_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__read_lock(&g_clients_global_rwlock_guard);

    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        goto label_client_msg__process_get_client_info_request_end;
    }

    target = &g_clients_array[json_client_id->valueint];
    if (target->is_authenticated == FALSE || target->is_existing == FALSE)
    {
        goto label_client_msg__process_get_client_info_request_end;
    }

    server_msg__send_client_info_to_single_client(admin, target);

label_client_msg__process_get_client_info_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to create a music bot client in a channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_create_music_bot_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    cJSON* json_music_bot_username = 0;

    client_t* admin = 0;
    client_t* music_bot = 0;
    int64 new_music_bot_index = 0;

    status = _client_msg_internal__is_client_msg__process_create_music_bot_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_music_bot is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");
    json_music_bot_username = cJSON_GetObjectItemCaseSensitive(json_message_object, "music_bot_username");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        server_msg__send_access_denied_to_single_client(admin);
        goto label_client_msg__process_create_music_bot_end;
    }

    // check if channel exists
    if (g_channel_array[json_channel_id->valueint].is_existing == FALSE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

#ifndef MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL
    // one music bot per channel. define MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL (definitions.h)
    // to lift this limit - debug aid only: several bots give several simultaneous audio senders for
    // testing multi-speaker mixing without needing several people. everything downstream (per-bot frame
    // ids, per-bot decoders, delete-all-bots-in-channel) works in both modes
    if (g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel == TRUE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }
#endif

    // music bots are not allowed in temp channels
    if (g_channel_array[json_channel_id->valueint].is_temp_channel == TRUE)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

    // load mp3 file on server side from disk. don't upload it yet, that will on on the end

    // first create client
    // assign him some random keys (client will be music client, messaging to him won't be possible), he will get channel keys but he won't get public key
    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_create_music_bot_request \n");

    new_music_bot_index = base__get_new_index_for_client();

    DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__process_create_music_bot_request", new_music_bot_index, "\n");

    if (new_music_bot_index < 0)
    {
        goto label_client_msg__process_create_music_bot_end;
    }

    g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel = TRUE;

    music_bot = &g_clients_array[new_music_bot_index];

    music_bot->is_authenticated = TRUE;
    music_bot->timestamp_connected = base__get_timestamp_ms();
    music_bot->p_ws_connection = NULL_POINTER;
    music_bot->is_existing = TRUE;
    music_bot->client_id = new_music_bot_index;
    music_bot->audio_state = AUDIO_STATE__PUSH_TO_TALK_ACTIVE;
    music_bot->is_music_bot = TRUE;
    music_bot->timestamp_connected = base__get_timestamp_ms();
    music_bot->is_streaming_song = TRUE;
    music_bot->channel_id = json_channel_id->valueint;

    DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__process_create_music_bot_request client id is -> ", music_bot->client_id, "\n");
    DBG_CLIENT_MESSAGE log_info("%s %p %s", "client_msg__process_create_music_bot_request client id is -> ", (void*)&music_bot->client_id, "\n");

    clib__null_memory(music_bot->username, USERNAME_MAX_LENGTH);
    clib__copy_memory(json_music_bot_username->valuestring, music_bot->username, clib__utf8_string_length(json_music_bot_username->valuestring), USERNAME_MAX_LENGTH);

    server_msg__send_client_connect_message_to_all_clients(music_bot->client_id);

    music_bot->music_bot_client_extension.is_music_bot_running = TRUE;
    pthread_create((pthread_t*)&music_bot->music_bot_client_extension.music_bot_pthread_handle, 0, (void*)&musicbot__threadstart, (void*)music_bot);

label_client_msg__process_create_music_bot_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes an admin request to delete a music bot from its channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_delete_music_bot_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_channel_id = 0;
    client_t* music_bot = 0;
    client_t* admin = 0;
    uint64 bot_loop_index = 0;

    status = _client_msg_internal__is_client_msg__process_delete_music_bot_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_music_bot_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_channel_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_id");

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within write lock like here
    admin = &g_clients_array[sender_client_id];

    if (admin->is_authenticated == FALSE || admin->is_existing == FALSE || admin->is_admin == FALSE)
    {
        server_msg__send_access_denied_to_single_client(admin);
        goto label_client_msg__process_delete_music_bot_end;
    }

    // check if channel exists
    if (g_channel_array[json_channel_id->valueint].is_existing == FALSE)
    {
        goto label_client_msg__process_delete_music_bot_end;
    }

    // check if there is at least one music bot in the channel
    if (g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel == FALSE)
    {
        goto label_client_msg__process_delete_music_bot_end;
    }

    // tear down EVERY music bot in the channel (there can be several since multi-bot was allowed).
    // nothing is freed here: musicbot__begin_delete only signals the bot's stream thread and hides the
    // bot; a detached reaper thread joins the stream thread and THEN frees the songs and the slot.
    // freeing inline here was a use-after-free against the stream/preload threads, and nulling the
    // client_t made the slot reusable while the old bot thread still wrote into it
    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_music_bot_request \n");

    for (bot_loop_index = 0; bot_loop_index < g_server_settings.max_client_count; bot_loop_index++)
    {
        music_bot = &g_clients_array[bot_loop_index];

        if (music_bot->is_existing == FALSE || music_bot->is_music_bot == FALSE || music_bot->channel_id != (uint64)json_channel_id->valueint)
        {
            continue;
        }

        DBG_CLIENT_MESSAGE log_info("%s %lld %s", "client_msg__process_delete_music_bot_request music bot id -> ", music_bot->client_id, "\n");
        DBG_CLIENT_MESSAGE log_info("%s %s %s", "client_msg__process_delete_music_bot_request music bot username -> ", music_bot->username, "\n");

        server_msg__send_client_disconnect_message_to_all_clients(music_bot->client_id);

        musicbot__begin_delete(music_bot);
    }

    g_channel_array[json_channel_id->valueint].is_music_bot_active_in_channel = FALSE;

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_delete_music_bot_request END \n");

label_client_msg__process_delete_music_bot_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief receives file from client, but keeps it in memory, this function doenst know what to do with it
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_file_send_request(cJSON* json_root, uint64 sender_client_id)
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
    status = _client_msg_internal__is_client_msg_file_send_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_file_send_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    client_sender = &g_clients_array[sender_client_id];

    if (client_sender->is_authenticated == FALSE || client_sender->is_existing == FALSE)
    {
        DBG_FILE_UPLOAD log_info("%s", "client->is_authenticated == FALSE || client->is_existing == FALSE end");
        goto label_client_msg__process_file_send_request_end;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_message_total_length = cJSON_GetObjectItemCaseSensitive(json_message_object, "total_bytes_length");
    json_message_is_new_file = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_new_file");
    json_message_data_part_base64 = cJSON_GetObjectItemCaseSensitive(json_message_object, "data_part_base64");

    // if appending another chunk could overflow the upload buffer, force this part to start a fresh file
    if ((client_sender->file_upload_extension.buffer_cursor + MAX_CLIENT_FILE_UPLOAD_LENGTH / 400) > MAX_CLIENT_FILE_UPLOAD_LENGTH)
    {
        log_info("%s %llu %s", "upload restarted: buffer full at cursor", client_sender->file_upload_extension.buffer_cursor, "\n");
        json_message_is_new_file->valueint = 1;
    }

    // a new file (re)allocates or clears the upload buffer and resets the cursor; a continuation appends to it
    if (json_message_is_new_file->valueint == 1)
    {
        DBG_FILE_UPLOAD log_info("%s", "json_message_is_new_file->valueint == 1");

        if (client_sender->file_upload_extension.file_upload_buffer == NULL_POINTER)
        {
            client_sender->file_upload_extension.file_upload_buffer = (ubyte*)memorymanager__allocate(MAX_CLIENT_FILE_UPLOAD_LENGTH, MEMALLOC_FILE_UPLOAD_BY_PARTS);
        }
        else
        {
            clib__null_memory((void*)client_sender->file_upload_extension.file_upload_buffer, MAX_CLIENT_FILE_UPLOAD_LENGTH);
        }

        client_sender->file_upload_extension.buffer_cursor = 0;
        client_sender->file_upload_extension.expected_file_length = json_message_total_length->valueint;
    }
    else
    {
        if (client_sender->file_upload_extension.expected_file_length != json_message_total_length->valueint)
        {
            DBG_FILE_UPLOAD log_info("%s", "expected length of file and what user is sending is not the same, aborting upload of this file");

            if (client_sender->file_upload_extension.file_upload_buffer == NULL_POINTER)
            {
                client_sender->file_upload_extension.file_upload_buffer = (ubyte*)memorymanager__allocate(MAX_CLIENT_FILE_UPLOAD_LENGTH, MEMALLOC_FILE_UPLOAD_BY_PARTS);
            }

            client_sender->file_upload_extension.buffer_cursor = 0;
            client_sender->file_upload_extension.expected_file_length = 0;

            goto label_client_msg__process_file_send_request_end;
        }
    }

    // append this base64 chunk at the cursor, then advance the cursor by its length
    data_part_length = clib__utf8_string_length(json_message_data_part_base64->valuestring);

    // never let the accumulated upload exceed the client-declared length: bounds per-client buffer
    // growth and keeps the accumulated base64 consistent with what was declared
    if (client_sender->file_upload_extension.buffer_cursor + data_part_length > client_sender->file_upload_extension.expected_file_length)
    {
        DBG_FILE_UPLOAD log_info("%s", "upload chunk would exceed declared file length, aborting upload");
        client_sender->file_upload_extension.buffer_cursor = 0;
        client_sender->file_upload_extension.expected_file_length = 0;
        goto label_client_msg__process_file_send_request_end;
    }

    clib__copy_memory(json_message_data_part_base64->valuestring, client_sender->file_upload_extension.file_upload_buffer + client_sender->file_upload_extension.buffer_cursor, data_part_length, MAX_CLIENT_FILE_UPLOAD_LENGTH / 400);

    client_sender->file_upload_extension.buffer_cursor += data_part_length;

    DBG_FILE_UPLOAD log_info("%s %llu %s", "client_msg__process_file_send_request client_sender->file_upload_extension.buffer_cursor", client_sender->file_upload_extension.buffer_cursor, "\n");
    DBG_FILE_UPLOAD log_info("%s %d %s", "client_msg__process_file_send_request total bytes length", json_message_total_length->valueint, "\n");

    if (client_sender->file_upload_extension.buffer_cursor == json_message_total_length->valueint)
    {
        server_msg__send_file_send_completed_status_to_single_client(client_sender);
    }

label_client_msg__process_file_send_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a client's request for a music bot's current song list
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_musicbot_get_song_list_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_musicbot_id = 0;
    client_t* client = 0;
    client_t* music_bot = 0;

    status = _client_msg_internal__is_musicbot_get_song_list_request_valid(json_root);
    if (status == FALSE)
    {
        log_info("%s", "client_msg__process_musicbot_get_song_list_request is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    clib__read_lock(&g_clients_global_rwlock_guard);

    // check if client that sent the message is valid. If he is connected and he exists.
    // this was checked before but not within read lock like here
    client = &g_clients_array[sender_client_id];

    if (client->is_authenticated == FALSE || client->is_existing == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client->is_authenticated == FALSE || client->is_existing == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    if (client->is_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client->is_admin == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        server_msg__send_access_denied_to_single_client(client);
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    music_bot = &g_clients_array[json_musicbot_id->valueint];
    if (music_bot->is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "music_bot->is_music_bot == FALSE goto label_client_msg__process_musicbot_get_song_list_request_end end");
        goto label_client_msg__process_musicbot_get_song_list_request_end;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__send_music_bot_song_list_to_single_client");

    server_msg__send_music_bot_song_list_to_single_client(sender_client_id, music_bot->client_id);

label_client_msg__process_musicbot_get_song_list_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief tells server what to do with uploaded file. currently implemented only for mp3s for music bots, plan to do it for channel and direct images
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_file_send_completed_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_file_send_intent = 0;
    cJSON* json_file_send_intent_extra_data = 0;

    cJSON* json_musicbot_id = 0;
    client_t* sender_client = 0;
    client_t* music_bot = 0;
    cJSON* json_song_name = 0;
    void* mp3_data_buffer = NULL_POINTER;
    uint64 mp3_data_buffer_length = 0;
    musicbot_add_song_arg_struct_t* arguments = NULL_POINTER;
    uint64 thread_id = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;

    status = _client_msg_internal__is_file_send_completed_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_file_send_completed_request is not valid");
        return;
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_file_send_completed_request is valid");
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
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_file_send_completed_request: no upload in progress, ignoring");
        goto label_client_msg__process_file_send_completed_request_end;
    }

    // not all parts arrived
    if (sender_client->file_upload_extension.expected_file_length == 0
        || sender_client->file_upload_extension.buffer_cursor != sender_client->file_upload_extension.expected_file_length)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_file_send_completed_request: upload incomplete, discarding");
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

        // decode base64 mp3 data to normal mp3 data
        // free the base64 mp3 data
        // pass the normal mp3 data to musicbot__add_song
        // music bot is responsible for freeing the final mp3 data buffer when it gets deleted
        // size the decode output from the ACTUAL base64 length, never the client-declared expected_file_length:
        // base64 decodes to ~3/4 of its length, so a buffer of the base64 length always holds the result and cannot overflow
        mp3_data_buffer = (void*)memorymanager__allocate(clib__utf8_string_length(sender_client->file_upload_extension.file_upload_buffer) + 1, MEMALLOC_MUSICBOT_SONG);

        if (mp3_data_buffer == NULL_POINTER)
        {
            goto label_client_msg__process_file_send_completed_request_end;
        }

        mp3_data_buffer_length = zchg_base64_decode(sender_client->file_upload_extension.file_upload_buffer, clib__utf8_string_length(sender_client->file_upload_extension.file_upload_buffer), mp3_data_buffer);

        // decoder returns 0 and writes nothing on bad base64
        if (mp3_data_buffer_length == 0)
        {
            DBG_CLIENT_MESSAGE log_info("%s", "musicbot_file: base64 decode failed, discarding upload");
            memorymanager__free((nuint)mp3_data_buffer);
            goto label_client_msg__process_file_send_completed_request_end;
        }

        // copy of song name must be initialized here because by the time the add_music_bot thread gets to it
        // main thread deletes the json holding the song name
        arguments = (musicbot_add_song_arg_struct_t*)memorymanager__allocate(sizeof(musicbot_add_song_arg_struct_t), MEMALLOC_MUSICBOT_SONG);
        clib__null_memory(arguments, sizeof(musicbot_add_song_arg_struct_t));

        arguments->music_bot = music_bot;
        arguments->mp3_data_buffer_length = mp3_data_buffer_length;
        arguments->mp3_data_buffer = mp3_data_buffer;
        arguments->sender_client_id = sender_client_id;
        clib__copy_memory(json_song_name->valuestring, arguments->song_name, clib__utf8_string_length(json_song_name->valuestring), SONG_NAME_MAX_LENGTH - 1);

        pthread_create((pthread_t*)&thread_id, 0, (void*)&musicbot__add_song, arguments);
    }
    else if (clib__is_string_equal(json_file_send_intent->valuestring, "direct_chat_picture_file") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "receiver_id");
        json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "local_message_id");
        client_msg__process_direct_chat_picture(sender_client_id, json_receiver_id->valueint, json_local_message_id->valueint, sender_client->file_upload_extension.file_upload_buffer);
        DBG_CLIENT_MESSAGE log_info("%s", "direct_chat_picture_file success");
    }
    else if (clib__is_string_equal(json_file_send_intent->valuestring, "channel_chat_picture_file") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "receiver_id");
        json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_file_send_intent_extra_data, "local_message_id");
        client_msg__process_channel_chat_picture(sender_client_id, json_local_message_id->valueint, sender_client->file_upload_extension.file_upload_buffer);
    }

    // no matter what the reason for calling client_msg__process_file_send_completed_request was
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

/**
 * @brief processes an admin request to remove a song from a music bot and refreshes the song list
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__process_remove_song_from_music_bot_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;

    cJSON* json_message_object = 0;
    cJSON* json_song_id = 0;
    cJSON* json_musicbot_id = 0;

    client_t* sender_client = 0;
    client_t* music_bot = 0;

    status = _client_msg_internal__is_remove_song_from_music_bot_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_remove_song_from_music_bot_request is not valid");
        return;
    }

    clib__write_lock(&g_clients_global_rwlock_guard);

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_song_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "song_id");
    json_musicbot_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "musicbot_id");

    sender_client = &g_clients_array[sender_client_id];
    music_bot = &g_clients_array[json_musicbot_id->valueint];

    if (music_bot->is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "not music bot");
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }

    if (sender_client->is_existing == FALSE || sender_client->is_authenticated == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "not valid client");
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }

    if (sender_client->is_admin == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "sender is not admin");
        server_msg__send_access_denied_to_single_client(sender_client);
        goto label_client_msg__process_remove_song_from_music_bot_request_end;
    }

    // questionable thread safety
    musicbot__remove_song(music_bot, json_song_id->valueint);

    server_msg__send_music_bot_song_list_to_single_client(sender_client_id, music_bot->client_id);

    if (music_bot->music_bot_client_extension.music_bot_songs_count == 0)
    {
        server_msg__send_stop_song_stream_message_to_clients_in_same_channel(music_bot);
    }

label_client_msg__process_remove_song_from_music_bot_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief resends encrypted files in chunks to client. receive because the end user receives the file, not this server.
 *
 * @param data_for_file_send_thread_t* arg -> the file-send thread arguments holding the buffer, sender/receiver ids and message ids
 *
 * @return void
 */
static void _client_msg_internal__file_download_thread(data_for_file_send_thread_t* arg)
{
    uint64 parts_count = 400;
    uint64 chunk_size = 0;
    uint64 offset = 0;
    boole is_sender_valid = FALSE;
    boole is_receiver_valid = FALSE;
    uint64 client_receiver_id = 0;
    uint64 remaining = 0;
    uint64 current_size = 0;
    char* chunk = NULL_POINTER;
    uint64 i = 0;

    if (arg->send_type == FILE_SEND_TYPE_TO_CLIENT)
    {
        // Compute chunk size (ceil division)
        chunk_size = (arg->size + parts_count - 1) / parts_count;

        // modify so it loops through clients based on their count
        while (offset < arg->size)
        {
            remaining = arg->size - offset;
            current_size = remaining < chunk_size ? remaining : chunk_size;
            chunk = 0;

            chunk = malloc(current_size + 1);
            clib__copy_memory(arg->buffer + offset, chunk, current_size, current_size + 1);
            chunk[current_size] = '\0';

            clib__read_lock(&g_clients_global_rwlock_guard);

            is_sender_valid = util__is_client_valid_and_not_music_bot(arg->client_sender_id);
            is_receiver_valid = util__is_client_valid_and_not_music_bot(arg->client_receiver_id);

            if (is_sender_valid == TRUE && is_receiver_valid == TRUE)
            {
                server_msg__send_file_by_chunk_to_single_client(chunk, current_size, arg->client_sender_id, arg->client_receiver_id, arg->server_chat_message_id);
            }

            clib__unlock(&g_clients_global_rwlock_guard);

            clib__null_memory(chunk, current_size + 1);
            free(chunk);

            offset += current_size;
        }

        clib__read_lock(&g_clients_global_rwlock_guard);

        is_sender_valid = util__is_client_valid_and_not_music_bot(arg->client_sender_id);
        is_receiver_valid = util__is_client_valid_and_not_music_bot(arg->client_receiver_id);

        if (is_sender_valid == TRUE && is_receiver_valid == TRUE)
        {
            server_msg__send_file_receive_completed_to_single_client(arg, arg->client_receiver_id, "direct_chat_picture");
            server_msg__send_image_status_to_single_client(&g_clients_array[arg->client_sender_id], "success"); // this is for client that sent it
            server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(arg->client_sender_id, arg->server_chat_message_id, arg->local_chat_message_id);
        }

        clib__unlock(&g_clients_global_rwlock_guard);

        memorymanager__free((nuint)arg->buffer);
        memorymanager__free((nuint)arg);
    }
    else if (arg->send_type == FILE_SEND_TYPE_TO_CHANNEL)
    {
        // Compute chunk size (ceil division)
        chunk_size = (arg->size + parts_count - 1) / parts_count;

        // modify so it loops through clients based on their count
        while (offset < arg->size)
        {
            remaining = arg->size - offset;
            current_size = remaining < chunk_size ? remaining : chunk_size;
            chunk = 0;

            chunk = malloc(current_size + 1);
            clib__copy_memory(arg->buffer + offset, chunk, current_size, current_size + 1);
            chunk[current_size] = '\0';

            for (i = 0; i < arg->receiving_clients_count; i++)
            {
                clib__read_lock(&g_clients_global_rwlock_guard);

                client_receiver_id = arg->receiving_client_ids[i];
                is_sender_valid = util__is_client_valid_and_not_music_bot(arg->client_sender_id);
                is_receiver_valid = util__is_client_valid_and_not_music_bot(client_receiver_id);

                if (is_sender_valid == TRUE && is_receiver_valid == TRUE)
                {
                    server_msg__send_file_by_chunk_to_single_client(chunk, current_size, arg->client_sender_id, client_receiver_id, arg->server_chat_message_id);
                }

                clib__unlock(&g_clients_global_rwlock_guard);

                // base__sleep_for_milliseconds(10);
            }

            clib__null_memory(chunk, current_size + 1);
            free(chunk);

            offset += current_size;
        }

        for (i = 0; i < arg->receiving_clients_count; i++)
        {
            clib__read_lock(&g_clients_global_rwlock_guard);

            client_receiver_id = arg->receiving_client_ids[i];

            is_sender_valid = util__is_client_valid_and_not_music_bot(arg->client_sender_id);
            is_receiver_valid = util__is_client_valid_and_not_music_bot(client_receiver_id);

            if (is_sender_valid == TRUE && is_receiver_valid == TRUE)
            {
                server_msg__send_file_receive_completed_to_single_client(arg, client_receiver_id, "channel_chat_picture");
                server_msg__send_image_status_to_single_client(&g_clients_array[arg->client_sender_id], "success"); // this is for client that sent it
                server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(arg->client_sender_id, arg->server_chat_message_id, arg->local_chat_message_id);
            }

            clib__unlock(&g_clients_global_rwlock_guard);
        }

        memorymanager__free((nuint)arg->buffer);
        memorymanager__free((nuint)arg);
    }
}
