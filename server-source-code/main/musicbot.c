#include "definitions.h"

#include "../third-party/dave-g-json/cJSON.h"
#include "../third-party/libopus-1.5.2/include/opus.h"
#include "../third-party/rxi-log/log.h"
#define DR_MP3_IMPLEMENTATION
#include "../third-party/dr_mp3/dr_mp3.h"

#include "base.h"

#include "audio_channel.h"
#include "musicbot.h"
#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "memory_manager.h"
#include "server_message.h"
#include "util.h"

/* todo, add stereo, mono sounds weird */

/**
 * @brief removes a song from the music bot: frees its mp3 buffer, clears its slot, decrements the song count
 *
 * @param client_t* music_bot -> the client_t that represents the music bot
 * @param uint64 song_id -> index of the song slot to remove
 *
 * @attention questionable thread safety
 *
 * @return void
 */
void musicbot__remove_song(client_t* music_bot, uint64 song_id)
{
    if (music_bot->music_bot_client_extension.songs[song_id].is_existing == FALSE)
    {
        return;
    }
    music_bot->music_bot_client_extension.songs[song_id].is_existing = FALSE;

    if (music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer);
        music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer = 0;
    }

    /* null out single music_bot_single_song_data_t entry */
    clib__null_memory((void*)&music_bot->music_bot_client_extension.songs[song_id], sizeof(music_bot_single_song_data_t));

    music_bot->music_bot_client_extension.music_bot_songs_count--;
}

/**
 * @brief adds song to music bot, it is used within write lock on clients array
 * @attention can cause future problems with tread safety
 * *
 * @return void
 * */

/* add client check macro that checks if client is valid before every access to it */

void musicbot__add_song(musicbot_add_song_arg_struct_t* arg)
{
    music_bot_single_song_data_t* song_in_loop = NULL_POINTER;
    drmp3 mp3;
    boole status = FALSE;
    boole is_song_added_succesfully = FALSE;
    void* mp3_data_buffer1 = NULL_POINTER;
    int64 mp3_data_buffer_length1 = 0;
    uint64 i = 0;
    drmp3_uint64 frameCount = 0;
    uint64 seconds_length = 0;
    
    /* find free song index */
    for (i = 0; i < MUSIC_BOT_MAX_FILE_COUNT; i++)
    {
        frameCount = 0;
        seconds_length = 0;
        
        /* lock clients array and do all the checks if music bot is valid, if it song slot is valid */
        /* if all checks are passed assign buffer to the song                                       */
        
        clib__write_lock(&g_clients_global_rwlock_guard);

        status = util__is_client_valid_musicbot(arg->music_bot->client_id);

        if (status == FALSE)
        {
            memorymanager__free((nuint)arg->mp3_data_buffer);
            memorymanager__free((nuint)arg);
            clib__unlock(&g_clients_global_rwlock_guard);
            break;
        }

        song_in_loop = &arg->music_bot->music_bot_client_extension.songs[i];
        if (song_in_loop->is_existing == TRUE || song_in_loop->is_being_uploaded == TRUE)
        {
            clib__unlock(&g_clients_global_rwlock_guard);
            continue;
        }

        song_in_loop->is_being_uploaded = TRUE; /* so other thread won't start uploading to the same slot */
        song_in_loop->mp3_data_buffer = arg->mp3_data_buffer;
        song_in_loop->mp3_data_buffer_length = arg->mp3_data_buffer_length;
        DBG_MUSIC_BOT log_info("%s %s %s", "musicbot__add_song added song -> ", arg->song_name, "\n");
        clib__copy_memory((void*)arg->song_name, song_in_loop->song_name, clib__utf8_string_length(arg->song_name), SONG_NAME_MAX_LENGTH);
        clib__null_memory((void*)&mp3, sizeof(drmp3));

        /* Decode MP3 */
        mp3_data_buffer1 = song_in_loop->mp3_data_buffer;
        mp3_data_buffer_length1 = song_in_loop->mp3_data_buffer_length;

        clib__unlock(&g_clients_global_rwlock_guard);

        /* unlock write lock here because drmp3_init_memory is very time consuming
           it's called because I wanted to find out what is the length of the mp3 in seconds */

        /* add is being_processed to song in loop so other song won't screw it */

        drmp3_init_memory(&mp3, mp3_data_buffer1, mp3_data_buffer_length1, NULL_POINTER);
        frameCount = drmp3_get_pcm_frame_count(&mp3);
        seconds_length = frameCount / mp3.sampleRate;
        DBG_MUSIC_BOT log_info("%s", "music bot add song success");
        drmp3_uninit(&mp3);

        /* do check here, again */
        clib__write_lock(&g_clients_global_rwlock_guard);

        status = util__is_client_valid_musicbot(arg->music_bot->client_id);

        if (status == FALSE)
        {
            memorymanager__free((nuint)arg->mp3_data_buffer);
            memorymanager__free((nuint)arg);
            clib__unlock(&g_clients_global_rwlock_guard);
            break;
        }

        /* at this point read lock is still active */
        song_in_loop = &arg->music_bot->music_bot_client_extension.songs[i];
        if (song_in_loop->is_existing == TRUE)
        {
            clib__unlock(&g_clients_global_rwlock_guard);
            continue;
        }

        song_in_loop->length_seconds = seconds_length;
        arg->music_bot->music_bot_client_extension.music_bot_songs_count++;
        song_in_loop->is_existing = TRUE;
        song_in_loop->is_being_uploaded = FALSE;

        is_song_added_succesfully = TRUE;

        clib__unlock(&g_clients_global_rwlock_guard);

        break;
    }

    if (is_song_added_succesfully == TRUE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);

        /* server_msg__send_music_bot_song_list_to_single_client checks validity of client */
        server_msg__send_music_bot_song_list_to_single_client(arg->sender_client_id, arg->music_bot->client_id);

        clib__unlock(&g_clients_global_rwlock_guard);
        DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__send_music_bot_song_list_to_single_client");

        memorymanager__free((nuint)arg);
    }
}

/**
 * @brief music bot playback thread; repeatedly walks the bot's song list and
 *        streams each song to its channel as Opus frames over the WebRTC datachannel
 *
 * @param client_t* music_bot_client -> the client_t that represents the music bot
 *
 * @return void
 */
void musicbot__threadstart(client_t* music_bot_client)
{
    drmp3 mp3_decoder;
    music_bot_single_song_data_t* current_song = 0;
    int64 song_index = 0;
    const int64 opus_sample_rate = 48000; /* Opus always runs at 48 kHz */
    const int64 frame_size = 960;         /* samples per frame, 20 ms @ 48 kHz */

    drmp3_uint64 decoded_frame_count = 0;
    float* decoded_pcm = NULL_POINTER;    /* float PCM decoded straight from the mp3 */
    drmp3_uint64 resampled_frame_count = 0;
    float* resampled_pcm = NULL_POINTER;  /* decoded PCM resampled to 48 kHz */
    opus_int16* pcm_int16 = NULL_POINTER; /* 48 kHz PCM converted to int16 for the encoder */

    OpusEncoder* opus_encoder = NULL_POINTER;
    int opus_error = 0; /* opus_encoder_create writes an int through this */
    unsigned char opus_packet[2048];      /* holds one encoded frame */
    opus_int16 frame_samples[960 * 2];    /* exactly frame_size samples, up to 2 channels */
    int64 encoded_byte_count = 0;

    uint64 song_length_seconds = 0;
    uint64 timestamp_started_playing = 0;
    uint64 timestamp_stopped_playing = 0;
    uint64 remaining_play_ms = 0;
    uint64 frames_done = 0;
    uint64 pacing_target_ms = 0;
    uint64 pacing_now_ms = 0;
    boole is_stop_reason_sudden_song_deletion = FALSE;

    double source_sample_position = 0;
    drmp3_uint64 source_index = 0;
    double interpolation_fraction = 0;
    drmp3_uint64 resample_index = 0;
    drmp3_uint64 sample_index = 0;
    drmp3_uint64 available_samples = 0;
    float sample = 0.0f;

    /* pacing diagnostic (DBG_MUSIC_BOT): whether the bot keeps up with real time */
    uint64 probe_song_start = 0;
    uint64 probe_frames_sent = 0;
    uint64 probe_elapsed = 0;
    uint64 probe_audio_ms = 0;

    

    music_bot_client->music_bot_client_extension.music_bot_songs_count = 0;

    while (music_bot_client->music_bot_client_extension.is_music_bot_running == TRUE)
    {
        base__sleep_for_milliseconds(1000);

        for (song_index = 0; song_index < music_bot_client->music_bot_client_extension.music_bot_songs_count; song_index++)
        {
            /* stop promptly if the bot was shut down (e.g. deleted) while looping */
            if (music_bot_client->music_bot_client_extension.is_music_bot_running == FALSE)
            {
                break;
            }

            current_song = &music_bot_client->music_bot_client_extension.songs[song_index];

            if (current_song->is_existing == FALSE)
            {
                music_bot_client->music_bot_client_extension.is_music_bot_running = FALSE;
                DBG_MUSIC_BOT log_info("%s", "BEEP, something is very wrong here, this should not happen \n");
                break;
            }

            /* decode the whole mp3 into float PCM */
            clib__null_memory((void*)&mp3_decoder, sizeof(drmp3));
            drmp3_init_memory(&mp3_decoder, current_song->mp3_data_buffer, current_song->mp3_data_buffer_length, NULL_POINTER);

            decoded_frame_count = drmp3_get_pcm_frame_count(&mp3_decoder);
            decoded_pcm = (float*)memorymanager__allocate(decoded_frame_count * mp3_decoder.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);
            drmp3_read_pcm_frames_f32(&mp3_decoder, decoded_frame_count, decoded_pcm);

            /* reject songs that are empty or unreasonably long */
            song_length_seconds = decoded_frame_count / mp3_decoder.sampleRate;
            if (song_length_seconds == 0 || song_length_seconds > 1000)
            {
                goto label_single_music_bot_thread_end;
            }

            DBG_MUSIC_BOT log_info("%s %llu %s", "seconds length is ->", song_length_seconds, "\n");

            /* resample to 48 kHz if the source rate differs (linear interpolation) */
            resampled_pcm = decoded_pcm;
            resampled_frame_count = decoded_frame_count;

            if (mp3_decoder.sampleRate != opus_sample_rate)
            {
                resampled_frame_count = (drmp3_uint64)(decoded_frame_count * (double)opus_sample_rate / mp3_decoder.sampleRate);
                resampled_pcm = (float*)memorymanager__allocate(resampled_frame_count * mp3_decoder.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);

                for (resample_index = 0; resample_index < resampled_frame_count; resample_index++)
                {
                    source_sample_position = resample_index * ((double)mp3_decoder.sampleRate / opus_sample_rate);
                    source_index = (drmp3_uint64)source_sample_position;
                    interpolation_fraction = source_sample_position - source_index;

                    for (uint64 channel = 0; channel < mp3_decoder.channels; channel++)
                    {
                        if (source_index + 1 < decoded_frame_count)
                        {
                            resampled_pcm[resample_index * mp3_decoder.channels + channel] = decoded_pcm[source_index * mp3_decoder.channels + channel] * (1.0 - interpolation_fraction) + decoded_pcm[(source_index + 1) * mp3_decoder.channels + channel] * interpolation_fraction;
                        }
                        else
                        {
                            resampled_pcm[resample_index * mp3_decoder.channels + channel] = decoded_pcm[source_index * mp3_decoder.channels + channel];
                        }
                    }
                }
            }

            /* create the Opus encoder for this song */
            opus_error = 0;
            opus_encoder = opus_encoder_create(opus_sample_rate, mp3_decoder.channels, OPUS_APPLICATION_AUDIO, &opus_error);
            if (opus_error != OPUS_OK)
            {
                log_info("%s", "opus_encoder_create failed \n");
                return;
            }
            opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(96000));

            /* convert the float PCM to clipped int16, which is what Opus encodes */
            pcm_int16 = (opus_int16*)memorymanager__allocate(resampled_frame_count * mp3_decoder.channels * sizeof(opus_int16), MEMALLOC_MUSICBOT_SONG);
            for (sample_index = 0; sample_index < resampled_frame_count * mp3_decoder.channels; sample_index++)
            {
                sample = resampled_pcm[sample_index];
                if (sample > 1.0f)
                {
                    sample = 1.0f;
                }
                if (sample < -1.0f)
                {
                    sample = -1.0f;
                }
                pcm_int16[sample_index] = (opus_int16)(sample * 32767.0f);
            }

            /* announce the song, then stream it frame by frame */
            clib__null_memory(opus_packet, sizeof(opus_packet));
            timestamp_started_playing = base__get_timestamp_ms();

            clib__null_memory(music_bot_client->song_name, SONG_NAME_MAX_LENGTH);
            clib__copy_memory(current_song->song_name, music_bot_client->song_name, clib__utf8_string_length(current_song->song_name), SONG_NAME_MAX_LENGTH);
            server_msg__send_start_song_stream_message_to_clients_in_same_channel(music_bot_client);

            is_stop_reason_sudden_song_deletion = FALSE;

            DBG_MUSIC_BOT
            {
                probe_song_start = base__get_timestamp_ms();
                probe_frames_sent = 0;
            }

            for (drmp3_uint64 frame_offset = 0; frame_offset < resampled_frame_count; frame_offset += frame_size)
            {
                /* stop mid-song if the bot was shut down or the song was deleted */
                if (music_bot_client->music_bot_client_extension.is_music_bot_running == FALSE || music_bot_client->music_bot_client_extension.songs[song_index].is_existing == FALSE)
                {
                    is_stop_reason_sudden_song_deletion = TRUE;
                    break;
                }

                /* always encode exactly frame_size samples, zero-padding the last frame */
                clib__null_memory(frame_samples, sizeof(frame_samples));

                available_samples = resampled_frame_count - frame_offset;
                if (available_samples > frame_size)
                {
                    available_samples = frame_size;
                }

                clib__copy_memory((void*)(pcm_int16 + frame_offset * mp3_decoder.channels), (void*)frame_samples, available_samples * mp3_decoder.channels * sizeof(opus_int16), available_samples * mp3_decoder.channels * sizeof(opus_int16));

                encoded_byte_count = opus_encode(opus_encoder, frame_samples, frame_size, opus_packet, sizeof(opus_packet));
                if (encoded_byte_count < 0)
                {
                    DBG_MUSIC_BOT log_info("%s", "opus_encode failed \n");
                    break;
                }

                audio_channel__send_music_bot_data(music_bot_client->channel_id, opus_packet, encoded_byte_count);
                DBG_MUSIC_BOT probe_frames_sent++;

                /* pace to real time: after sending frame K we should be at started + (K+1)*20ms.
                   sleep only if we are ahead; if behind, send the next frame immediately to catch up.
                   this self-corrects sleep overshoot / load instead of accumulating drift like a fixed sleep did */
                frames_done = (frame_offset / frame_size) + 1;
                pacing_target_ms = timestamp_started_playing + frames_done * 20;
                pacing_now_ms = base__get_timestamp_ms();
                if (pacing_target_ms > pacing_now_ms)
                {
                    base__sleep_for_milliseconds(pacing_target_ms - pacing_now_ms);
                }
            }

            /* pacing diagnostic (DBG_MUSIC_BOT): each frame is 20 ms of audio, so if wall-clock
               exceeds the audio duration the bot is streaming slower than real time */
            DBG_MUSIC_BOT
            {
                probe_elapsed = base__get_timestamp_ms() - probe_song_start;
                probe_audio_ms = probe_frames_sent * 20;
                log_info("%s %llu %s %llu %s %llu %s %s %s", "MUSICBOT PACING frames", probe_frames_sent, "audio_ms", probe_audio_ms, "wall_ms", probe_elapsed, "status", probe_elapsed > probe_audio_ms ? "behind" : "ok", "\n");
            }

label_single_music_bot_thread_end:

            /* release this song's buffers and encoder */
            memorymanager__free((nuint)pcm_int16);
            if (resampled_pcm != decoded_pcm)
            {
                memorymanager__free((nuint)resampled_pcm);
            }
            memorymanager__free((nuint)decoded_pcm);
            opus_encoder_destroy(opus_encoder);
            drmp3_uninit(&mp3_decoder);

            /* if the song ended early, sleep out the rest of its real duration before the next one */
            timestamp_stopped_playing = base__get_timestamp_ms();
            if (song_length_seconds > 0 && is_stop_reason_sudden_song_deletion == FALSE)
            {
                if ((song_length_seconds * 1000) > (timestamp_stopped_playing - timestamp_started_playing))
                {
                    remaining_play_ms = (song_length_seconds * 1000) - (timestamp_stopped_playing - timestamp_started_playing);
                    DBG_MUSIC_BOT log_info("%s %llu %s", "sleeping for ->", remaining_play_ms, "ms \n");
                    base__sleep_for_milliseconds(remaining_play_ms);
                }
            }
        }
    }
}