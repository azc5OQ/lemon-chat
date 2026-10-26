#ifndef BASE_H
#define BASE_H

#include "definitions.h"

boole base__is_request_allowed_based_on_spam_protection(uint64 client_index);
uint64 base__get_chat_message_id(void);
void base__increment_chat_message_id(void);

boole base__is_client_valid(int client_id);
boole base__is_client_valid_and_not_music_bot(int client_id);
boole base__is_client_valid_admin(int client_id);
boole base__is_client_valid_musicbot(int client_id);
boole base__is_music_bot_and_song_slot_valid(int client_id);
boole base__is_attachment_blocked(client_t* client, cstring receive_type);
boole base__is_server_ram_budget_reached(void);
void base__count_webrtc_bytes(uint64 received, uint64 sent);
void base__get_webrtc_bytes(uint64* received, uint64* sent);
void base__start_server_uptime(void);
uint64 base__get_server_uptime_seconds(void);

void base__process_authenticated_client_message(ws_cli_conn_t* websocket, uint64 client_index, char* decrypted_metadata_cstring);

void base__process_not_authenticated_client_message(ws_cli_conn_t* websocket, uint64 index, char* decrypted_metadata_cstring);

extern uint64 g_chat_message_id;
extern client_t* g_clients_array;
extern channel_t* g_channel_array;
extern icon_t* g_icons_array;
extern group_t* g_groups_array;
extern ban_entry_t* g_ban_array;
extern server_settings_t g_server_settings;
extern custom_rwlock_t g_clients_global_rwlock_guard;
extern custom_rwlock_t g_webrtc_peers_rwlock_guard;
extern custom_rwlock_t g_channels_global_rwlock_guard;
extern custom_rwlock_t g_icons_global_rwlock_guard;
extern custom_rwlock_t g_groups_global_rwlock_guard;
extern custom_rwlock_t g_bans_global_rwlock_guard;
extern pthread_mutex_t g_chat_message_id_mutex;

#endif
