#ifndef MUSIC_BOT_H
#define MUSIC_BOT_H 1

void musicbot__remove_song(client_t *music_bot, int song_id);
void musicbot__add_song(char *song_name, ubyte *mp3_data_buffer, uint64 mp3_data_buffer_length, client_t *music_bot);
void musicbot__threadstart(client_t *music_bot1);

#endif