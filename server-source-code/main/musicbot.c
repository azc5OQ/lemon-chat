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

/* result of preparing one song for streaming: 48 kHz int16 PCM ready for the opus encoder */
typedef struct musicbot_prepared_song_t
{
    boole is_valid;
    int64 song_index;
    opus_int16* pcm_int16;
    uint64 frame_count; /* frames at 48 kHz */
    uint64 channels;
    uint64 song_length_seconds;
} musicbot_prepared_song_t;

/* argument/result carrier for the preload thread; owned (allocated, joined, freed) by the bot thread */
typedef struct musicbot_prepare_arg_t
{
    client_t* music_bot_client;
    int64 song_index;
    musicbot_prepared_song_t result;
} musicbot_prepare_arg_t;

/**
 * @brief decodes one uploaded mp3 into 48 kHz int16 PCM ready for the opus encoder: full mp3 decode,
 *        catmull-rom cubic resample to 48 kHz when the source rate differs, float -> clipped int16.
 *        rejects corrupt files, empty songs and songs longer than 1000 seconds.
 *
 * @param client_t* music_bot_client -> the bot whose song slot to prepare
 * @param int64 song_index -> index of the song slot to prepare
 * @param musicbot_prepared_song_t* out_prepared -> receives the buffers and counts; is_valid says if usable
 *
 * @note runs on the bot thread (first song and fallback) or on the preload thread (next song prepared
 *       while the current one streams). reads the slot's mp3 buffer without a lock - the same exposure to
 *       a concurrent admin song-delete the previous inline decode had
 *
 * @return void
 */
static void _musicbot_internal__prepare_song(client_t* music_bot_client, int64 song_index, musicbot_prepared_song_t* out_prepared)
{
    drmp3 mp3_decoder;
    music_bot_single_song_data_t* song = NULL_POINTER;
    ubyte* mp3_copy = NULL_POINTER;
    uint64 mp3_copy_length = 0;
    drmp3_uint64 decoded_frame_count = 0;
    float* decoded_pcm = NULL_POINTER;
    drmp3_uint64 resampled_frame_count = 0;
    float* resampled_pcm = NULL_POINTER;
    const int64 opus_sample_rate = 48000;
    double source_sample_position = 0;
    drmp3_uint64 source_index = 0;
    double interpolation_fraction = 0;
    drmp3_uint64 resample_index = 0;
    drmp3_uint64 sample_index = 0;
    drmp3_uint64 last_frame_index = 0;
    drmp3_uint64 index_previous = 0;
    drmp3_uint64 index_next = 0;
    drmp3_uint64 index_after_next = 0;
    float tap0 = 0;
    float tap1 = 0;
    float tap2 = 0;
    float tap3 = 0;
    float sample = 0;
    uint64 channel = 0;

    clib__null_memory((void*)out_prepared, sizeof(musicbot_prepared_song_t));
    out_prepared->song_index = song_index;

    /* snapshot the mp3 into our own buffer under the clients read lock: remove-song and delete-bot free
       the slot's buffer under the write lock, so the (long) decode below must never touch the shared
       buffer directly - it works on this private copy */
    clib__read_lock(&g_clients_global_rwlock_guard);

    song = &music_bot_client->music_bot_client_extension.songs[song_index];

    if (song->is_existing == FALSE || song->mp3_data_buffer == NULL_POINTER || song->mp3_data_buffer_length == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    mp3_copy_length = song->mp3_data_buffer_length;
    mp3_copy = (ubyte*)memorymanager__allocate(mp3_copy_length, MEMALLOC_MUSICBOT_SONG);

    if (mp3_copy == NULL_POINTER)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__copy_memory((void*)song->mp3_data_buffer, (void*)mp3_copy, mp3_copy_length, mp3_copy_length);

    clib__unlock(&g_clients_global_rwlock_guard);

    /* decode the whole mp3 into float PCM */
    clib__null_memory((void*)&mp3_decoder, sizeof(drmp3));
    drmp3_init_memory(&mp3_decoder, mp3_copy, mp3_copy_length, NULL_POINTER);

    decoded_frame_count = drmp3_get_pcm_frame_count(&mp3_decoder);

    /* reject files dr_mp3 could not make sense of (also avoids a division by zero below) */
    if (decoded_frame_count == 0 || mp3_decoder.sampleRate == 0 || mp3_decoder.channels == 0 || mp3_decoder.channels > 2)
    {
        drmp3_uninit(&mp3_decoder);
        memorymanager__free((nuint)mp3_copy);
        return;
    }

    decoded_pcm = (float*)memorymanager__allocate(decoded_frame_count * mp3_decoder.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);
    drmp3_read_pcm_frames_f32(&mp3_decoder, decoded_frame_count, decoded_pcm);

    out_prepared->channels = mp3_decoder.channels;
    out_prepared->song_length_seconds = decoded_frame_count / mp3_decoder.sampleRate;

    /* reject songs that are empty or unreasonably long */
    if (out_prepared->song_length_seconds == 0 || out_prepared->song_length_seconds > 1000)
    {
        memorymanager__free((nuint)decoded_pcm);
        drmp3_uninit(&mp3_decoder);
        memorymanager__free((nuint)mp3_copy);
        return;
    }

    /* resample to 48 kHz if the source rate differs. catmull-rom cubic over 4 taps: audibly cleaner
       high end than the linear interpolation used before, still cheap and dependency-free */
    resampled_pcm = decoded_pcm;
    resampled_frame_count = decoded_frame_count;

    if (mp3_decoder.sampleRate != opus_sample_rate)
    {
        resampled_frame_count = (drmp3_uint64)(decoded_frame_count * (double)opus_sample_rate / mp3_decoder.sampleRate);
        resampled_pcm = (float*)memorymanager__allocate(resampled_frame_count * mp3_decoder.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);

        last_frame_index = decoded_frame_count - 1;

        for (resample_index = 0; resample_index < resampled_frame_count; resample_index++)
        {
            source_sample_position = resample_index * ((double)mp3_decoder.sampleRate / opus_sample_rate);
            source_index = (drmp3_uint64)source_sample_position;

            if (source_index > last_frame_index)
            {
                source_index = last_frame_index;
            }

            interpolation_fraction = source_sample_position - source_index;

            index_previous = source_index > 0 ? source_index - 1 : 0;
            index_next = source_index < last_frame_index ? source_index + 1 : last_frame_index;
            index_after_next = source_index + 2 <= last_frame_index ? source_index + 2 : last_frame_index;

            for (channel = 0; channel < mp3_decoder.channels; channel++)
            {
                tap0 = decoded_pcm[index_previous * mp3_decoder.channels + channel];
                tap1 = decoded_pcm[source_index * mp3_decoder.channels + channel];
                tap2 = decoded_pcm[index_next * mp3_decoder.channels + channel];
                tap3 = decoded_pcm[index_after_next * mp3_decoder.channels + channel];

                resampled_pcm[resample_index * mp3_decoder.channels + channel] = (float)(tap1 + 0.5 * interpolation_fraction * (tap2 - tap0 + interpolation_fraction * (2.0 * tap0 - 5.0 * tap1 + 4.0 * tap2 - tap3 + interpolation_fraction * (3.0 * (tap1 - tap2) + tap3 - tap0))));
            }
        }
    }

    /* convert the float PCM to clipped int16, which is what Opus encodes */
    out_prepared->pcm_int16 = (opus_int16*)memorymanager__allocate(resampled_frame_count * mp3_decoder.channels * sizeof(opus_int16), MEMALLOC_MUSICBOT_SONG);

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
        out_prepared->pcm_int16[sample_index] = (opus_int16)(sample * 32767.0f);
    }

    out_prepared->frame_count = resampled_frame_count;

    if (resampled_pcm != decoded_pcm)
    {
        memorymanager__free((nuint)resampled_pcm);
    }
    memorymanager__free((nuint)decoded_pcm);
    drmp3_uninit(&mp3_decoder);
    memorymanager__free((nuint)mp3_copy);

    out_prepared->is_valid = TRUE;
}

/**
 * @brief preload thread: prepares the next song's PCM while the current song streams, so songs chain
 *        without a decode gap between them
 *
 * @param void* arg_void -> musicbot_prepare_arg_t owned by the bot thread; not freed here
 *
 * @return void* always 0
 */
static void* _musicbot_internal__prepare_thread(void* arg_void)
{
    musicbot_prepare_arg_t* arg = (musicbot_prepare_arg_t*)arg_void;

    _musicbot_internal__prepare_song(arg->music_bot_client, arg->song_index, &arg->result);

    return NULL_POINTER;
}

/**
 * @brief reaper thread for a deleted music bot: waits (holding no locks) for the bot's stream thread to
 *        exit - which itself joins its preload thread - and only then frees the song buffers and nulls
 *        the client_t, releasing the slot for reuse. freeing any of this earlier was a use-after-free:
 *        the stream/preload threads could still be reading the buffers, and nulling the client_t made the
 *        slot immediately reusable by a new connection while the old bot thread still wrote into it.
 *
 * @param void* arg_void -> the music bot's client_t (slot stays reserved until this thread nulls it)
 *
 * @return void* always 0
 */
static void* _musicbot_internal__reaper_thread(void* arg_void)
{
    client_t* music_bot_client = (client_t*)arg_void;
    music_bot_single_song_data_t* single_song = NULL_POINTER;
    uint64 song_slot_index = 0;

    pthread_join((pthread_t)music_bot_client->music_bot_client_extension.music_bot_pthread_handle, NULL_POINTER);

    /* the stream and preload threads are gone; nothing else touches a bot's songs */
    clib__write_lock(&g_clients_global_rwlock_guard);

    for (song_slot_index = 0; song_slot_index < MUSIC_BOT_MAX_FILE_COUNT; song_slot_index++)
    {
        single_song = &music_bot_client->music_bot_client_extension.songs[song_slot_index];

        if (single_song->is_existing == TRUE && single_song->mp3_data_buffer != NULL_POINTER)
        {
            memorymanager__free((nuint)single_song->mp3_data_buffer);
        }
    }

    clib__null_memory((void*)music_bot_client, sizeof(client_t));

    clib__unlock(&g_clients_global_rwlock_guard);

    return NULL_POINTER;
}

/**
 * @brief begins deleting a music bot: signals its stream thread to stop, hides the bot from lists and
 *        relays, and hands the actual cleanup to a detached reaper thread. the client slot stays reserved
 *        (timestamp_connected != 0) until the reaper nulls the client_t, so no new connection can claim it
 *        while the old threads are still winding down.
 *
 * @param client_t* music_bot_client -> the bot to delete
 *
 * @attention caller must hold the clients write lock. this function returns immediately; it never waits
 *            for the bot thread, so holding the lock here cannot stall the server
 *
 * @return void
 */
void musicbot__begin_delete(client_t* music_bot_client)
{
    pthread_t reaper_thread = 0;

    music_bot_client->music_bot_client_extension.is_music_bot_running = FALSE;

    /* hidden from client lists and every relay loop, but the slot stays reserved for the reaper */
    music_bot_client->is_existing = FALSE;

    if (pthread_create(&reaper_thread, 0, _musicbot_internal__reaper_thread, (void*)music_bot_client) == 0)
    {
        pthread_detach(reaper_thread);
    }
}

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
    music_bot_single_song_data_t* current_song = 0;
    int64 song_index = 0;
    const int64 frame_size = 960;         /* samples per frame, 20 ms @ 48 kHz */

    musicbot_prepared_song_t prepared_song;              /* the song currently being streamed */
    musicbot_prepare_arg_t* preload_arg = NULL_POINTER;  /* in-flight preparation of the next song */
    pthread_t preload_thread = 0;
    boole is_preload_running = FALSE;
    int64 preload_song_index = 0;

    OpusEncoder* opus_encoder = NULL_POINTER;
    int opus_error = 0; /* opus_encoder_create writes an int through this */
    unsigned char opus_packet[2048];      /* holds one encoded frame */
    opus_int16 frame_samples[960 * 2];    /* exactly frame_size samples, up to 2 channels */
    int64 encoded_byte_count = 0;

    uint64 streamed_song_length_seconds = 0;
    uint64 sleep_slice_ms = 0;
    /* per-bot-lifetime frame sequence (16 bits, wraps); continues across songs so receivers never see a
       false counter restart at a song boundary */
    uint64 song_stream_sequence_number = 0;
    uint64 timestamp_started_playing = 0;
    uint64 timestamp_stopped_playing = 0;
    uint64 remaining_play_ms = 0;
    uint64 frames_done = 0;
    uint64 pacing_target_ms = 0;
    uint64 pacing_now_ms = 0;
    boole is_stop_reason_sudden_song_deletion = FALSE;
    drmp3_uint64 available_samples = 0;

    /* pacing diagnostic (DBG_MUSIC_BOT): whether the bot keeps up with real time */
    uint64 probe_song_start = 0;
    uint64 probe_frames_sent = 0;
    uint64 probe_elapsed = 0;
    uint64 probe_audio_ms = 0;

    clib__null_memory((void*)&prepared_song, sizeof(musicbot_prepared_song_t));

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

            /* collect the preloaded PCM if it is for this exact song, otherwise prepare it right here.
               the preload was started while the previous song streamed, so on the happy path this join
               returns immediately and playback continues with no decode gap between songs */
            if (is_preload_running == TRUE)
            {
                pthread_join(preload_thread, NULL_POINTER);
                is_preload_running = FALSE;
            }

            if (preload_arg != NULL_POINTER && preload_arg->result.is_valid == TRUE && preload_arg->song_index == song_index && current_song->is_existing == TRUE)
            {
                prepared_song = preload_arg->result;
            }
            else
            {
                /* stale or failed preload (song list changed since it was started); drop it and prepare inline */
                if (preload_arg != NULL_POINTER && preload_arg->result.pcm_int16 != NULL_POINTER)
                {
                    memorymanager__free((nuint)preload_arg->result.pcm_int16);
                }
                _musicbot_internal__prepare_song(music_bot_client, song_index, &prepared_song);
            }

            if (preload_arg != NULL_POINTER)
            {
                memorymanager__free((nuint)preload_arg);
                preload_arg = NULL_POINTER;
            }

            /* unusable song (empty, corrupt or too long); skip it */
            if (prepared_song.is_valid == FALSE)
            {
                clib__null_memory((void*)&prepared_song, sizeof(musicbot_prepared_song_t));
                continue;
            }

            DBG_MUSIC_BOT log_info("%s %llu %s", "seconds length is ->", prepared_song.song_length_seconds, "\n");

            /* start preparing the NEXT song in the rotation while this one streams */
            if (music_bot_client->music_bot_client_extension.music_bot_songs_count > 0)
            {
                preload_song_index = (song_index + 1) % music_bot_client->music_bot_client_extension.music_bot_songs_count;

                if (music_bot_client->music_bot_client_extension.songs[preload_song_index].is_existing == TRUE)
                {
                    preload_arg = (musicbot_prepare_arg_t*)memorymanager__allocate(sizeof(musicbot_prepare_arg_t), MEMALLOC_MUSICBOT_SONG);

                    if (preload_arg != NULL_POINTER)
                    {
                        clib__null_memory((void*)preload_arg, sizeof(musicbot_prepare_arg_t));
                        preload_arg->music_bot_client = music_bot_client;
                        preload_arg->song_index = preload_song_index;

                        if (pthread_create(&preload_thread, 0, _musicbot_internal__prepare_thread, (void*)preload_arg) == 0)
                        {
                            is_preload_running = TRUE;
                        }
                        else
                        {
                            memorymanager__free((nuint)preload_arg);
                            preload_arg = NULL_POINTER;
                        }
                    }
                }
            }

            /* create the Opus encoder for this song. 160 kbps + full complexity: the old 96 kbps was
               audibly below the source quality for music */
            opus_error = 0;
            opus_encoder = opus_encoder_create(48000, (int)prepared_song.channels, OPUS_APPLICATION_AUDIO, &opus_error);
            if (opus_error != OPUS_OK)
            {
                log_info("%s", "opus_encoder_create failed \n");
                memorymanager__free((nuint)prepared_song.pcm_int16);
                clib__null_memory((void*)&prepared_song, sizeof(musicbot_prepared_song_t));
                continue;
            }
            opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(160000));
            opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(10));

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

            for (drmp3_uint64 frame_offset = 0; frame_offset < prepared_song.frame_count; frame_offset += frame_size)
            {
                /* stop mid-song if the bot was shut down or the song was deleted */
                if (music_bot_client->music_bot_client_extension.is_music_bot_running == FALSE || music_bot_client->music_bot_client_extension.songs[song_index].is_existing == FALSE)
                {
                    is_stop_reason_sudden_song_deletion = TRUE;
                    break;
                }

                /* always encode exactly frame_size samples, zero-padding the last frame */
                clib__null_memory(frame_samples, sizeof(frame_samples));

                available_samples = prepared_song.frame_count - frame_offset;
                if (available_samples > frame_size)
                {
                    available_samples = frame_size;
                }

                clib__copy_memory((void*)(prepared_song.pcm_int16 + frame_offset * prepared_song.channels), (void*)frame_samples, available_samples * prepared_song.channels * sizeof(opus_int16), available_samples * prepared_song.channels * sizeof(opus_int16));

                encoded_byte_count = opus_encode(opus_encoder, frame_samples, frame_size, opus_packet, sizeof(opus_packet));
                if (encoded_byte_count < 0)
                {
                    DBG_MUSIC_BOT log_info("%s", "opus_encode failed \n");
                    break;
                }

                audio_channel__send_music_bot_data(music_bot_client->client_id, music_bot_client->channel_id, song_stream_sequence_number, opus_packet, encoded_byte_count);
                song_stream_sequence_number = (song_stream_sequence_number + 1) & 0xffff;
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

            /* release this song's buffers and encoder */
            streamed_song_length_seconds = prepared_song.song_length_seconds;
            memorymanager__free((nuint)prepared_song.pcm_int16);
            clib__null_memory((void*)&prepared_song, sizeof(musicbot_prepared_song_t));
            opus_encoder_destroy(opus_encoder);
            opus_encoder = NULL_POINTER;

            /* if the song ended early, sleep out the rest of its real duration before the next one.
               sliced sleep so a delete request stops the bot within ~100 ms instead of after minutes */
            timestamp_stopped_playing = base__get_timestamp_ms();
            if (streamed_song_length_seconds > 0 && is_stop_reason_sudden_song_deletion == FALSE)
            {
                if ((streamed_song_length_seconds * 1000) > (timestamp_stopped_playing - timestamp_started_playing))
                {
                    remaining_play_ms = (streamed_song_length_seconds * 1000) - (timestamp_stopped_playing - timestamp_started_playing);
                    DBG_MUSIC_BOT log_info("%s %llu %s", "sleeping for ->", remaining_play_ms, "ms \n");

                    while (remaining_play_ms > 0 && music_bot_client->music_bot_client_extension.is_music_bot_running == TRUE)
                    {
                        sleep_slice_ms = remaining_play_ms > 100 ? 100 : remaining_play_ms;
                        base__sleep_for_milliseconds(sleep_slice_ms);
                        remaining_play_ms = remaining_play_ms - sleep_slice_ms;
                    }
                }
            }
        }
    }

    /* the bot is shutting down: collect and drop any preparation still in flight so nothing leaks */
    if (is_preload_running == TRUE)
    {
        pthread_join(preload_thread, NULL_POINTER);
    }
    if (preload_arg != NULL_POINTER)
    {
        if (preload_arg->result.pcm_int16 != NULL_POINTER)
        {
            memorymanager__free((nuint)preload_arg->result.pcm_int16);
        }
        memorymanager__free((nuint)preload_arg);
    }
}