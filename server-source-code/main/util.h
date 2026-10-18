#ifndef UTIL_H

#define UTIL_H 1

boole util__is_client_valid(int client_id);
boole util__is_attachment_blocked(client_t* client, cstring receive_type);
boole util__is_client_valid_admin(int client_id);
boole util__is_client_valid_musicbot(int client_id);
boole util__is_client_valid_and_not_music_bot(int client_id);
boole util__is_music_bot_and_song_slot_valid(int client_id);
boole util__normalize_country_code(cJSON* json_entry, char* out_code);
boole util__start_detached_thread(void* (*entry)(void*), void* argument);
boole util__get_process_ram_usage(uint64* bytes);
void util__count_webrtc_bytes(uint64 received, uint64 sent);
void util__get_webrtc_bytes(uint64* received, uint64* sent);
boole util__is_server_ram_budget_reached(void);

#endif
