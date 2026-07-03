#ifndef MUSIC_BOT_H
#define MUSIC_BOT_H 1

typedef struct musicbot_add_song_arg_struct_t
{
    char song_name[SONG_NAME_MAX_LENGTH];
    ubyte* mp3_data_buffer;
    uint64 mp3_data_buffer_length;
    client_t* music_bot;
    uint64 sender_client_id;
} musicbot_add_song_arg_struct_t;

void musicbot__remove_song(client_t* music_bot, uint64 song_id);
void musicbot__add_song(musicbot_add_song_arg_struct_t* arg);
void musicbot__threadstart(client_t* music_bot1);
void musicbot__begin_delete(client_t* music_bot_client);

#endif