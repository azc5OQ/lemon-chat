#ifndef UTIL_H

#define UTIL_H 1

boole util__is_client_valid(int client_id);
boole util__is_client_valid_admin(int client_id);
boole util__is_client_valid_musicbot(int client_id);
boole util__is_client_valid_and_not_music_bot(int client_id);
boole util__is_music_bot_and_song_slot_valid(int client_id);

#endif