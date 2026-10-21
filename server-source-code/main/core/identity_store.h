#ifndef IDENTITY_STORE_H
#define IDENTITY_STORE_H

#include "../definitions.h"

boole identity_store__is_public_key_present_in_client_stored_data(char* public_key);
void identity_store__restore_identity_tags(client_t* client);
void identity_store__restore_identity_avatar(client_t* client);
void identity_store__restore_identity_alias(client_t* client);
void identity_store__snapshot_connected_clients_into_identity_store(void);
void identity_store__sync_client_identity_in_store(client_t* client);
boole identity_store__delete_identity_from_store_by_hash(char* identity_hash);
void identity_store__set_identity_avatar_by_hash(char* identity_hash, char* base64_avatar);
boole identity_store__is_alias_taken_by_another_identity(char* alias, char* identity_hash);
void identity_store__touch_identity_last_seen_by_hash(char* identity_hash);
void identity_store__store_identity_raw_public_key(char* identity_hash, char* raw_public_key);
void identity_store__clear_identity_raw_public_key(char* identity_hash);
boole identity_store__get_identity_hash_by_alias(char* alias, char* out_identity_hash, uint64 out_buffer_size);
boole identity_store__is_identity_registered_by_hash(char* identity_hash);
boole identity_store__queue_offline_message(char* recipient_identity_hash, char* sender_identity_hash, char* sender_alias, char* base64_encrypted_message);
void identity_store__free_offline_messages_for_identity(char* identity_hash);
void identity_store__set_identity_alias_by_hash(char* identity_hash, char* alias);
boole identity_store__get_identity_avatar_by_hash(char* identity_hash, char* out_buffer, uint64 out_buffer_size);
boole identity_store__modify_identity_tag_in_store(char* identity_hash, uint64 tag_id, boole add);

extern client_stored_data_t* g_client_stored_data;
extern pthread_mutex_t g_client_stored_data_mutex;
extern offline_chat_message_t* g_offline_messages;
extern pthread_mutex_t g_offline_messages_mutex;
extern uint64 g_offline_message_sequence_counter;

#endif
