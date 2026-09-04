// Created by user on 4/1/2024.

#ifndef TEST0S_SOLUTION_SERVER_MESSAGE_H
#define TEST0S_SOLUTION_SERVER_MESSAGE_H

void server_msg__send_public_key_challenge_to_single_client(ws_cli_conn_t* websocket, char* random_value_challenge_string, char* dh_public_mix_for_client);
void server_msg__send_rsa_key_too_weak_to_single_client(ws_cli_conn_t* websocket);
void server_msg__send_authentication_status_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret);
void server_msg__send_channel_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret);
void server_msg__send_client_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret, char* local_clients_username, uint64 receiver_client_id);
void server_msg__send_icon_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret);
void server_msg__send_tag_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret);
void server_msg__send_identity_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret);
void server_msg__send_active_microphone_usage_for_current_channel_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret, uint64 current_channel_id);
void server_msg__send_client_connect_message_to_all_clients(uint64 connected_client_id);
void server_msg__send_maintainer_id_to_single_client(client_t* client, uint64 channel_id, uint64 current_channel_maintainer_id);
void server_msg__send_connection_check_response_to_single_client(client_t* client);
void server_msg__send_fast_reconnect_ok_to_single_client(client_t* client);
void server_msg__send_client_rename_message_to_all_clients(uint64 username_changer_client_id, char* new_username);
void server_msg__send_access_denied_to_single_client(client_t* client, char* reason);
void server_msg__send_force_admin_password_change_to_single_client(client_t* client);
void server_msg__send_client_info_to_single_client(client_t* receiving_client, client_t* target_client);
void server_msg__send_channel_full_to_single_client(client_t* client, uint64 channel_id);
void server_msg__send_server_settings_to_single_client(client_t* client);
void server_msg__send_admin_log_to_single_client(client_t* client);
void server_msg__send_policy_update_to_all_clients(void);
void server_msg__send_channel_create_message_to_all_clients(uint64 created_channel_index, uint64 channel_creator_client_id);
void server_msg__send_channel_edit_message_to_all_clients(uint64 edited_channel_id, uint64 channel_editor_client_id);
void server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(uint64 client_id, uint64 chat_message_id, uint64 local_message_id);
void server_msg__send_chat_message_to_single_client(uint64 sender_client_id, uint64 receiver_client_id, uint64 server_chat_message_id, char* chat_message_value);
void server_msg__send_chat_message_to_clients_in_same_channel(uint64 sender_client_id, uint64 receiving_channel_id, uint64 server_chat_message_id, char* chat_message_value);
void server_msg__send_chat_message_action_to_single_client(uint64 client_receiver_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value);
void server_msg__send_chat_message_action_to_clients_in_same_channel(uint64 receiving_channel_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value);
void server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(uint64 sender_client_id, uint64 receiving_channel_id, uint64 server_chat_message_id, uint64 encrypted_size);
void server_msg__send_channel_chat_picture_to_clients_in_same_channel(uint64 sender_client_id, uint64 receiving_channel_id, uint64 server_chat_message_id, char* chat_message_value);
void server_msg__send_image_status_to_single_client(client_t* client, char* status);
void server_msg__send_chat_picture_metadata_to_single_client(uint64 sender_client_id, uint64 receiver_client_id, uint64 server_chat_message_id, uint64 encrypted_size);
void server_msg__send_channel_chat_file_metadata_to_clients_in_same_channel(uint64 sender_client_id, uint64 receiving_channel_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size);
void server_msg__send_chat_file_metadata_to_single_client(uint64 sender_client_id, uint64 receiver_client_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size);
void server_msg__send_file_send_error_to_single_client(client_t* client, char* reason, uint64 local_message_id);
void server_msg__send_channel_join_message_to_all_clients(client_t* client_that_switched_channel, channel_t* new_channel);
void server_msg__send_channel_join_message_to_single_client(client_t* client_that_switched_channel, channel_t* new_channel, client_t* receiving_client);
void server_msg__send_maintainer_id_to_clients_in_same_channel(uint64 channel_id, uint64 channel_maintainer_client_id);
void server_msg__send_channel_delete_message_to_all_clients(uint64 deleted_channel_id, uint64 channel_deletor_client_id);
void server_msg__send_client_disconnect_message_to_all_clients(uint64 client_id);
void server_msg__send_poke_to_single_client(client_t* client, uint64 sender_client_id, char* poke_message);
void server_msg__send_typing_indicator(uint64 sender_client_id, char* receiver_type, uint64 receiver_id);
void server_msg__send_audio_state_of_client_to_all_clients(uint64 client_id, uint64 audio_state);
void server_msg__send_start_song_stream_message_to_clients_in_same_channel(client_t* client_that_streams);
void server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client_t* client_that_streams);
void server_msg__send_add_tag_to_client_event_to_all_clients(uint64 client_id_of_client_that_got_the_new_tag, uint64 tag_id);
void server_msg__send_client_alias_changed_to_all_clients(uint64 client_id, char* alias);
void server_msg__send_avatar_changed_event_to_all_clients(uint64 client_id_whose_avatar_changed);
void server_msg__send_client_avatar_to_single_client(ws_cli_conn_t* websocket, char* dh_shared_secret, uint64 client_id, char* base64_avatar);
void server_msg__send_stored_clients_to_single_client(ws_cli_conn_t* websocket, char* dh_shared_secret);
void server_msg__send_queued_offline_messages_to_single_client(client_t* client);
void server_msg__send_remove_tag_from_client_event_to_all_clients(uint64 client_id_of_client_that_got_tag_removed, uint64 tag_id);
void server_msg__send_add_new_icon_event_to_all_clients(uint64 new_icon_id, char* icon_base64_value);
void server_msg__send_create_new_tag_event_to_all_clients(uint64 tag_id, char* tag_name, uint64 tag_linked_icon_id, boole has_icon);
void server_msg__send_remove_tag_event_to_all_clients(uint64 tag_id);
void server_msg__send_remove_icon_event_to_all_clients(uint64 icon_id);
void server_msg__send_tag_icon_changed_event_to_all_clients(uint64 tag_id, boole has_icon, uint64 icon_id);
void server_msg__send_channel_icon_changed_event_to_all_clients(uint64 channel_id, boole has_channel_icon, uint64 icon_id);
void server_msg__send_call_event_to_idle_client(client_t* caller, client_t* callee);
void server_msg__send_client_going_to_idle_mode_info_to_all_clients(uint64 idle_going_client_id);
void server_msg__send_client_coming_back_from_idle_mode_info_to_all_clients(uint64 back_from_idle_client_id, uint64 channel_the_client_joins);
void server_msg__send_music_bot_song_list_to_single_client(uint64 sender_client_id, uint64 music_bot_id);
void server_msg__send_file_send_completed_status_to_single_client(client_t* client);
void server_msg__send_file_by_chunk_to_single_client(char* chunk, uint64 current_size, uint64 sender_client_id, uint64 receiver_client_id, uint64 server_chat_message_id);
void server_msg__send_file_receive_completed_to_single_client(data_for_file_send_thread_t* info, uint64 receiver_client_id, char* receive_type);

// audio related
void server_msg__send_webrtc_sdp_offer_to_single_client(const char* cand, const char* mid, client_t* client);

#endif // TEST0S_SOLUTION_SERVER_MESSAGE_H
