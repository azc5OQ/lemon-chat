// This file checks requests to view or change server settings, server information and the server banner.
#include "client_msg.h"
#include "../../core/permissions.h"
#include <string.h>
#include "../../core/channel.h"
#include "../../core/identity_store.h"
#include "../../core/administration.h"
#include "../../core/login_admission.h"
#include "../../core/server_settings.h"
#include "../server/server_msg.h"
#include "../../core/server_logs.h"
#include "../../core/audio_channel.h"
#include "../../video_websocket.h"
#include "../../core/video_channel.h"
#include "../../base.h"
#include "../../util.h"
#include "../../memory_manager.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"

// declarations
static boole _client_msg_server_settings_internal__is_save_server_settings_request_valid(cJSON* json_root);
static boole _client_msg_server_settings_internal__is_server_info_requester(uint64 sender_client_id, ws_cli_conn_t* socket);

/**
 * @brief validates a save-server-settings request: it carries no payload beyond message.type,
 *        which the upstream envelope/type check already verified
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole -> always TRUE
 */
static boole _client_msg_server_settings_internal__is_save_server_settings_request_valid(cJSON* json_root)
{
    return TRUE;
}

/**
 * @brief processes an admin request to apply and persist server settings, only an authenticated
 *        admin may do this
 *
 *        the general-settings toggles the admin sent (country flags, server-wide audio, music bot
 *        audio, hide-in-password-channels, temp channels) are applied to g_server_settings, each
 *        only when present and boolean. then everything persistable - those toggles plus the
 *        channel layout, the groups/icons and the identity snapshot - is written into
 *        server_settings.json. music bots and channel maintainers are runtime state and are never
 *        written. if the file cannot be written the toggles are rolled back to their previous
 *        values, so the running state stays consistent with what is on disk.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__server_settings__process_save_request(cJSON* json_root, uint64 sender_client_id)
{
    uint64 now;
    uint64 i;
    boole status = FALSE;
    boole does_sender_have_permission_to_save_settings = FALSE;
    boole save_succeeded = FALSE;
    boole previous_is_display_country_flags_active = FALSE;
    boole previous_voice_chat_active = FALSE;
    boole previous_is_audio_over_websocket_allowed = FALSE;
    boole previous_is_audio_over_webrtc_allowed = FALSE;
    boole previous_music_bot_audio_active = FALSE;
    boole previous_video_streaming_active = FALSE;
    boole previous_is_hide_clients_in_password_protected_channels_active = FALSE;
    boole previous_temp_channel_creation_allowed = FALSE;
    boole previous_is_admin_country_flag_hidden = FALSE;
    boole previous_is_connection_check_enforced = TRUE;
    boole previous_is_channel_key_reuse_enabled = TRUE;
    boole previous_is_channel_key_reuse_on_join_allowed = TRUE;
    boole previous_is_channel_key_reuse_on_leave_allowed = TRUE;
    int64 previous_channel_key_reuse_join_min_clients = 50;
    int64 previous_channel_key_reuse_leave_min_clients = 100;
    boole previous_is_server_uptime_published = TRUE;
    boole previous_is_server_slots_published = TRUE;
    boole previous_is_server_banner_published = TRUE;
    boole previous_is_server_features_published = TRUE;
    int64 previous_max_concurrent_logins = 4;
    int64 previous_avatar_max_size_bytes = 0;
    boole previous_is_blocking_direct_chat_pictures_active = FALSE;
    boole previous_is_blocking_direct_chat_files_active = FALSE;
    boole previous_is_blocking_channel_chat_pictures_active = FALSE;
    boole previous_is_blocking_channel_chat_files_active = FALSE;
    cJSON* json_message_object = NULL_POINTER;
    cJSON* json_field = NULL_POINTER;
    cJSON* json_country_entry = NULL_POINTER;

    // for the admin log line; captured under the same read lock as the admin check
    char sender_username[USERNAME_MAX_LENGTH];
    sender_username[0] = 0;

    status = _client_msg_server_settings_internal__is_save_server_settings_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__server_settings__process_save_request is not valid");
        return;
    }

    // Serialize permission checks, settings changes and persistence with other
    // admin saves and ban updates. Nested snapshot locks follow the normal order.
    clib__write_lock(&g_clients_global_rwlock_guard);
    does_sender_have_permission_to_save_settings = base__is_client_valid_admin(sender_client_id);
    if (does_sender_have_permission_to_save_settings == TRUE)
    {
        snprintf(sender_username, sizeof(sender_username), "%s", g_clients_array[sender_client_id].username);
    }

    if (does_sender_have_permission_to_save_settings == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__server_settings__process_save_request sender with sender_client_id", sender_client_id, "does not have permission to save server settings");
        return;
    }

    // snapshot the current toggles so they can be rolled back if the persist fails
    previous_is_display_country_flags_active = g_server_settings.is_display_country_flags_active;
    previous_voice_chat_active = g_server_settings.is_voice_chat_active;
    previous_is_audio_over_websocket_allowed = g_server_settings.is_audio_over_websocket_allowed;
    previous_is_audio_over_webrtc_allowed = g_server_settings.is_audio_over_webrtc_allowed;
    previous_music_bot_audio_active = g_server_settings.is_music_bot_audio_active;
    previous_video_streaming_active = g_server_settings.is_video_streaming_active;
    previous_is_hide_clients_in_password_protected_channels_active = g_server_settings.is_hide_clients_in_password_protected_channels_active;
    previous_temp_channel_creation_allowed = g_server_settings.is_temp_channel_creation_allowed;
    previous_is_connection_check_enforced = g_server_settings.is_connection_check_enforced;
    previous_is_channel_key_reuse_enabled = g_server_settings.is_channel_key_reuse_enabled;
    previous_is_channel_key_reuse_on_join_allowed = g_server_settings.is_channel_key_reuse_on_join_allowed;
    previous_is_channel_key_reuse_on_leave_allowed = g_server_settings.is_channel_key_reuse_on_leave_allowed;
    previous_channel_key_reuse_join_min_clients = g_server_settings.channel_key_reuse_join_min_clients;
    previous_channel_key_reuse_leave_min_clients = g_server_settings.channel_key_reuse_leave_min_clients;
    previous_max_concurrent_logins = g_server_settings.max_concurrent_logins;
    previous_avatar_max_size_bytes = g_server_settings.avatar_max_size_bytes;
    previous_is_blocking_direct_chat_pictures_active = g_server_settings.is_blocking_direct_chat_pictures_active;
    previous_is_blocking_direct_chat_files_active = g_server_settings.is_blocking_direct_chat_files_active;
    previous_is_blocking_channel_chat_pictures_active = g_server_settings.is_blocking_channel_chat_pictures_active;
    previous_is_blocking_channel_chat_files_active = g_server_settings.is_blocking_channel_chat_files_active;
    previous_is_server_uptime_published = g_server_settings.is_server_uptime_published;
    previous_is_server_slots_published = g_server_settings.is_server_slots_published;
    previous_is_server_banner_published = g_server_settings.is_server_banner_published;
    previous_is_server_features_published = g_server_settings.is_server_features_published;

    // the change check after the save: a toggled value re-sends the admins' shown country codes
    previous_is_admin_country_flag_hidden = g_server_settings.is_admin_country_flag_hidden;

    // apply the general-settings toggles the admin sent (each only when present and boolean)
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    settings__read_attachment_receive_policy(json_message_object);
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_server_uptime_published");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_server_uptime_published = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_server_slots_published");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_server_slots_published = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_server_banner_published");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_server_banner_published = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_server_features_published");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_server_features_published = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_display_country_flags_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_display_country_flags_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_voice_chat_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_voice_chat_active = cJSON_IsTrue(json_field);
    }

    // the audio connections the admin allows (the select under the audio switch): both, websocket or webrtc
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "audio_connections");
    if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
    {
        if (clib__is_string_equal(json_field->valuestring, "both") == TRUE)
        {
            g_server_settings.is_audio_over_websocket_allowed = TRUE;
            g_server_settings.is_audio_over_webrtc_allowed = TRUE;
        }
        else if (clib__is_string_equal(json_field->valuestring, "websocket") == TRUE)
        {
            g_server_settings.is_audio_over_websocket_allowed = TRUE;
            g_server_settings.is_audio_over_webrtc_allowed = FALSE;
        }
        else if (clib__is_string_equal(json_field->valuestring, "webrtc") == TRUE)
        {
            g_server_settings.is_audio_over_websocket_allowed = FALSE;
            g_server_settings.is_audio_over_webrtc_allowed = TRUE;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_music_bot_audio_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_music_bot_audio_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_video_streaming_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_video_streaming_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_hide_clients_in_password_protected_channels_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_hide_clients_in_password_protected_channels_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_typing_indicator_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_typing_indicator_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_client_rename_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_client_rename_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_link_preview_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_link_preview_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_sending_text_to_idle_clients_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_sending_text_to_idle_clients_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_private_messaging_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_private_messaging_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_temp_channel_creation_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_temp_channel_creation_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_file_upload_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_file_upload_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "avatar_max_size_kb");
    if (cJSON_IsNumber(json_field) && json_field->valueint >= 1 && json_field->valueint <= 90)
    {
        g_server_settings.avatar_max_size_bytes = (int64)json_field->valueint * 1024;
    }

    // The settings tab sends whole megabytes; keep the value within the supported range.
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "file_upload_max_size_mb");
    if (cJSON_IsNumber(json_field) && json_field->valueint > 0)
    {
        g_server_settings.file_upload_max_size_bytes = (int64)json_field->valueint * 1024 * 1024;

        if (g_server_settings.file_upload_max_size_bytes < FILE_UPLOAD_MIN_SIZE_BYTES)
        {
            g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MIN_SIZE_BYTES;
        }
        else if (g_server_settings.file_upload_max_size_bytes > FILE_UPLOAD_MAX_SIZE_BYTES)
        {
            g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MAX_SIZE_BYTES;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_chat_picture_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_chat_picture_allowed = cJSON_IsTrue(json_field);
    }

    // sent in whole megabytes, same as the file limit
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "chat_picture_max_size_mb");
    if (cJSON_IsNumber(json_field) && json_field->valueint > 0)
    {
        g_server_settings.chat_picture_max_size_bytes = (int64)json_field->valueint * 1024 * 1024;

        if (g_server_settings.chat_picture_max_size_bytes < PICTURE_MIN_SIZE_BYTES)
        {
            g_server_settings.chat_picture_max_size_bytes = PICTURE_MIN_SIZE_BYTES;
        }
        else if (g_server_settings.chat_picture_max_size_bytes > PICTURE_MAX_SIZE_BYTES)
        {
            g_server_settings.chat_picture_max_size_bytes = PICTURE_MAX_SIZE_BYTES;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_same_ip_address_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_same_ip_address_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_admin_country_flag_hidden");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_admin_country_flag_hidden = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_fast_reconnect_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_fast_reconnect_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_identity_takeover_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_identity_takeover_allowed = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_websocket_ping_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_websocket_ping_active = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_channel_key_reuse_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_channel_key_reuse_enabled = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_channel_key_reuse_on_join_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_channel_key_reuse_on_join_allowed = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_channel_key_reuse_on_leave_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_channel_key_reuse_on_leave_allowed = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_key_reuse_join_min_clients");
    if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 30 && json_field->valuedouble <= 1000000)
    {
        g_server_settings.channel_key_reuse_join_min_clients = (int64)json_field->valuedouble;
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "channel_key_reuse_leave_min_clients");
    if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 30 && json_field->valuedouble <= 1000000)
    {
        g_server_settings.channel_key_reuse_leave_min_clients = (int64)json_field->valuedouble;
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_connection_check_enforced");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_connection_check_enforced = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "max_concurrent_logins");
    if (cJSON_IsNumber(json_field))
    {
        g_server_settings.max_concurrent_logins = json_field->valueint;
        if (g_server_settings.max_concurrent_logins < 1)
        {
            g_server_settings.max_concurrent_logins = 1;
        }
        if (g_server_settings.max_concurrent_logins > 64)
        {
            g_server_settings.max_concurrent_logins = 64;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "webrtc_datachannel_cooldown_seconds");
    if (cJSON_IsNumber(json_field))
    {
        g_server_settings.webrtc_datachannel_cooldown_seconds = (int64)json_field->valuedouble;
        if (g_server_settings.webrtc_datachannel_cooldown_seconds < 0)
        {
            g_server_settings.webrtc_datachannel_cooldown_seconds = 0;
        }
        if (g_server_settings.webrtc_datachannel_cooldown_seconds > 86400)
        {
            g_server_settings.webrtc_datachannel_cooldown_seconds = 86400;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "icon_max_size_bytes");
    if (cJSON_IsNumber(json_field))
    {
        g_server_settings.icon_max_size_bytes = (int64)json_field->valuedouble;
        if (g_server_settings.icon_max_size_bytes < 1000)
        {
            g_server_settings.icon_max_size_bytes = 1000;
        }
        if (g_server_settings.icon_max_size_bytes > 30000)
        {
            g_server_settings.icon_max_size_bytes = 30000;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_music_bot_marquee_visible_to_everyone");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_music_bot_marquee_visible_to_everyone = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "minimum_rsa_key_bits");
    if (cJSON_IsNumber(json_field))
    {
        g_server_settings.minimum_rsa_key_bits = (int64)json_field->valuedouble;
        if (g_server_settings.minimum_rsa_key_bits < 2048)
        {
            g_server_settings.minimum_rsa_key_bits = 2048;
        }
        if (g_server_settings.minimum_rsa_key_bits > 8192)
        {
            g_server_settings.minimum_rsa_key_bits = 8192;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_minimum_rsa_key_bits_announced");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_minimum_rsa_key_bits_announced = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_country_blocking_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_country_blocking_active = cJSON_IsTrue(json_field);
    }

    // the block list is replaced as a whole; entries must look like iso codes (2 letters, any case)
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "blocked_countries");
    if (cJSON_IsArray(json_field))
    {
        g_server_settings.blocked_countries_count = 0;

        cJSON_ArrayForEach(json_country_entry, json_field)
        {
            if (g_server_settings.blocked_countries_count >= MAX_BLOCKED_COUNTRIES)
            {
                break;
            }

            if (util__normalize_country_code(json_country_entry, &g_server_settings.blocked_countries[g_server_settings.blocked_countries_count][0]) == TRUE)
            {
                g_server_settings.blocked_countries_count++;
            }
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_client_joins_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_client_joins_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_username_changes_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_username_changes_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_group_changes_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_group_changes_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_server_settings_updates_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_server_settings_updates_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_kicks_and_bans_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_kicks_and_bans_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_client_disconnects_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_client_disconnects_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_socket_opens_and_closes_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_socket_opens_and_closes_enabled = cJSON_IsTrue(json_field);
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "is_logging_failed_attempts_enabled");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_logging_failed_attempts_enabled = cJSON_IsTrue(json_field);
    }

    // sent in whole megabytes
    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "admin_log_max_size_mb");
    if (cJSON_IsNumber(json_field) && json_field->valueint > 0)
    {
        g_server_settings.admin_log_max_size_bytes = (int64)json_field->valueint * 1024 * 1024;

        if (g_server_settings.admin_log_max_size_bytes < ADMIN_LOG_MIN_SIZE_BYTES)
        {
            g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MIN_SIZE_BYTES;
        }
        else if (g_server_settings.admin_log_max_size_bytes > ADMIN_LOG_MAX_SIZE_BYTES)
        {
            g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MAX_SIZE_BYTES;
        }
    }

    json_field = cJSON_GetObjectItemCaseSensitive(json_message_object, "admin_log_retention_days");
    if (cJSON_IsNumber(json_field) && json_field->valueint > 0)
    {
        g_server_settings.admin_log_retention_days = json_field->valueint;

        if (g_server_settings.admin_log_retention_days > ADMIN_LOG_MAX_RETENTION_DAYS)
        {
            g_server_settings.admin_log_retention_days = ADMIN_LOG_MAX_RETENTION_DAYS;
        }
    }

    // video streaming switched off: every running stream ends now, before the clients hear the new
    // policy and hide their stream ui (the stop needs the write locks the save below does not)
    if (previous_video_streaming_active == TRUE && g_server_settings.is_video_streaming_active == FALSE)
    {
        clib__write_lock(&g_clients_global_rwlock_guard);
        clib__write_lock(&g_channels_global_rwlock_guard);
        video_channel__stop_all("disabled");
        video_websocket__disconnect_all();
        clib__unlock(&g_channels_global_rwlock_guard);
        clib__unlock(&g_clients_global_rwlock_guard);
    }

    // persist everything into server_settings.json. the save reads channels, icons, groups and bans, so take
    // those read locks in lock order (bans is always last). the clients read lock is taken first (clients
    // before channels, matching the auth path) so the identity snapshot can read each client's group list
    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_channels_global_rwlock_guard);
    clib__read_lock(&g_icons_global_rwlock_guard);
    clib__read_lock(&g_groups_global_rwlock_guard);
    clib__read_lock(&g_bans_global_rwlock_guard);

    identity_store__snapshot_connected_clients_into_identity_store();
    save_succeeded = settings__save_server_settings_to_file();

    clib__unlock(&g_bans_global_rwlock_guard);
    clib__unlock(&g_groups_global_rwlock_guard);
    clib__unlock(&g_icons_global_rwlock_guard);
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);

    // if the file could not be written, roll the toggles back so the running state stays consistent with
    // what is on disk (which is what the next restart will load)
    if (save_succeeded == FALSE)
    {
        g_server_settings.is_display_country_flags_active = previous_is_display_country_flags_active;
        g_server_settings.is_voice_chat_active = previous_voice_chat_active;
        g_server_settings.is_audio_over_websocket_allowed = previous_is_audio_over_websocket_allowed;
        g_server_settings.is_audio_over_webrtc_allowed = previous_is_audio_over_webrtc_allowed;
        g_server_settings.is_music_bot_audio_active = previous_music_bot_audio_active;
        g_server_settings.is_video_streaming_active = previous_video_streaming_active;
        g_server_settings.is_hide_clients_in_password_protected_channels_active = previous_is_hide_clients_in_password_protected_channels_active;
        g_server_settings.is_temp_channel_creation_allowed = previous_temp_channel_creation_allowed;
        g_server_settings.is_connection_check_enforced = previous_is_connection_check_enforced;
        g_server_settings.is_channel_key_reuse_enabled = previous_is_channel_key_reuse_enabled;
        g_server_settings.is_channel_key_reuse_on_join_allowed = previous_is_channel_key_reuse_on_join_allowed;
        g_server_settings.is_channel_key_reuse_on_leave_allowed = previous_is_channel_key_reuse_on_leave_allowed;
        g_server_settings.channel_key_reuse_join_min_clients = previous_channel_key_reuse_join_min_clients;
        g_server_settings.channel_key_reuse_leave_min_clients = previous_channel_key_reuse_leave_min_clients;
        g_server_settings.max_concurrent_logins = previous_max_concurrent_logins;
        g_server_settings.avatar_max_size_bytes = previous_avatar_max_size_bytes;
        g_server_settings.is_blocking_direct_chat_pictures_active = previous_is_blocking_direct_chat_pictures_active;
        g_server_settings.is_blocking_direct_chat_files_active = previous_is_blocking_direct_chat_files_active;
        g_server_settings.is_blocking_channel_chat_pictures_active = previous_is_blocking_channel_chat_pictures_active;
        g_server_settings.is_blocking_channel_chat_files_active = previous_is_blocking_channel_chat_files_active;
        g_server_settings.is_server_uptime_published = previous_is_server_uptime_published;
        g_server_settings.is_server_slots_published = previous_is_server_slots_published;
        g_server_settings.is_server_banner_published = previous_is_server_banner_published;
        g_server_settings.is_server_features_published = previous_is_server_features_published;
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__server_settings__process_save_request: save failed, rolled back the general-settings toggles");
    }

    // a connection switched off, or audio switched off, is ended for the clients that use it, before they learn the new policy
    if (save_succeeded == TRUE && (previous_is_audio_over_websocket_allowed != g_server_settings.is_audio_over_websocket_allowed || previous_is_audio_over_webrtc_allowed != g_server_settings.is_audio_over_webrtc_allowed || previous_voice_chat_active != g_server_settings.is_voice_chat_active || previous_music_bot_audio_active != g_server_settings.is_music_bot_audio_active))
    {
        audio_channel__apply_policy_change();
    }

    // connected clients learn policy changes right away, not only on their next join
    if (save_succeeded == TRUE)
    {
        login_admission__set_limit((uint)g_server_settings.max_concurrent_logins);

        // A client may have been quiet for a long time while the checks were disabled.
        // Start a full new waiting period when the admin enables them again.
        if (previous_is_connection_check_enforced == FALSE && g_server_settings.is_connection_check_enforced == TRUE)
        {
            now = util__get_timestamp_ms();
            for (i = 0; i < g_server_settings.max_client_count; i++)
            {
                if (g_clients_array[i].is_authenticated == TRUE)
                {
                    g_clients_array[i].timestamp_last_maintain_connection_message_received = now;
                }
            }
        }

        clib__read_lock(&g_clients_global_rwlock_guard);
        server_msg__server_settings__send_policy_update_to_all_clients();
        server_msg__server_settings__send_info_to_all_clients((boole)(previous_is_server_banner_published != g_server_settings.is_server_banner_published));

        // admins' flags appear or vanish for everyone at once; the clients only see the codes, never the setting
        if (previous_is_admin_country_flag_hidden != g_server_settings.is_admin_country_flag_hidden)
        {
            server_msg__profile__send_admin_country_codes_to_all_clients();
        }

        clib__unlock(&g_clients_global_rwlock_guard);
    }

    server_msg__server_settings__send_saved(&g_clients_array[sender_client_id], save_succeeded);
    server_logs__server_settings_updated(sender_username, save_succeeded);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks that an information request still belongs to this authenticated connection.
 * @param uint64 sender_client_id -> client slot that sent the request
 * @param ws_cli_conn_t* socket -> socket that delivered the request
 * @return boole -> TRUE for the same authenticated human client
 * @attention The caller holds the clients lock.
 */
static boole _client_msg_server_settings_internal__is_server_info_requester(uint64 sender_client_id, ws_cli_conn_t* socket)
{
    return (boole)(sender_client_id < (uint64)g_server_settings.max_client_count && g_clients_array[sender_client_id].is_existing == TRUE && g_clients_array[sender_client_id].is_authenticated == TRUE && g_clients_array[sender_client_id].is_music_bot == FALSE && g_clients_array[sender_client_id].p_ws_connection == socket);
}

/**
 * @brief Replies with public server information, including for users in idle mode.
 * @param cJSON* json_root -> request with an optional include_banner boolean
 * @param uint64 sender_client_id -> client that requested the information
 * @param ws_cli_conn_t* socket -> socket that delivered the request
 * @return void
 */
void client_msg__server_settings__process_info_request(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket)
{
    cJSON* message = NULL_POINTER;
    cJSON* json_include_banner = NULL_POINTER;
    boole include_banner = TRUE;

    message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_include_banner = cJSON_GetObjectItemCaseSensitive(message, "include_banner");
    if (json_include_banner != NULL_POINTER)
    {
        if (cJSON_IsBool(json_include_banner) == FALSE)
        {
            return;
        }
        include_banner = (boole)cJSON_IsTrue(json_include_banner);
    }
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (_client_msg_server_settings_internal__is_server_info_requester(sender_client_id, socket) == TRUE)
    {
        if (permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_VIEW_SERVER_INFO) == TRUE)
        {
            server_msg__server_settings__send_info_to_single_client(&g_clients_array[sender_client_id], include_banner, FALSE);
        }
        else
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Saves an administrator's banner change and tells clients when it succeeds.
 * @param cJSON* json_root -> request containing a data URL, or an empty banner to remove it
 * @param uint64 sender_client_id -> administrator that sent the request
 * @param ws_cli_conn_t* socket -> socket that delivered the request
 * @return void
 */
void client_msg__server_settings__process_set_banner_request(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket)
{
    cJSON* message = NULL_POINTER;
    cJSON* banner = NULL_POINTER;
    cstring error = "";
    char* replacement = NULL_POINTER;
    char* previous = NULL_POINTER;
    uint64 banner_length = 0;
    boole allowed = FALSE;
    boole saved = FALSE;

    message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    banner = cJSON_GetObjectItemCaseSensitive(message, "banner");
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (_client_msg_server_settings_internal__is_server_info_requester(sender_client_id, socket) == TRUE)
    {
        allowed = g_clients_array[sender_client_id].is_admin;
        if (allowed == FALSE)
        {
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
            server_msg__server_settings__send_banner_result_to_single_client(&g_clients_array[sender_client_id], FALSE, "not_admin");
        }
        else if (cJSON_IsString(banner) && banner->valuestring[0] != 0 && base__is_server_ram_budget_reached())
        {
            allowed = FALSE;
            server_msg__server_settings__send_banner_result_to_single_client(&g_clients_array[sender_client_id], FALSE, "server_ram_budget_reached");
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
    if (allowed == FALSE)
    {
        return;
    }

    if (cJSON_IsString(banner) == FALSE || server_admin__is_server_banner_valid(banner->valuestring) == FALSE)
    {
        error = "invalid_banner";
    }
    else if (banner->valuestring[0] != 0)
    {
        banner_length = strlen(banner->valuestring);
        replacement = (char*)memorymanager__allocate(banner_length + 1, MEMALLOC_SERVER_BANNER);
        if (replacement == NULL_POINTER)
        {
            error = "out_of_memory";
        }
        else
        {
            memcpy(replacement, banner->valuestring, banner_length + 1);
        }
    }

    clib__write_lock(&g_clients_global_rwlock_guard);
    if (_client_msg_server_settings_internal__is_server_info_requester(sender_client_id, socket) == TRUE)
    {
        if (g_clients_array[sender_client_id].is_admin == FALSE)
        {
            error = "not_admin";
            server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        }
        if (error[0] == 0)
        {
            previous = g_server_settings.server_banner;
            g_server_settings.server_banner = replacement;
            clib__read_lock(&g_channels_global_rwlock_guard);
            clib__read_lock(&g_icons_global_rwlock_guard);
            clib__read_lock(&g_groups_global_rwlock_guard);
            clib__read_lock(&g_bans_global_rwlock_guard);
            saved = settings__save_server_settings_to_file();
            clib__unlock(&g_bans_global_rwlock_guard);
            clib__unlock(&g_groups_global_rwlock_guard);
            clib__unlock(&g_icons_global_rwlock_guard);
            clib__unlock(&g_channels_global_rwlock_guard);
            if (saved == TRUE)
            {
                replacement = previous;
            }
            else
            {
                g_server_settings.server_banner = previous;
                error = "save_failed";
            }
        }
        server_msg__server_settings__send_banner_result_to_single_client(&g_clients_array[sender_client_id], saved, error);
        if (saved == TRUE)
        {
            server_msg__server_settings__send_banner_changed_to_all_clients();
        }
    }
    clib__unlock(&g_clients_global_rwlock_guard);
    if (replacement != NULL_POINTER)
    {
        memorymanager__free((nuint)replacement);
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
void client_msg__server_settings__process_load_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole does_sender_have_permission_to_read_settings = FALSE;

    status = _client_msg_server_settings_internal__is_save_server_settings_request_valid(json_root);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__server_settings__process_load_request is not valid");
        return;
    }

    // the reply is sent under the clients read lock, which also covers reading the sender's slot
    clib__read_lock(&g_clients_global_rwlock_guard);

    does_sender_have_permission_to_read_settings = base__is_client_valid_admin(sender_client_id);
    if (does_sender_have_permission_to_read_settings == TRUE)
    {
        server_msg__server_settings__send_to_single_client(&g_clients_array[sender_client_id], (boole)cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_root, "message"), "memory_only")));
    }
    else
    {
        server_msg__administration__send_access_denied_to_single_client(&g_clients_array[sender_client_id], NULL_POINTER);
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__server_settings__process_load_request sender with sender_client_id", sender_client_id, "does not have permission to read server settings");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}
