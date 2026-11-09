#include "session/server_info.h"

static const desktop_server_feature_t g_server_features[] = { { "is_voice_chat_active", "Voice chat" }, { "is_music_bot_audio_active", "Music bot audio" }, { "is_video_streaming_allowed", "Video streaming" }, { "is_audio_over_websocket_allowed", "Audio/video over WebSocket" }, { "is_audio_over_webrtc_allowed", "WebRTC DataChannel" }, { "is_private_messaging_allowed", "Private messages" }, { "is_file_upload_allowed", "File uploads" }, { "is_chat_picture_allowed", "Chat pictures" }, { "is_link_preview_allowed", "Link previews" }, { "is_avatar_upload_allowed", "Avatars" }, { "is_typing_indicator_allowed", "Typing indicators" }, { "is_client_rename_allowed", "Name changes" }, { "is_temp_channel_creation_allowed", "Temporary channels" }, { "is_idle_mode_allowed", "Idle mode" }, { "is_fast_reconnect_allowed", "Fast reconnect" }, { "is_connection_check_enforced", "Disconnect on missed keepalives" }, { "is_identity_storage_enabled", "Saved identities" }, { "is_stored_client_list_allowed", "Offline user list" }, { "is_last_seen_enabled", "Last seen times" }, { "is_offline_messaging_allowed", "Offline messages" }, { "is_display_country_flags_active", "Country flags" }, { "is_hide_clients_in_password_protected_channels_active", "Hide users in protected channels" }, { "is_sending_text_to_idle_clients_allowed", "Messages to idle users" }, { "is_music_bot_marquee_visible_to_everyone", "Show music titles to everyone" } };

_Static_assert(sizeof(g_server_features) / sizeof(*g_server_features) == DESKTOP_SERVER_FEATURES, "Public feature count must match its bitset.");

/**
 * @brief returns a known public feature's protocol key and display label.
 * @param int64 index -> feature position from zero through DESKTOP_SERVER_FEATURES - 1
 * @return const desktop_server_feature_t* -> static description, or NULL for an invalid position
 */
const desktop_server_feature_t *desktop_server_info__feature(int64 index)
{
    return index >= 0 && index < DESKTOP_SERVER_FEATURES ? &g_server_features[index] : NULL;
}
