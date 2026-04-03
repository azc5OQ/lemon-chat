#ifndef BASE_H
#define BASE_H

//everything should be as simple as possible, but not simpler!

int base__get_new_index_for_client(void);

void base__process_authenticated_client_message(ws_cli_conn_t *websocket, int client_index, char *decrypted_metadata_cstring);
void base__process_not_authenticated_client_message(ws_cli_conn_t *websocket, int index, char *decrypted_metadata_cstring);
char *base__encrypt_cstring_and_convert_to_base64(char *string, int *out_allocated_buffer_size, char *dh_shared_secret);
void base__get_data_from_base64_and_decrypt_it(int client_id, char *base64_string, unsigned char *out_buffer, int out_buffer_length);
void base__free_json_message(cJSON *json_root_object1, char *json_root_object1_string);
int base__get_client_count_for_channel(int channel_id);
char *base__encrypt_string_with_public_key(char *public_key_modulus, unsigned char *bytes, uint64 buffer_length);
void base__fill_block_of_data_with_ascii_characters(char *block, int length);
boole base__is_there_a_client_with_same_public_key(cstring public_key);
boole base__assign_username_for_newly_joined_client(int client_index, cstring default_name);
boole base__is_public_key_present_in_client_stored_data(char *public_key);
void base__close_websocket_connection(int client_index, boole use_readlock);
uint64 base__get_timestamp_ms(void);
void base__sleep_for_milliseconds(int milliseconds);
uint64 base__get_chat_message_id(void);
void base__increment_chat_message_id(void);
boole base__is_request_allowed_based_on_spam_protection(int client_index);
boole base__find_new_maintainer_for_channel(int *_out__new_index_of_maintainer, int channel_id, int index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer);
boole base__is_there_a_client_with_same_ip_address(cstring ip_address);
void base__mark_channels_for_deletion(int channel_id, int *current_index, int *channel_indices);
boole base__is_tag_id_real(int tag_id);
boole base__is_client_already_assigned_this_tag_id(int client_id, int this_tag_id);
int base__get_index_of_tag_id_of_client(int client_id, int this_tag_id);
client_t *base__find_music_bot_in_channel(int channel_id);
void base__process_client_disconnect(int client_index);
boole base__is_client_valid(int client_id);
int base__get_other_clients_in_channel(int client_to_ignore, int channel_id, int *receiving_client_ids);

extern uint64 chat_message_id;

extern client_t *clients_array;
extern channel_t *channel_array;
extern icon_t *icons_array;
extern tag_t *tags_array;
extern client_stored_data_t *client_stored_data;

//global lock ordering goes like this
//clients, muggles, channels, icons, tags

extern server_settings_t g_server_settings;
extern custom_rwlock_t clients_global_rwlock_guard;
extern custom_rwlock_t webrtc_muggles_rwlock_guard;
extern custom_rwlock_t channels_global_rwlock_guard;
extern custom_rwlock_t icons_global_rwlock_guard;
extern custom_rwlock_t tags_global_rwlock_guard;

extern pthread_mutex_t chat_message_id_mutex;

#endif
