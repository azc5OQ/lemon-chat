#ifndef MUSIC_BOT_H
#define MUSIC_BOT_H 1

#include "../definitions.h"
#include "../../third-party/libopus-1.6.1/include/opus_types.h"

typedef struct musicbot_add_song_arg_struct_t
{
    char song_name[SONG_NAME_MAX_LENGTH];
    ubyte *mp3_data_buffer;
    uint64 mp3_data_buffer_length;
    client_t *music_bot;
    uint64 sender_client_id;
    char sender_identity[MAX_PUBLIC_KEY_LENGTH];
} musicbot_add_song_arg_struct_t;

// result of preparing one song for streaming: 48 kHz int16 PCM ready for the opus encoder
typedef struct musicbot_prepared_song_t
{
    boole is_valid;
    int64 song_index;
    uint64 generation;
    char song_name[SONG_NAME_MAX_LENGTH];
    opus_int16 *pcm_int16;
    uint64 frame_count; // frames at 48 kHz
    uint64 channels;
    uint64 song_length_seconds;
} musicbot_prepared_song_t;

// argument/result carrier for the preload thread; owned (allocated, joined, freed) by the bot thread
typedef struct musicbot_prepare_arg_t
{
    client_t *music_bot_client;
    int64 song_index;
    musicbot_prepared_song_t result;
} musicbot_prepare_arg_t;

client_t *musicbot__allocate(uint64 channel_id, const char *name);
boole musicbot__start(client_t *bot);
void musicbot__free_unstarted(client_t *bot);
void musicbot__remove_song(client_t *music_bot, uint64 song_id);
void musicbot__add_song(musicbot_add_song_arg_struct_t *arg);
void *musicbot__threadstart(void *argument);
boole musicbot__begin_delete(client_t *music_bot_client);

#endif
