#ifndef BASE_H
#define BASE_H

/* everything should be as simple as possible, but not simpler! */

int64   base__get_new_index_for_client(void);

void    base__process_authenticated_client_message(ws_cli_conn_t* websocket, uint64 client_id, char* decrypted_metadata_cstring);
void    base__process_not_authenticated_client_message(ws_cli_conn_t* websocket, uint64 index, char* decrypted_metadata_cstring);
char*   base__encrypt_cstring_and_convert_to_base64(char* string, int64* out_allocated_buffer_size, char* dh_shared_secret);
void    base__hash_password_to_base64(char* plaintext, char* out, int64 out_size);
boole   base__password_matches(char* plaintext, char* stored_base64_hash);
void    base__get_data_from_base64_and_decrypt_it(uint64 client_id, char* base64_string, unsigned char* out_buffer, int64 out_buffer_length);
void    base__free_json_message(cJSON* json_root_object1, char* json_root_object1_string);
uint64  base__get_client_count_for_channel(uint64 channel_id);
char*   base__encrypt_string_with_public_key(char* public_key_modulus, unsigned char* bytes, uint64 buffer_length);
boole   base__fill_secure_random_bytes(unsigned char* out_buffer, uint64 length);
boole   base__fill_block_of_data_with_ascii_characters(char* block, uint64 length);
boole   base__is_there_a_client_with_same_public_key(cstring public_key);
boole   base__assign_username_for_newly_joined_client(uint64 client_id, cstring default_name);
boole   base__is_public_key_present_in_client_stored_data(char* public_key);
void    base__restore_identity_tags(client_t* client);
void    base__restore_identity_avatar(client_t* client);
void    base__snapshot_connected_clients_into_identity_store(void);
void    base__sync_client_identity_in_store(client_t* client);
boole   base__delete_identity_from_store_by_hash(char* identity_hash);
boole   base__modify_identity_tag_in_store(char* identity_hash, uint64 tag_id, boole add);
void    base__set_identity_avatar_by_hash(char* identity_hash, char* base64_avatar);
boole   base__get_identity_avatar_by_hash(char* identity_hash, char* out_buffer, uint64 out_buffer_size);
void    base__close_websocket_connection(uint64 client_id, boole use_readlock);
uint64  base__get_timestamp_ms(void);
void    base__sleep_for_milliseconds(uint64 milliseconds);
uint64  base__get_chat_message_id(void);
void    base__increment_chat_message_id(void);
boole   base__is_request_allowed_based_on_spam_protection(uint64 client_id);
boole   base__find_new_maintainer_for_channel(uint64* _out__new_index_of_maintainer, uint64 channel_id, uint64 client_left_id, boole do_not_include_client_that_left_when_searching_for_new_maintainer);
boole   base__is_there_a_client_with_same_ip_address(cstring ip_address);
void    base__mark_channels_for_deletion(uint64 channel_id, uint64* out_current_index, uint64* out_channel_indices);
boole   base__save_server_settings_to_file(void);
boole   base__write_file_atomically(char* path, char* contents);
boole   base__is_tag_id_real(uint64 tag_id);
boole   base__is_client_already_assigned_this_tag_id(uint64 client_id, uint64 this_tag_id);
int64   base__get_index_of_tag_id_of_client(uint64 client_id, uint64 this_tag_id);
client_t* base__find_music_bot_in_channel(uint64 channel_id);
void    base__process_client_disconnect(uint64 client_id);
void    base__destroy_temp_channel(uint64 temp_channel_id);
void    base__move_client_into_channel(uint64 client_id, uint64 destination_channel_id);
boole   base__is_client_valid(uint64 client_id);
uint64  base__get_other_clients_in_channel(int client_to_ignore, uint64 channel_id, int64* out_receiving_client_ids);
boole   base__is_ip_banned(char* ip_address);
boole   base__add_ban(char* ip_address, char* country_iso_code, char* identity, char* extra_data);
boole   base__remove_ban_by_ip(char* ip_address);

extern uint64 g_chat_message_id;

extern client_t* g_clients_array;
extern channel_t* g_channel_array;
extern icon_t* g_icons_array;
extern tag_t* g_tags_array;
extern ban_entry_t* g_ban_array;
extern client_stored_data_t* g_client_stored_data;
extern pthread_mutex_t g_client_stored_data_mutex;

/* global lock ordering goes like this: clients, muggles, channels, icons, tags, bans (bans is always taken last) */

extern server_settings_t g_server_settings;
extern custom_rwlock_t g_clients_global_rwlock_guard;
extern custom_rwlock_t g_webrtc_muggles_rwlock_guard;
extern custom_rwlock_t g_channels_global_rwlock_guard;
extern custom_rwlock_t g_icons_global_rwlock_guard;
extern custom_rwlock_t g_tags_global_rwlock_guard;
extern custom_rwlock_t g_bans_global_rwlock_guard;

extern pthread_mutex_t g_chat_message_id_mutex;

#endif
