// This file sends server settings, public server information, banners and changes to server rules.

#include "server_msg.h"
#include "../../core/permissions.h"
#include "../../base.h"
#include "../../util.h"
#include "../../core/audio_channel.h"

// declarations
static void _server_msg_server_settings_internal__add_server_info(cJSON* message, boole include_banner, boole banner_changed);

/**
 * @brief Tells clients which attachment types they are allowed to block.
 * @param cJSON* message -> login, settings or policy message being built
 * @return void
 */
void server_msg__server_settings__add_attachment_receive_policy(cJSON* message)
{
    cJSON_AddBoolToObject(message, "is_blocking_direct_chat_pictures_active", g_server_settings.is_blocking_direct_chat_pictures_active);
    cJSON_AddBoolToObject(message, "is_blocking_direct_chat_files_active", g_server_settings.is_blocking_direct_chat_files_active);
    cJSON_AddBoolToObject(message, "is_blocking_channel_chat_pictures_active", g_server_settings.is_blocking_channel_chat_pictures_active);
    cJSON_AddBoolToObject(message, "is_blocking_channel_chat_files_active", g_server_settings.is_blocking_channel_chat_files_active);
}

/**
 * @brief replies to a single client with the current general-settings toggle values, so the client can
 *        reflect the live server state (used when an admin opens the general-settings tab)
 *
 * @param client_t* receiver -> authenticated administrator receiving the reply
 * @param boole memory_only -> TRUE refreshes RAM, slots and uptime without replacing edited settings
 *
 * @return void
 */
void server_msg__server_settings__send_to_single_client(client_t* receiver, boole memory_only)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    cJSON* json_bans = 0;
    cJSON* json_ban = 0;
    cJSON* json_blocked_countries = 0;
    ban_entry_t* ban_in_loop = 0;
    uint64 i = 0;
    uint64 used_slots = 0;
    uint64 ram_bytes = 0;
    uint64 webrtc_received = 0;
    uint64 webrtc_sent = 0;

    root = server_msg__delivery__new_message("server_settings_values", &message);
    base__get_webrtc_bytes(&webrtc_received, &webrtc_sent);
    cJSON_AddNumberToObject(message, "websocket_received_bytes", (double)ws_get_received_bytes());
    cJSON_AddNumberToObject(message, "websocket_sent_bytes", (double)ws_get_sent_bytes());
    cJSON_AddNumberToObject(message, "webrtc_received_bytes", (double)webrtc_received);
    cJSON_AddNumberToObject(message, "webrtc_sent_bytes", (double)webrtc_sent);
    if (util__get_process_ram_usage(&ram_bytes))
    {
        cJSON_AddNumberToObject(message, "ram_bytes", (double)ram_bytes);
    }
    else
    {
        cJSON_AddNullToObject(message, "ram_bytes");
    }
    cJSON_AddNumberToObject(message, "uptime_seconds", (double)base__get_server_uptime_seconds());
    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == TRUE || g_clients_array[i].timestamp_connected != 0)
        {
            used_slots++;
        }
    }
    cJSON_AddNumberToObject(message, "used_slots", (double)used_slots);
    cJSON_AddNumberToObject(message, "max_slots", (double)g_server_settings.max_client_count);
    if (memory_only)
    {
        cJSON_AddBoolToObject(message, "memory_only", TRUE);
        server_msg__delivery__send_json_to_client(receiver, root);
        cJSON_Delete(root);
        return;
    }
    server_msg__server_settings__add_attachment_receive_policy(message);
    cJSON_AddBoolToObject(message, "is_server_uptime_published", g_server_settings.is_server_uptime_published);
    cJSON_AddBoolToObject(message, "is_server_slots_published", g_server_settings.is_server_slots_published);
    cJSON_AddBoolToObject(message, "is_server_banner_published", g_server_settings.is_server_banner_published);
    cJSON_AddBoolToObject(message, "is_server_features_published", g_server_settings.is_server_features_published);
    cJSON_AddStringToObject(message, "server_banner", g_server_settings.server_banner != NULL_POINTER ? g_server_settings.server_banner : "");
    cJSON_AddItemToObject(message, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(message, "is_admin_country_flag_hidden", cJSON_CreateBool(g_server_settings.is_admin_country_flag_hidden == TRUE));
    cJSON_AddItemToObject(message, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddStringToObject(message, "audio_connections", audio_channel__policy_value());
    cJSON_AddItemToObject(message, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_AddItemToObject(message, "is_video_streaming_active", cJSON_CreateBool(g_server_settings.is_video_streaming_active == TRUE));
    cJSON_AddItemToObject(message, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(message, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_typing_indicator_allowed", cJSON_CreateBool(g_server_settings.is_typing_indicator_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_client_rename_allowed", cJSON_CreateBool(g_server_settings.is_client_rename_allowed == TRUE));
    cJSON_AddBoolToObject(message, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed);
    cJSON_AddItemToObject(message, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_private_messaging_allowed", cJSON_CreateBool(g_server_settings.is_private_messaging_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_AddBoolToObject(message, "is_permission_system_enabled", g_server_settings.is_permission_system_enabled);
    cJSON_AddBoolToObject(message, "is_stored_client_list_allowed", g_server_settings.is_identity_storage_enabled && g_server_settings.is_stored_client_list_allowed);
    cJSON_AddBoolToObject(message, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed);
    cJSON_AddNumberToObject(message, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients);
    cJSON_AddNumberToObject(message, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients);
    cJSON_AddNumberToObject(message, "max_concurrent_logins", (double)g_server_settings.max_concurrent_logins);
    cJSON_AddNumberToObject(message, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds);
    cJSON_AddNumberToObject(message, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes);
    cJSON_AddNumberToObject(message, "avatar_max_size_kb", (double)(g_server_settings.avatar_max_size_bytes / 1024));
    cJSON_AddItemToObject(message, "is_music_bot_marquee_visible_to_everyone", cJSON_CreateBool(g_server_settings.is_music_bot_marquee_visible_to_everyone == TRUE));
    cJSON_AddNumberToObject(message, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_AddItemToObject(message, "is_minimum_rsa_key_bits_announced", cJSON_CreateBool(g_server_settings.is_minimum_rsa_key_bits_announced == TRUE));
    cJSON_AddItemToObject(message, "is_file_upload_allowed", cJSON_CreateBool(g_server_settings.is_file_upload_allowed == TRUE));
    cJSON_AddNumberToObject(message, "file_upload_max_size_mb", (double)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)));
    cJSON_AddNumberToObject(message, "chat_picture_max_size_mb", (double)(g_server_settings.chat_picture_max_size_bytes / (1024 * 1024)));
    cJSON_AddItemToObject(message, "is_chat_picture_allowed", cJSON_CreateBool(g_server_settings.is_chat_picture_allowed == TRUE));
    cJSON_AddItemToObject(message, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    json_blocked_countries = cJSON_CreateArray();
    cJSON_AddItemToObject(message, "blocked_countries", json_blocked_countries);
    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        cJSON_AddItemToArray(json_blocked_countries, cJSON_CreateString(&g_server_settings.blocked_countries[i][0]));
    }
    cJSON_AddItemToObject(message, "is_logging_client_joins_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_joins_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_username_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_username_changes_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_group_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_group_changes_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_server_settings_updates_enabled", cJSON_CreateBool(g_server_settings.is_logging_server_settings_updates_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_kicks_and_bans_enabled", cJSON_CreateBool(g_server_settings.is_logging_kicks_and_bans_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_client_disconnects_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_disconnects_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_socket_opens_and_closes_enabled", cJSON_CreateBool(g_server_settings.is_logging_socket_opens_and_closes_enabled == TRUE));
    cJSON_AddItemToObject(message, "is_logging_failed_attempts_enabled", cJSON_CreateBool(g_server_settings.is_logging_failed_attempts_enabled == TRUE));
    cJSON_AddNumberToObject(message, "admin_log_max_size_mb", (double)(g_server_settings.admin_log_max_size_bytes / (1024 * 1024)));
    cJSON_AddNumberToObject(message, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);

    // include the current ban list so the admin's bans section can render it
    json_bans = cJSON_CreateArray();
    cJSON_AddItemToObject(message, "bans", json_bans);
    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        ban_in_loop = &g_ban_array[i];
        if (ban_in_loop->is_existing == FALSE)
        {
            continue;
        }
        json_ban = cJSON_CreateObject();
        cJSON_AddStringToObject(json_ban, "ip_address", &ban_in_loop->ip_address[0]);
        cJSON_AddStringToObject(json_ban, "country_iso_code", &ban_in_loop->country_iso_code[0]);
        cJSON_AddStringToObject(json_ban, "identity", &ban_in_loop->identity[0]);
        cJSON_AddStringToObject(json_ban, "extra_data", &ban_in_loop->extra_data[0]);
        cJSON_AddNumberToObject(json_ban, "timestamp_banned", (double)ban_in_loop->timestamp_banned);
        cJSON_AddItemToArray(json_bans, json_ban);
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Adds only the server information the administrator has chosen to share.
 * @param cJSON* message -> message object to fill
 * @param boole include_banner -> include the saved banner when TRUE
 * @param boole banner_changed -> ask visible clients to refresh their banner when TRUE
 * @return void
 * @attention The caller holds the clients lock while reading settings and slots.
 */
static void _server_msg_server_settings_internal__add_server_info(cJSON* message, boole include_banner, boole banner_changed)
{
    cJSON* visibility = NULL_POINTER;
    cJSON* features = NULL_POINTER;
    uint64 used_slots = 0;
    uint64 i = 0;

    visibility = cJSON_AddObjectToObject(message, "visibility");
    cJSON_AddBoolToObject(visibility, "uptime", g_server_settings.is_server_uptime_published);
    cJSON_AddBoolToObject(visibility, "slots", g_server_settings.is_server_slots_published);
    cJSON_AddBoolToObject(visibility, "banner", g_server_settings.is_server_banner_published);
    cJSON_AddBoolToObject(visibility, "features", g_server_settings.is_server_features_published);
    if (g_server_settings.is_server_uptime_published == TRUE)
    {
        cJSON_AddNumberToObject(message, "uptime_seconds", (double)base__get_server_uptime_seconds());
    }
    if (g_server_settings.is_server_slots_published == TRUE)
    {
        for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
        {
            if (g_clients_array[i].is_existing == TRUE || g_clients_array[i].timestamp_connected != 0)
            {
                used_slots++;
            }
        }
        cJSON_AddNumberToObject(message, "used_slots", (double)used_slots);
        cJSON_AddNumberToObject(message, "max_slots", (double)g_server_settings.max_client_count);
    }
    if (g_server_settings.is_server_banner_published == TRUE && include_banner == TRUE)
    {
        cJSON_AddStringToObject(message, "server_banner", g_server_settings.server_banner != NULL_POINTER ? g_server_settings.server_banner : "");
    }
    if (g_server_settings.is_server_banner_published == TRUE && banner_changed == TRUE)
    {
        cJSON_AddBoolToObject(message, "banner_changed", TRUE);
    }
    if (g_server_settings.is_server_features_published == FALSE)
    {
        return;
    }

    // Only public feature switches belong here. Passwords, keys, bans and
    // administrator-only settings must stay in their existing private replies.
    features = cJSON_AddObjectToObject(message, "features");
    cJSON_AddBoolToObject(features, "is_voice_chat_active", g_server_settings.is_voice_chat_active);
    cJSON_AddBoolToObject(features, "is_music_bot_audio_active", g_server_settings.is_music_bot_audio_active);
    cJSON_AddBoolToObject(features, "is_video_streaming_allowed", g_server_settings.is_video_streaming_active);
    cJSON_AddBoolToObject(features, "is_audio_over_websocket_allowed", g_server_settings.is_audio_over_websocket_allowed);
    cJSON_AddBoolToObject(features, "is_audio_over_webrtc_allowed", g_server_settings.is_audio_over_webrtc_allowed);
    cJSON_AddBoolToObject(features, "is_idle_mode_allowed", g_server_settings.is_idle_mode_allowed);
    cJSON_AddBoolToObject(features, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
    cJSON_AddBoolToObject(features, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced);
    cJSON_AddBoolToObject(features, "is_typing_indicator_allowed", g_server_settings.is_typing_indicator_allowed);
    cJSON_AddBoolToObject(features, "is_client_rename_allowed", g_server_settings.is_client_rename_allowed);
    cJSON_AddBoolToObject(features, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed);
    cJSON_AddBoolToObject(features, "is_avatar_upload_allowed", g_server_settings.is_avatar_upload_allowed);
    cJSON_AddBoolToObject(features, "is_private_messaging_allowed", g_server_settings.is_private_messaging_allowed);
    cJSON_AddBoolToObject(features, "is_file_upload_allowed", g_server_settings.is_file_upload_allowed);
    cJSON_AddBoolToObject(features, "is_chat_picture_allowed", g_server_settings.is_chat_picture_allowed);
    cJSON_AddBoolToObject(features, "is_temp_channel_creation_allowed", g_server_settings.is_permission_system_enabled || g_server_settings.is_temp_channel_creation_allowed);
    cJSON_AddBoolToObject(features, "is_identity_storage_enabled", g_server_settings.is_identity_storage_enabled);
    cJSON_AddBoolToObject(features, "is_stored_client_list_allowed", g_server_settings.is_identity_storage_enabled && g_server_settings.is_stored_client_list_allowed);
    cJSON_AddBoolToObject(features, "is_last_seen_enabled", g_server_settings.is_identity_storage_enabled && g_server_settings.is_last_seen_enabled);
    cJSON_AddBoolToObject(features, "is_offline_messaging_allowed", g_server_settings.is_identity_storage_enabled && g_server_settings.is_stored_client_list_allowed && g_server_settings.is_offline_messaging_allowed);
    cJSON_AddBoolToObject(features, "is_display_country_flags_active", g_server_settings.is_display_country_flags_active);
    cJSON_AddBoolToObject(features, "is_hide_clients_in_password_protected_channels_active", g_server_settings.is_hide_clients_in_password_protected_channels_active);
    cJSON_AddBoolToObject(features, "is_sending_text_to_idle_clients_allowed", g_server_settings.is_sending_text_to_idle_clients_allowed);
    cJSON_AddBoolToObject(features, "is_music_bot_marquee_visible_to_everyone", g_server_settings.is_music_bot_marquee_visible_to_everyone);
}

/**
 * @brief Sends public server information to an authenticated client.
 * @param client_t* receiver -> client that requested the information
 * @param boole include_banner -> include the saved banner when TRUE
 * @param boole banner_changed -> tell the client its cached banner needs refreshing
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__server_settings__send_info_to_single_client(client_t* receiver, boole include_banner, boole banner_changed)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("server_info", &message);
    if (permissions__is_allowed(receiver, PERMISSION_VIEW_SERVER_INFO) == TRUE)
    {
        _server_msg_server_settings_internal__add_server_info(message, include_banner, banner_changed);
    }
    else
    {
        cJSON_AddItemToObject(message, "visibility", cJSON_CreateObject());
    }
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief Updates every client's public server information, including users in idle mode.
 * @param boole banner_changed -> ask clients to refresh the banner when it is published
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__server_settings__send_info_to_all_clients(boole banner_changed)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;
    uint64 i = 0;

    root = server_msg__delivery__new_message("server_info", &message);
    _server_msg_server_settings_internal__add_server_info(message, FALSE, banner_changed);
    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (permissions__is_allowed(&g_clients_array[i], PERMISSION_VIEW_SERVER_INFO) == TRUE)
        {
            server_msg__delivery__send_json_to_client(&g_clients_array[i], root);
        }
    }
    cJSON_Delete(root);
}

/**
 * @brief Announces a changed banner only when the administrator publishes it.
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__server_settings__send_banner_changed_to_all_clients(void)
{
    if (g_server_settings.is_server_banner_published == TRUE)
    {
        server_msg__server_settings__send_info_to_all_clients(TRUE);
    }
}

/**
 * @brief Tells an administrator whether saving the banner succeeded.
 * @param client_t* receiver -> administrator that sent the change
 * @param boole success -> TRUE when the banner was saved
 * @param cstring error -> short failure code, or an empty string on success
 * @return void
 * @attention The caller holds the clients lock.
 */
void server_msg__server_settings__send_banner_result_to_single_client(client_t* receiver, boole success, cstring error)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("server_banner_result", &message);
    cJSON_AddBoolToObject(message, "success", success);
    cJSON_AddStringToObject(message, "error", error);
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}

/**
 * @brief broadcasts the client-relevant policy values (file/picture limits, avatars, typing,
 *        saved users) to every connected client, so a settings change applies without a reconnect
 *
 * @note the caller must hold the clients read lock
 *
 * @return void
 */
void server_msg__server_settings__send_policy_update_to_all_clients(void)
{
    cJSON* root = NULL_POINTER;
    cJSON* message = NULL_POINTER;

    root = server_msg__delivery__new_message("server_policy", &message);
    server_msg__server_settings__add_attachment_receive_policy(message);

    cJSON_AddBoolToObject(message, "is_file_upload_allowed", g_server_settings.is_file_upload_allowed);
    cJSON_AddNumberToObject(message, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(message, "chat_picture_max_size", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_AddBoolToObject(message, "is_chat_picture_allowed", g_server_settings.is_chat_picture_allowed);
    cJSON_AddBoolToObject(message, "is_typing_indicator_allowed", g_server_settings.is_typing_indicator_allowed);
    cJSON_AddBoolToObject(message, "is_client_rename_allowed", g_server_settings.is_client_rename_allowed);
    cJSON_AddBoolToObject(message, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed);
    cJSON_AddBoolToObject(message, "is_avatar_upload_allowed", g_server_settings.is_avatar_upload_allowed);
    cJSON_AddNumberToObject(message, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
    cJSON_AddNumberToObject(message, "icon_max_size", (double)g_server_settings.icon_max_size_bytes);
    cJSON_AddBoolToObject(message, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
    cJSON_AddBoolToObject(message, "is_permission_system_enabled", g_server_settings.is_permission_system_enabled);
    cJSON_AddBoolToObject(message, "is_stored_client_list_allowed", g_server_settings.is_identity_storage_enabled && g_server_settings.is_stored_client_list_allowed);
    cJSON_AddBoolToObject(message, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed);
    cJSON_AddBoolToObject(message, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed);
    cJSON_AddNumberToObject(message, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients);
    cJSON_AddNumberToObject(message, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients);
    cJSON_AddBoolToObject(message, "is_music_bot_marquee_visible_to_everyone", g_server_settings.is_music_bot_marquee_visible_to_everyone);
    cJSON_AddBoolToObject(message, "is_video_streaming_allowed", g_server_settings.is_video_streaming_active);
    cJSON_AddBoolToObject(message, "is_audio_over_websocket_allowed", g_server_settings.is_audio_over_websocket_allowed);
    cJSON_AddBoolToObject(message, "is_audio_over_webrtc_allowed", g_server_settings.is_audio_over_webrtc_allowed);
    // the audio switches too: a client whose audio was switched off stops asking for connections
    cJSON_AddBoolToObject(message, "is_voice_chat_active", g_server_settings.is_voice_chat_active);
    cJSON_AddBoolToObject(message, "is_music_bot_audio_active", g_server_settings.is_music_bot_audio_active);

    server_msg__delivery__send_json_to_all_clients(root, NULL_POINTER);
    cJSON_Delete(root);
}

/**
 * @brief Confirms whether the server settings were written to disk.
 * @param receiver Administrator who requested the save.
 * @param success TRUE when the settings file was saved.
 */
void server_msg__server_settings__send_saved(client_t* receiver, boole success)
{
    cJSON* message = NULL_POINTER;
    cJSON* root = server_msg__delivery__new_message("server_settings_saved", &message);

    cJSON_AddBoolToObject(message, "success", success);
    server_msg__delivery__send_json_to_client(receiver, root);
    cJSON_Delete(root);
}
