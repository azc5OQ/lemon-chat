//
// Created by user on 4/1/2024.
//

#ifndef TEST0S_SOLUTION_CLIENT_MESSAGE_H
#define TEST0S_SOLUTION_CLIENT_MESSAGE_H

boole client_msg__is_message_correct_at_first_sight_and_get_message_type(cJSON *json_root, int client_array_index, char **message_type);
void client_msg__get_public_key_and_verification_string_and_dh_public_mix(cJSON *json_root, char **public_key, char **verification_string, char **dh_mix);
void client_msg__process_public_key_challenge_response(cJSON *json_root, int sender_client_index);
void client_msg__process_public_key_info(cJSON *json_root, int sender_client_index);
void client_msg__process_client_connection_check(cJSON *json_root, int sender_client_index);
void client_msg__process_change_client_username(cJSON *json_root, int sender_client_index);
void client_msg__process_create_channel_request(cJSON *json_root, int sender_client_index);
void client_msg__process_edit_channel_request(cJSON *json_root, int sender_client_index);
void client_msg__process_direct_chat_message(cJSON *json_root, int sender_client_index);
void client_msg__process_channel_chat_message(cJSON *json_root, int sender_client_index);
void client_msg__process_channel_chat_picture(cJSON *json_root, int sender_client_index);
void client_msg__process_direct_chat_picture(cJSON *json_root, int sender_client_index);
void client_msg__process_join_channel_request(cJSON *json_root, int sender_client_index);
void client_msg__process_delete_channel_request(cJSON *json_root, int sender_client_index);
void client_msg__process_poke_client_request(cJSON *json_root, int sender_client_index);
void client_msg__process_sdp_answer(cJSON *json_root, int sender_client_index);
void client_msg__process_ice_candidate(cJSON *json_root, int sender_client_index);
void client_msg__process_microphone_usage(cJSON *json_root, int sender_client_index);
void client_msg__process_start_song_stream_message(cJSON *json_root, int sender_client_index);
void client_msg__process_stop_song_stream_message(cJSON *json_root, int sender_client_index);
void client_msg__process_admin_password_message(cJSON *json_root, int sender_client_index);
void client_msg__process_add_tag_to_client_message(cJSON *json_root, int sender_client_index);
void client_msg__process_remove_tag_from_client_message(cJSON *json_root, int sender_client_index);
void client_msg__process_set_server_settings_icon_upload(cJSON *json_root, int sender_client_index);
void client_msg__process_set_server_settings_add_new_tag(cJSON *json_root, int sender_client_index);

#endif //TEST0S_SOLUTION_CLIENT_MESSAGE_H