#ifndef CLIENT_MSG_H
#define CLIENT_MSG_H

#include "../../definitions.h"
#include "../../../third-party/dave-g-json/cJSON.h"

void client_msg__connection__process_connection_check(cJSON* json_root, uint64 sender_client_id);
void client_msg__connection__process_public_key_info(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket);
void client_msg__connection__process_public_key_challenge_response(cJSON* json_root, uint64 sender_client_id);

void client_msg__channel__process_set_icon(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_create_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_edit_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_join_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_delete_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_reset_maintainer_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_go_to_idle_mode_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__channel__process_come_back_from_idle_mode_request(cJSON* json_root, uint64 sender_client_id);

void client_msg__chat__process_direct_chat_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_channel_chat_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_delete_chat_message_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_edit_chat_message_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_poke_client_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_typing_indicator_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_offline_chat_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_call_idle_client_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__chat__process_resolve_url_request(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket);

void client_msg__attachments__process_attachment_receive_preferences(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket);
void client_msg__attachments__process_file_send_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__attachments__process_file_send_completed_request(cJSON* json_root, uint64 sender_client_id);

void client_msg__profile__process_change_client_username(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_set_identity_alias_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_set_alias_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_request_stored_clients(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_avatar_upload(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_delete_avatar(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_request_avatar_for_client(cJSON* json_root, uint64 sender_client_id);
void client_msg__profile__process_request_avatars_batch(cJSON* json_root, uint64 sender_client_id);

void client_msg__server_settings__process_save_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__server_settings__process_info_request(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket);
void client_msg__server_settings__process_set_banner_request(cJSON* json_root, uint64 sender_client_id, ws_cli_conn_t* socket);
void client_msg__server_settings__process_load_request(cJSON* json_root, uint64 sender_client_id);

void client_msg__administration__process_admin_password_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_change_admin_password_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_add_tag_to_client_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_remove_tag_from_client_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_icon_upload(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_add_tag(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_delete_tag(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_request_identity_list(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_delete_identity(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_modify_identity_tag(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_delete_icon(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_set_tag_icon(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_admin_log_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_admin_log_clear(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_kick_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_ban_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_remove_ban_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__administration__process_get_client_info_request(cJSON* json_root, uint64 sender_client_id);

void client_msg__audio__process_sdp_answer(cJSON* json_root, uint64 sender_client_id);
void client_msg__audio__process_ice_candidate(cJSON* json_root, uint64 sender_client_id);
void client_msg__audio__process_microphone_usage(cJSON* json_root, uint64 sender_client_id);
void client_msg__audio__process_create_new_webrtc_datachannel_connection(cJSON* json_root, uint64 sender_client_id);

void client_msg__video__process_start(cJSON* json_root, uint64 sender_client_id);
void client_msg__video__process_stop(cJSON* json_root, uint64 sender_client_id);
void client_msg__video__process_watch(cJSON* json_root, uint64 sender_client_id);
void client_msg__video__process_allow_viewer(cJSON* json_root, uint64 sender_client_id);
void client_msg__video__process_keyframe_request(cJSON* json_root, uint64 sender_client_id);

void client_msg__musicbot__process_start_song_stream_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__musicbot__process_stop_song_stream_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__musicbot__process_create_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__musicbot__process_delete_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__musicbot__process_get_song_list_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__musicbot__process_remove_song_request(cJSON* json_root, uint64 sender_client_id);

#endif
