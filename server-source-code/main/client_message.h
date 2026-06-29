/* Created by user on 4/1/2024. */

#ifndef TEST0S_SOLUTION_CLIENT_MESSAGE_H
#define TEST0S_SOLUTION_CLIENT_MESSAGE_H

boole client_msg__is_message_correct_at_first_sight_and_get_message_type(cJSON* json_root, uint64 sender_client_id, char** out_message_type);
void client_msg__get_public_key_and_verification_string_and_dh_public_mix(cJSON* json_root, char** out_public_key, char** out_verification_string, char** out_dh_mix);
void client_msg__process_public_key_challenge_response(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_public_key_info(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_client_connection_check(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_change_client_username(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_create_channel_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_edit_channel_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_direct_chat_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_channel_chat_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_channel_chat_picture(uint64 sender_client_id, uint64 local_message_id, char* message_value);
void client_msg__process_direct_chat_picture(uint64 sender_client_id, uint64 receiver_id, uint64 local_message_id, char* message_value);
void client_msg__process_join_channel_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_delete_channel_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_poke_client_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_sdp_answer(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_ice_candidate(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_microphone_usage(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_start_song_stream_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_stop_song_stream_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_admin_password_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_change_admin_password_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_add_tag_to_client_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_remove_tag_from_client_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_set_server_settings_icon_upload(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_set_server_settings_add_new_tag(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_save_server_settings_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_load_server_settings_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_call_idle_client_message(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_go_to_idle_mode_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_come_back_from_idle_mode_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_create_new_webrtc_datachannel_connection(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_kick_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_ban_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_remove_ban_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_get_client_info_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_create_music_bot_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_delete_music_bot_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_musicbot_get_song_list_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_remove_song_from_music_bot_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_file_send_request(cJSON* json_root, uint64 sender_client_id);
void client_msg__process_file_send_completed_request(cJSON* json_root, uint64 sender_client_id);

#endif /* TEST0S_SOLUTION_CLIENT_MESSAGE_H */