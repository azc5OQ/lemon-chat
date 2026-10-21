#ifndef SERVER_MSG_H
#define SERVER_MSG_H

#include "../../definitions.h"
#include "../../../third-party/dave-g-json/cJSON.h"

void server_msg__connection__send_rsa_key_too_weak_to_single_client(client_t* receiver);
void server_msg__connection__send_public_key_challenge_to_single_client(client_t* receiver, char* random_value_challenge_string, char* dh_public_mix_for_client);
void server_msg__connection__send_authentication_status_to_single_client(client_t* receiver);
void server_msg__connection__send_client_list_to_single_client(client_t* receiver);
void server_msg__connection__send_client_connect_message_to_all_clients(client_t* subject);
void server_msg__connection__send_connection_check_response_to_single_client(client_t* receiver);
void server_msg__connection__send_fast_reconnect_ok_to_single_client(client_t* receiver);
void server_msg__connection__send_client_disconnect_message_to_all_clients(uint64 client_index);

void server_msg__channel__send_full_to_single_client(client_t* receiver, uint64 channel_id);
void server_msg__channel__send_list_to_single_client(client_t* receiver);
void server_msg__channel__send_maintainer_id_to_single_client(client_t* receiver, channel_t* channel, uint64 maintainer_id);
void server_msg__channel__send_create_message_to_all_clients(channel_t* channel, client_t* creator);
void server_msg__channel__send_edit_message_to_all_clients(channel_t* channel, client_t* editor);
void server_msg__channel__send_join_message_to_all_clients(client_t* subject, channel_t* new_channel);
void server_msg__channel__send_join_message_to_single_client(client_t* receiver, client_t* subject, channel_t* new_channel);
void server_msg__channel__send_maintainer_id_to_clients_in_same_channel(channel_t* channel, uint64 maintainer_id);
void server_msg__channel__send_delete_message_to_all_clients(uint64 deleted_channel_id, uint64 channel_deletor_id);
void server_msg__channel__send_icon_changed_event_to_all_clients(uint64 channel_id, boole has_channel_icon, uint64 icon_id);
void server_msg__channel__send_call_event_to_idle_client(client_t* receiver, client_t* caller);
void server_msg__channel__send_client_going_to_idle_mode_info_to_all_clients(client_t* subject);
void server_msg__channel__send_client_coming_back_from_idle_mode_info_to_all_clients(client_t* subject, uint64 channel_the_client_joins);

void server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(client_t* receiver, uint64 chat_message_id, uint64 local_message_id);
void server_msg__chat__send_message_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, char* chat_message_value);
void server_msg__chat__send_message_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* chat_message_value);
void server_msg__chat__send_message_action_to_single_client(client_t* receiver, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value);
void server_msg__chat__send_message_action_to_clients_in_same_channel(uint64 receiving_channel_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value);
void server_msg__chat__send_poke_to_single_client(client_t* receiver, client_t* sender, char* poke_message);
void server_msg__chat__send_typing_indicator(client_t* subject, char* receiver_type, uint64 receiver_id);
void server_msg__chat__send_queued_offline_messages_to_single_client(client_t* receiver);
void server_msg__chat__send_url_resolved_to_single_client(client_t* client, const char* request_id, const char* url, const cJSON* result);

void server_msg__attachments__send_channel_chat_picture_metadata_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, uint64 encrypted_size);
void server_msg__attachments__send_channel_chat_picture_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* chat_message_value);
void server_msg__attachments__send_image_status_to_single_client(client_t* receiver, char* status);
void server_msg__attachments__send_chat_picture_metadata_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, uint64 encrypted_size);
void server_msg__attachments__send_channel_chat_file_metadata_to_clients_in_same_channel(client_t* sender, uint64 channel_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size);
void server_msg__attachments__send_chat_file_metadata_to_single_client(client_t* receiver, client_t* sender, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size);
void server_msg__attachments__send_file_send_error_to_single_client(client_t* receiver, char* reason, uint64 local_message_id);
void server_msg__attachments__send_file_send_completed_status_to_single_client(client_t* receiver);
int server_msg__attachments__send_file_by_chunk_to_single_client(client_t* receiver, client_t* sender, char* chunk, uint64 current_size, uint64 server_chat_message_id);
char* server_msg__attachments__prepare_file_chunk(uint64 sender_id, char* chunk, uint64 server_chat_message_id);
int server_msg__attachments__queue_file_chunk(client_t* receiver, const char* plaintext);
void server_msg__attachments__send_file_receive_completed_to_single_client(client_t* receiver, data_for_file_send_thread_t* info, char* receive_type);

cstring server_msg__profile__country_code_to_send(client_t* client);
void server_msg__profile__send_client_rename_message_to_all_clients(client_t* subject, char* new_username);
void server_msg__profile__send_client_alias_changed_to_all_clients(client_t* subject, char* alias);
void server_msg__profile__send_client_country_code_changed_to_all_clients(client_t* subject);
void server_msg__profile__send_admin_country_codes_to_all_clients(void);
void server_msg__profile__send_avatar_changed_event_to_all_clients(client_t* subject);
void server_msg__profile__send_client_avatar_to_single_client(client_t* receiver, uint64 avatar_owner_client_id, char* base64_avatar);
void server_msg__profile__send_stored_clients_to_single_client(client_t* receiver);

void server_msg__server_settings__add_attachment_receive_policy(cJSON* message);
void server_msg__server_settings__send_to_single_client(client_t* receiver, boole memory_only);
void server_msg__server_settings__send_info_to_single_client(client_t* receiver, boole include_banner, boole banner_changed);
void server_msg__server_settings__send_info_to_all_clients(boole banner_changed);
void server_msg__server_settings__send_banner_changed_to_all_clients(void);
void server_msg__server_settings__send_banner_result_to_single_client(client_t* receiver, boole success, cstring error);
void server_msg__server_settings__send_policy_update_to_all_clients(void);

void server_msg__administration__send_client_info_to_single_client(client_t* receiver, client_t* subject);
void server_msg__administration__send_icon_list_to_single_client(client_t* receiver);
void server_msg__administration__send_tag_list_to_single_client(client_t* receiver);
void server_msg__administration__send_identity_list_to_single_client(client_t* receiver);
void server_msg__administration__send_force_admin_password_change_to_single_client(client_t* receiver);
void server_msg__administration__send_access_denied_to_single_client(client_t* receiver, char* reason);
void server_msg__administration__send_admin_log_to_single_client(client_t* receiver);
void server_msg__administration__send_add_tag_to_client_event_to_all_clients(client_t* subject, uint64 tag_id);
void server_msg__administration__send_remove_tag_from_client_event_to_all_clients(client_t* subject, uint64 tag_id);
void server_msg__administration__send_add_new_icon_event_to_all_clients(uint64 new_icon_id, char* icon_base64_value);
void server_msg__administration__send_remove_tag_event_to_all_clients(uint64 tag_id);
void server_msg__administration__send_remove_icon_event_to_all_clients(uint64 icon_id);
void server_msg__administration__send_tag_icon_changed_event_to_all_clients(uint64 tag_id, boole has_icon, uint64 icon_id);
void server_msg__administration__send_create_new_tag_event_to_all_clients(uint64 tag_id, char* tag_name, uint64 tag_linked_icon_id, boole has_icon);

void server_msg__audio__send_active_microphone_usage_for_current_channel_to_single_client(client_t* receiver, uint64 channel_id);
void server_msg__audio__send_datachannel_cooldown_to_single_client(client_t* receiver, uint64 seconds_left);
void server_msg__audio__send_webrtc_sdp_offer_to_single_client(client_t* receiver, const char* candidate, const char* mid);
void server_msg__audio__send_state_of_client_to_all_clients(client_t* subject, uint64 state);

void server_msg__video__send_state_to_clients_in_same_channel(uint64 channel_id, boole is_active, uint64 streamer_client_id, cstring reason);
void server_msg__video__send_offer_to_single_client(client_t* receiver, client_t* streamer);
void server_msg__video__send_viewer_request_to_single_client(client_t* receiver, client_t* newcomer);
void server_msg__video__send_viewer_state_to_single_client(client_t* receiver, uint64 viewer_client_id, boole is_watching);
void server_msg__video__send_keyframe_request_to_single_client(client_t* receiver);
void server_msg__video__send_refused_to_single_client(client_t* receiver, cstring reason);

void server_msg__musicbot__add_keys(cJSON* object, client_t* receiver, client_t* bot);
void server_msg__musicbot__send_key(client_t* receiver, client_t* bot);
void server_msg__musicbot__send_start_song_stream_message_to_clients_in_same_channel(client_t* subject);
void server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(client_t* subject);
void server_msg__musicbot__send_song_list_to_single_client(client_t* receiver, client_t* music_bot);

void server_msg__delivery__release_encoded_text(char* text);
int server_msg__delivery__queue_text(ws_cli_conn_t* socket, const crypto_session_keys_t* session, const char* plaintext, boole bulk, boole keepalive);
boole server_msg__delivery__queue_text_to_client(client_t* client, const char* plaintext);
cJSON* server_msg__delivery__new_message(const char* type, cJSON** out_message);
void server_msg__delivery__send_text_to_client(client_t* client, char* plaintext);
void server_msg__delivery__send_json(ws_cli_conn_t* socket, char* dh_shared_secret, cJSON* json_root);
void server_msg__delivery__send_json_to_client(client_t* client, cJSON* json_root);
void server_msg__delivery__send_json_to_all_clients(cJSON* json_root, client_t* except);
void server_msg__delivery__send_text_to_channel(char* plaintext, uint64 channel_id, client_t* except, cstring receive_type);
void server_msg__delivery__send_json_to_channel(cJSON* json_root, uint64 channel_id, client_t* except);

#endif
