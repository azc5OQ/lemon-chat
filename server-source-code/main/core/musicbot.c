#include "../definitions.h"
#include "../crypto/ciphers.h"

#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/libopus-1.6.1/include/opus.h"
#include "../../third-party/rxi-log/log.h"
#define DR_MP3_IMPLEMENTATION
#include "../../third-party/dr_mp3/dr_mp3.h"

#include "../base.h"

#include "audio_channel.h"
#include "musicbot.h"
#include "client_session.h"
#include "permissions.h"
#include "../clib/clib.h"
#include "../memory_manager.h"
#include "../msg/server/server_msg.h"
#include "../util.h"

// declarations
static boole _musicbot_internal__song_matches(client_t *client, int64 song_index, uint64 generation);
static void _musicbot_internal__prepare_song(client_t *music_bot_client, int64 song_index, musicbot_prepared_song_t *out_prepared);
static void *_musicbot_internal__prepare_thread(void *arg_void);
static void *_musicbot_internal__reaper_thread(void *arg_void);
static boole _musicbot_internal__wait_for_playback(client_t *client, uint64 *playback_start_ms, int64 song_index, uint64 generation);

// These helpers require the clients and channels write locks. Allocation and
// thread startup are separate so restoring a playlist can roll back cleanly.
client_t *musicbot__allocate(uint64 channel_id, const char *name)
{
    int64 index = 0;
    client_t *bot = NULL_POINTER;

    if (channel_id >= g_server_settings.max_channel_count || !g_channel_array[channel_id].is_existing || g_channel_array[channel_id].is_temp_channel || name == NULL_POINTER || name[0] == 0 || strlen(name) >= USERNAME_MAX_LENGTH)
    {
        return NULL_POINTER;
    }
#ifndef MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL
    if (g_channel_array[channel_id].is_music_bot_active_in_channel)
    {
        return NULL_POINTER;
    }
#endif
    index = client_session__get_new_index_for_client();
    if (index < 0)
    {
        return NULL_POINTER;
    }
    bot = &g_clients_array[index];
    bot->music_bot_client_extension.songs = (music_bot_single_song_data_t *)memorymanager__allocate(sizeof(music_bot_single_song_data_t) * MUSIC_BOT_MAX_FILE_COUNT, MEMALLOC_MUSICBOT_SONG);
    bot->music_bot_client_extension.audio_keys = calloc(1, sizeof(music_bot_audio_keys_t));
    if (bot->music_bot_client_extension.songs == NULL_POINTER || bot->music_bot_client_extension.audio_keys == NULL_POINTER || !util__fill_secure_random_bytes((ubyte *)bot->music_bot_client_extension.audio_keys, sizeof(music_bot_audio_keys_t)))
    {
        memorymanager__free((nuint)bot->music_bot_client_extension.songs);
        free(bot->music_bot_client_extension.audio_keys);
        memset(bot, 0, sizeof(*bot));
        return NULL_POINTER;
    }
    bot->is_authenticated = TRUE;
    bot->is_existing = TRUE;
    bot->is_music_bot = TRUE;
    bot->is_streaming_song = TRUE;
    bot->client_id = (uint64)index;
    bot->channel_id = channel_id;
    bot->timestamp_connected = util__get_timestamp_ms();
    bot->audio_state = AUDIO_STATE__PUSH_TO_TALK_ACTIVE;
    clib__string_copy(bot->username, sizeof(bot->username), (char *)name);
    atomic_init(&bot->music_bot_client_extension.is_music_bot_running, FALSE);
    atomic_init(&bot->music_bot_client_extension.is_restore_pending, FALSE);
    atomic_init(&bot->music_bot_client_extension.is_playback_paused, FALSE);
    atomic_init(&bot->music_bot_client_extension.is_skip_requested, FALSE);
    g_channel_array[channel_id].is_music_bot_active_in_channel = TRUE;
    return bot;
}

boole musicbot__start(client_t *bot)
{
    atomic_store(&bot->music_bot_client_extension.is_music_bot_running, TRUE);
    if (pthread_create((pthread_t *)&bot->music_bot_client_extension.music_bot_pthread_handle, NULL_POINTER, musicbot__threadstart, bot) != 0)
    {
        atomic_store(&bot->music_bot_client_extension.is_music_bot_running, FALSE);
        return FALSE;
    }
    return TRUE;
}

// Only use for an unstarted bot or after joining its worker. Uploads must not
// own this slot. The restoration path has not published the bot to clients yet.
void musicbot__free_unstarted(client_t *bot)
{
    uint64 song_index = 0;
    music_bot_single_song_data_t *song = NULL_POINTER;
    uint64 index = 0;
    uint64 channel_id = bot->channel_id;

    for (song_index = 0; song_index < MUSIC_BOT_MAX_FILE_COUNT; song_index++)
    {
        song = &bot->music_bot_client_extension.songs[song_index];
        memorymanager__free((nuint)song->mp3_data_buffer);
    }
    memorymanager__free((nuint)bot->music_bot_client_extension.songs);
    ciphers__clear(bot->music_bot_client_extension.audio_keys, sizeof(music_bot_audio_keys_t));
    free(bot->music_bot_client_extension.audio_keys);
    memset(bot, 0, sizeof(*bot));
    g_channel_array[channel_id].is_music_bot_active_in_channel = FALSE;
    for (index = 0; index < g_server_settings.max_client_count; index++)
    {
        if (g_clients_array[index].is_existing && g_clients_array[index].is_music_bot && g_clients_array[index].channel_id == channel_id)
        {
            g_channel_array[channel_id].is_music_bot_active_in_channel = TRUE;
        }
    }
}

/**
 * @brief Waits while playback is paused and keeps the playback clock current.
 * @param client Music bot whose controls are atomic.
 * @param playback_start_ms Clock adjusted by the time spent paused.
 */
static boole _musicbot_internal__wait_for_playback(client_t *client, uint64 *playback_start_ms, int64 song_index, uint64 generation)
{
    uint64 pause_start = 0;
    uint64 pause_end = 0;

    if (atomic_load(&client->music_bot_client_extension.is_playback_paused))
    {
        pause_start = util__get_timestamp_ms();
        while (atomic_load(&client->music_bot_client_extension.is_playback_paused) && _musicbot_internal__song_matches(client, song_index, generation) && !atomic_load(&client->music_bot_client_extension.is_skip_requested))
        {
            util__sleep_for_milliseconds(50);
        }
        pause_end = util__get_timestamp_ms();
        if (pause_end > pause_start)
        {
            *playback_start_ms += pause_end - pause_start;
        }
    }
    return _musicbot_internal__song_matches(client, song_index, generation) && !atomic_exchange(&client->music_bot_client_extension.is_skip_requested, FALSE);
}

static boole _musicbot_internal__working_set_allowed(uint64 compressed, uint64 frames, uint64 rate, uint64 channels)
{
    uint64 resampled = 0;
    uint64 working = 0;

    if (compressed > 256ULL * 1024 * 1024 || rate < 8000 || rate > 192000 || channels < 1 || channels > 2 || frames == 0 || frames > rate * 1001)
    {
        return FALSE;
    }
    resampled = frames * 48000 / rate;
    working = compressed + frames * channels * sizeof(float) + resampled * channels * sizeof(opus_int16);
    if (rate != 48000)
    {
        working += resampled * channels * sizeof(float);
    }
    return working <= 256ULL * 1024 * 1024;
}

/**
 * @brief decodes one uploaded mp3 into 48 kHz int16 PCM ready for the opus encoder: full mp3
 *        decode, catmull-rom cubic resample to 48 kHz when the source rate differs, float ->
 *        clipped int16. rejects corrupt files, empty songs and songs longer than 1000 seconds.
 *
 * @param client_t* music_bot_client -> the bot whose song slot to prepare
 * @param int64 song_index -> index of the song slot to prepare
 * @param musicbot_prepared_song_t* out_prepared -> receives the buffers and counts; is_valid says if usable
 *
 * @note runs on the bot or preload thread, decoding a private copy taken under the clients lock.
 *
 * @return void
 */
static void _musicbot_internal__prepare_song(client_t *music_bot_client, int64 song_index, musicbot_prepared_song_t *out_prepared)
{
    drmp3 mp3_decoder;
    music_bot_single_song_data_t *song = NULL_POINTER;
    ubyte *mp3_copy = NULL_POINTER;
    uint64 mp3_copy_length = 0;
    drmp3_uint64 decoded_frame_count = 0;
    float *decoded_pcm = NULL_POINTER;
    drmp3_uint64 resampled_frame_count = 0;
    float *resampled_pcm = NULL_POINTER;
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

    clib__null_memory((void *)out_prepared, sizeof(musicbot_prepared_song_t));
    out_prepared->song_index = song_index;

    // snapshot the mp3 into our own buffer under the clients read lock: remove-song and delete-bot free
    // the slot's buffer under the write lock, so the (long) decode below must never touch the shared
    // buffer directly - it works on this private copy
    clib__read_lock(&g_clients_global_rwlock_guard);

    if (song_index < 0 || song_index >= MUSIC_BOT_MAX_FILE_COUNT || !music_bot_client->music_bot_client_extension.is_music_bot_running)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    song = &music_bot_client->music_bot_client_extension.songs[song_index];

    if (song->is_existing == FALSE || song->mp3_data_buffer == NULL_POINTER || song->mp3_data_buffer_length == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    out_prepared->generation = song->generation;
    clib__string_copy(out_prepared->song_name, sizeof(out_prepared->song_name), song->song_name);
    mp3_copy_length = song->mp3_data_buffer_length;
    mp3_copy = (ubyte *)memorymanager__allocate(mp3_copy_length, MEMALLOC_MUSICBOT_SONG);

    if (mp3_copy == NULL_POINTER)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    clib__copy_memory((void *)song->mp3_data_buffer, (void *)mp3_copy, mp3_copy_length, mp3_copy_length);

    clib__unlock(&g_clients_global_rwlock_guard);

    // decode the whole mp3 into float PCM
    clib__null_memory((void *)&mp3_decoder, sizeof(drmp3));
    if (!drmp3_init_memory(&mp3_decoder, mp3_copy, mp3_copy_length, NULL_POINTER))
    {
        memorymanager__free((nuint)mp3_copy);
        return;
    }

    decoded_frame_count = drmp3_get_pcm_frame_count(&mp3_decoder);

    // reject files dr_mp3 could not make sense of (also avoids a division by zero below)
    if (decoded_frame_count == 0 || mp3_decoder.sampleRate == 0 || mp3_decoder.channels == 0 || mp3_decoder.channels > 2)
    {
        drmp3_uninit(&mp3_decoder);
        memorymanager__free((nuint)mp3_copy);
        return;
    }

    out_prepared->channels = mp3_decoder.channels;
    out_prepared->song_length_seconds = decoded_frame_count / mp3_decoder.sampleRate;

    // reject songs that are empty or unreasonably long
    if (out_prepared->song_length_seconds == 0 || out_prepared->song_length_seconds > 1000)
    {
        drmp3_uninit(&mp3_decoder);
        memorymanager__free((nuint)mp3_copy);
        return;
    }

    // Bound the complete working set before allocating PCM. The allocation manager
    // additionally limits all current/preloaded/preparing music buffers to 512 MiB.
    if (!_musicbot_internal__working_set_allowed(mp3_copy_length, decoded_frame_count, mp3_decoder.sampleRate, mp3_decoder.channels))
    {
        goto prepare_song_cleanup;
    }

    decoded_pcm = (float *)memorymanager__allocate(decoded_frame_count * mp3_decoder.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);
    if (decoded_pcm == NULL_POINTER)
    {
        goto prepare_song_cleanup;
    }
    if (drmp3_read_pcm_frames_f32(&mp3_decoder, decoded_frame_count, decoded_pcm) != decoded_frame_count)
    {
        goto prepare_song_cleanup;
    }

    // resample to 48 kHz if the source rate differs. catmull-rom cubic over 4 taps: audibly cleaner
    // high end than the linear interpolation used before, still cheap and dependency-free
    resampled_pcm = decoded_pcm;
    resampled_frame_count = decoded_frame_count;

    if (mp3_decoder.sampleRate != opus_sample_rate)
    {
        resampled_frame_count = (drmp3_uint64)(decoded_frame_count * (double)opus_sample_rate / mp3_decoder.sampleRate);
        resampled_pcm = (float *)memorymanager__allocate(resampled_frame_count * mp3_decoder.channels * sizeof(float), MEMALLOC_MUSICBOT_SONG);
        if (resampled_pcm == NULL_POINTER)
        {
            goto prepare_song_cleanup;
        }

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

    // convert the float PCM to clipped int16, which is what Opus encodes
    out_prepared->pcm_int16 = (opus_int16 *)memorymanager__allocate(resampled_frame_count * mp3_decoder.channels * sizeof(opus_int16), MEMALLOC_MUSICBOT_SONG);
    if (out_prepared->pcm_int16 == NULL_POINTER)
    {
        goto prepare_song_cleanup;
    }

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
    out_prepared->is_valid = TRUE;

prepare_song_cleanup:
    if (resampled_pcm != decoded_pcm)
    {
        memorymanager__free((nuint)resampled_pcm);
    }
    memorymanager__free((nuint)decoded_pcm);
    drmp3_uninit(&mp3_decoder);
    memorymanager__free((nuint)mp3_copy);
}

/**
 * @brief preload thread: prepares the next song's PCM while the current song streams, so songs
 *        chain without a decode gap between them
 *
 * @param void* arg_void -> musicbot_prepare_arg_t owned by the bot thread; not freed here
 *
 * @return void* always 0
 */
static void *_musicbot_internal__prepare_thread(void *arg_void)
{
    musicbot_prepare_arg_t *arg = (musicbot_prepare_arg_t *)arg_void;

    _musicbot_internal__prepare_song(arg->music_bot_client, arg->song_index, &arg->result);

    return NULL_POINTER;
}

/**
 * @brief Waits for a deleted music bot's stream thread, frees its songs, and clears its client slot
 *        for reuse.
 *
 * @details Waits without holding locks. The stream thread joins its preload thread before returning, so neither thread can access the buffers or slot after cleanup.
 *
 * @param void* arg_void -> the music bot's client_t (slot stays reserved until this thread nulls it)
 *
 * @return void* always 0
 */
static void *_musicbot_internal__reaper_thread(void *arg_void)
{
    client_t *music_bot_client = (client_t *)arg_void;
    music_bot_single_song_data_t *single_song = NULL_POINTER;
    uint64 song_slot_index = 0;

    pthread_join((pthread_t)music_bot_client->music_bot_client_extension.music_bot_pthread_handle, NULL_POINTER);

    // Detached uploads also hold pointers into the song table. Their count is
    // changed under this lock, and they drop it only after their final access.
    for (;;)
    {
        clib__write_lock(&g_clients_global_rwlock_guard);
        if (music_bot_client->music_bot_client_extension.pending_song_uploads == 0)
        {
            break;
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        util__sleep_for_milliseconds(10);
    }

    for (song_slot_index = 0; song_slot_index < MUSIC_BOT_MAX_FILE_COUNT; song_slot_index++)
    {
        single_song = &music_bot_client->music_bot_client_extension.songs[song_slot_index];

        if (single_song->is_existing == TRUE && single_song->mp3_data_buffer != NULL_POINTER)
        {
            memorymanager__free((nuint)single_song->mp3_data_buffer);
        }
    }

    memorymanager__free((nuint)music_bot_client->music_bot_client_extension.songs);
    if (music_bot_client->music_bot_client_extension.audio_keys != NULL_POINTER)
    {
        clib__null_memory(music_bot_client->music_bot_client_extension.audio_keys, sizeof(music_bot_audio_keys_t));
        free(music_bot_client->music_bot_client_extension.audio_keys);
    }
    clib__null_memory((void *)music_bot_client, sizeof(client_t));

    clib__unlock(&g_clients_global_rwlock_guard);

    return NULL_POINTER;
}

/**
 * @brief Starts a detached cleanup thread, stops music playback, and hides the bot. The client slot
 *        stays reserved until cleanup finishes.
 *
 * @param client_t* music_bot_client -> the bot to delete
 *
 * @attention caller must hold the clients write lock. this function returns immediately; it never waits for the bot thread, so holding the lock here cannot stall the server
 *
 * @return boole FALSE if cleanup could not be started; the bot then remains usable for a later retry
 */
boole musicbot__begin_delete(client_t *music_bot_client)
{
    boole status = FALSE;

    // The caller holds the clients write lock, so cleanup cannot release the
    // table before these flags are set. If creation fails, leave the bot alive.
    status = util__start_detached_thread(_musicbot_internal__reaper_thread, music_bot_client);
    if (status == FALSE)
    {
        return FALSE;
    }

    music_bot_client->music_bot_client_extension.is_music_bot_running = FALSE;

    // hidden from client lists and every relay loop, but the slot stays reserved for the reaper
    music_bot_client->is_existing = FALSE;

    return TRUE;
}

/**
 * @brief removes a song from the music bot: frees its mp3 buffer, clears its slot, decrements the
 *        song count
 *
 * @param client_t* music_bot -> the client_t that represents the music bot
 * @param uint64 song_id -> index of the song slot to remove
 *
 * @attention questionable thread safety
 *
 * @return void
 */
void musicbot__remove_song(client_t *music_bot, uint64 song_id)
{
    boole clear_buffered_audio = FALSE;

    if (song_id >= MUSIC_BOT_MAX_FILE_COUNT || music_bot->music_bot_client_extension.songs == NULL_POINTER || music_bot->music_bot_client_extension.songs[song_id].is_existing == FALSE)
    {
        return;
    }
    clear_buffered_audio = music_bot->music_bot_client_extension.songs[song_id].is_currently_playing || music_bot->music_bot_client_extension.songs[song_id].playback_buffered_until_ms > util__get_timestamp_ms();
    music_bot->music_bot_client_extension.songs[song_id].is_existing = FALSE;

    if (music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer);
        music_bot->music_bot_client_extension.songs[song_id].mp3_data_buffer = 0;
    }

    // null out single music_bot_single_song_data_t entry
    clib__null_memory((void *)&music_bot->music_bot_client_extension.songs[song_id], sizeof(music_bot_single_song_data_t));

    if (music_bot->music_bot_client_extension.music_bot_songs_count > 0)
    {
        music_bot->music_bot_client_extension.music_bot_songs_count--;
    }

    // Removing an unplayed/preloaded track must not interrupt the audible one.
    if (clear_buffered_audio)
    {
        server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(music_bot);
    }
}

/**
 * @brief Adds an uploaded MP3 to a free music bot slot and sends the updated song list.
 *
 * @details Reserves the slot before unlocking to decode the MP3, then rechecks the bot under the write lock. A successful slot owns the MP3 buffer. On failure the buffer and argument are freed; on success only the argument is freed.
 *
 * @param musicbot_add_song_arg_struct_t* arg -> the upload: target bot, mp3 buffer and its length, song name, and the client_id of the uploader to answer. this function takes ownership of it and of the mp3 buffer
 *
 * @attention questionable thread safety; a client validity check macro should be added so the bot is verified before every access to it
 *
 * @return void
 */
void musicbot__add_song(musicbot_add_song_arg_struct_t *arg)
{
    boole decoder_initialized = FALSE;
    music_bot_single_song_data_t *song_in_loop = NULL_POINTER;
    drmp3 mp3;
    boole status = FALSE;
    boole is_song_added_succesfully = FALSE;
    void *mp3_data_buffer1 = NULL_POINTER;
    int64 mp3_data_buffer_length1 = 0;
    uint64 i = 0;
    drmp3_uint64 frameCount = 0;
    uint64 seconds_length = 0;

    // find free song index
    for (i = 0; i < MUSIC_BOT_MAX_FILE_COUNT; i++)
    {
        frameCount = 0;
        seconds_length = 0;

        // lock clients array and do all the checks if music bot is valid, if it song slot is valid
        // if all checks are passed assign buffer to the song

        clib__write_lock(&g_clients_global_rwlock_guard);

        status = base__is_client_valid_musicbot(arg->music_bot->client_id) && arg->music_bot->music_bot_client_extension.next_song_generation < UINT64_MAX && permissions__is_allowed(&g_clients_array[arg->sender_client_id], PERMISSION_UPLOAD_MUSIC_TRACKS) && clib__strcmp(g_clients_array[arg->sender_client_id].public_key, arg->sender_identity) == 0;

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

        song_in_loop->is_being_uploaded = TRUE; // so other thread won't start uploading to the same slot
        song_in_loop->mp3_data_buffer = arg->mp3_data_buffer;
        song_in_loop->mp3_data_buffer_length = arg->mp3_data_buffer_length;
        DBG_MUSIC_BOT log_info("%s %s %s", "musicbot__add_song added song -> ", arg->song_name, "\n");
        clib__string_copy(song_in_loop->song_name, sizeof(song_in_loop->song_name), arg->song_name);
        clib__null_memory((void *)&mp3, sizeof(drmp3));

        // Decode MP3
        mp3_data_buffer1 = song_in_loop->mp3_data_buffer;
        mp3_data_buffer_length1 = song_in_loop->mp3_data_buffer_length;

        clib__unlock(&g_clients_global_rwlock_guard);

        // unlock write lock here because drmp3_init_memory is very time consuming
        // it's called because I wanted to find out what is the length of the mp3 in seconds

        // add is being_processed to song in loop so other song won't screw it

        decoder_initialized = drmp3_init_memory(&mp3, mp3_data_buffer1, mp3_data_buffer_length1, NULL_POINTER) != 0;
        frameCount = decoder_initialized ? drmp3_get_pcm_frame_count(&mp3) : 0;

        // Reject failed initialization and songs that cannot fit preparation before accepting the upload.
        if (!decoder_initialized || !_musicbot_internal__working_set_allowed(mp3_data_buffer_length1, frameCount, mp3.sampleRate, mp3.channels))
        {
            DBG_MUSIC_BOT log_info("%s", "musicbot__add_song: undecodable mp3, rejecting song \n");
            if (decoder_initialized)
            {
                drmp3_uninit(&mp3);
            }

            // release the slot claimed above, it would stay burned otherwise
            clib__write_lock(&g_clients_global_rwlock_guard);
            status = base__is_client_valid_musicbot(arg->music_bot->client_id);
            if (status == TRUE)
            {
                song_in_loop->is_being_uploaded = FALSE;
                song_in_loop->mp3_data_buffer = NULL_POINTER;
                song_in_loop->mp3_data_buffer_length = 0;
                clib__null_memory(song_in_loop->song_name, SONG_NAME_MAX_LENGTH);
            }
            clib__unlock(&g_clients_global_rwlock_guard);

            memorymanager__free((nuint)arg->mp3_data_buffer);
            memorymanager__free((nuint)arg);
            break;
        }

        seconds_length = frameCount / mp3.sampleRate;
        DBG_MUSIC_BOT log_info("%s", "music bot add song success");
        drmp3_uninit(&mp3);

        // do check here, again
        clib__write_lock(&g_clients_global_rwlock_guard);

        status = base__is_client_valid_musicbot(arg->music_bot->client_id);

        if (status == FALSE)
        {
            memorymanager__free((nuint)arg->mp3_data_buffer);
            memorymanager__free((nuint)arg);
            clib__unlock(&g_clients_global_rwlock_guard);
            break;
        }

        if (!permissions__is_allowed(&g_clients_array[arg->sender_client_id], PERMISSION_UPLOAD_MUSIC_TRACKS) || clib__strcmp(g_clients_array[arg->sender_client_id].public_key, arg->sender_identity) != 0)
        {
            clib__memset(song_in_loop, 0, sizeof(*song_in_loop));
            memorymanager__free((nuint)arg->mp3_data_buffer);
            memorymanager__free((nuint)arg);
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }

        // at this point read lock is still active
        song_in_loop = &arg->music_bot->music_bot_client_extension.songs[i];
        if (song_in_loop->is_existing == TRUE)
        {
            clib__unlock(&g_clients_global_rwlock_guard);
            continue;
        }

        song_in_loop->generation = ++arg->music_bot->music_bot_client_extension.next_song_generation;
        song_in_loop->length_seconds = seconds_length;
        arg->music_bot->music_bot_client_extension.music_bot_songs_count++;
        song_in_loop->is_existing = TRUE;
        song_in_loop->is_being_uploaded = FALSE;

        is_song_added_succesfully = TRUE;

        clib__unlock(&g_clients_global_rwlock_guard);

        break;
    }

    if (i == MUSIC_BOT_MAX_FILE_COUNT && is_song_added_succesfully == FALSE)
    {
        memorymanager__free((nuint)arg->mp3_data_buffer);
        memorymanager__free((nuint)arg);
        return;
    }

    if (is_song_added_succesfully == TRUE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);

        // the requester may have left and the bot's slot may have been reused while the song loaded
        if (permissions__is_allowed(&g_clients_array[arg->sender_client_id], PERMISSION_UPLOAD_MUSIC_TRACKS) && clib__strcmp(g_clients_array[arg->sender_client_id].public_key, arg->sender_identity) == 0 && base__is_client_valid_musicbot(arg->music_bot->client_id) == TRUE)
        {
            server_msg__musicbot__send_song_list_to_single_client(&g_clients_array[arg->sender_client_id], arg->music_bot);
        }

        clib__unlock(&g_clients_global_rwlock_guard);
        DBG_CLIENT_MESSAGE log_info("%s", "calling server_msg__musicbot__send_song_list_to_single_client");

        memorymanager__free((nuint)arg);
    }
}

/**
 * @brief music bot playback thread; repeatedly walks the bot's song list and streams each song to
 *        its channel as Opus frames over the WebRTC datachannel
 *
 * @param void* argument -> the client_t that represents the music bot
 *
 * @return void* always NULL
 */
void *musicbot__threadstart(void *argument)
{
    uint64 channel_snapshot = 0;
    uint64 next_offset = 0;
    boole song_exists = FALSE;
    int64 song_index = 0;
    const int64 frame_size = 960; // samples per frame, 20 ms @ 48 kHz
    musicbot_prepared_song_t prepared_song = { 0 }; // the song currently being streamed
    musicbot_prepare_arg_t *preload_arg = NULL_POINTER; // in-flight preparation of the next song
    pthread_t preload_thread = 0;
    boole is_preload_running = FALSE;
    int64 preload_song_index = 0;
    OpusEncoder *opus_encoder = NULL_POINTER;
    int opus_error = 0; // opus_encoder_create writes an int through this
    ubyte opus_packet[2048] = { 0 }; // holds one encoded frame
    opus_int16 frame_samples[960 * 2] = { 0 }; // exactly frame_size samples, up to 2 channels
    int64 encoded_byte_count = 0;
    uint64 streamed_song_length_seconds = 0;
    uint64 sleep_slice_ms = 0;

    // The 16-bit frame sequence continues across songs and wraps when sent.
    uint64 song_stream_sequence_number = 0;
    uint64 timestamp_started_playing = 0;
    uint64 timestamp_stopped_playing = 0;
    uint64 remaining_play_ms = 0;

    // The receiver starts MUSIC_BOT_LEAD_MS behind the sender; consecutive songs keep that lead.
    uint64 song_playback_start_ms = 0;
    uint64 previous_song_end_playback_ms = 0;
    uint64 frames_done = 0;
    uint64 pacing_target_ms = 0;
    uint64 pacing_now_ms = 0;
    boole is_stop_reason_sudden_song_deletion = FALSE;
    drmp3_uint64 available_samples = 0;

    // DBG_MUSIC_BOT compares the audio duration with elapsed playback time.
    uint64 probe_song_start = 0;
    uint64 probe_frames_sent = 0;
    uint64 probe_elapsed = 0;
    uint64 probe_audio_ms = 0;
    drmp3_uint64 frame_offset = 0;
    boole status = FALSE;
    client_t *music_bot_client = (client_t *)argument;

    // A restore starts every worker behind this gate. Failed startup can stop
    // and join them while holding the clients lock because no worker takes it yet.
    while (atomic_load(&music_bot_client->music_bot_client_extension.is_restore_pending))
    {
        if (!atomic_load(&music_bot_client->music_bot_client_extension.is_music_bot_running))
        {
            return NULL_POINTER;
        }
        util__sleep_for_milliseconds(10);
    }

    clib__null_memory((void *)&prepared_song, sizeof(musicbot_prepared_song_t));

    while (music_bot_client->music_bot_client_extension.is_music_bot_running == TRUE)
    {
        util__sleep_for_milliseconds(1000);

        for (song_index = 0; song_index < MUSIC_BOT_MAX_FILE_COUNT; song_index++)
        {
            // stop promptly if the bot was shut down (e.g. deleted) while looping
            if (music_bot_client->music_bot_client_extension.is_music_bot_running == FALSE)
            {
                break;
            }

            clib__read_lock(&g_clients_global_rwlock_guard);
            song_exists = music_bot_client->music_bot_client_extension.songs[song_index].is_existing;
            clib__unlock(&g_clients_global_rwlock_guard);
            if (song_exists == FALSE)
            {
                continue;
            }

            // collect the preloaded PCM if it is for this exact song, otherwise prepare it right here.
            // the preload was started while the previous song streamed, so on the happy path this join
            // returns immediately and playback continues with no decode gap between songs
            if (is_preload_running == TRUE)
            {
                pthread_join(preload_thread, NULL_POINTER);
                is_preload_running = FALSE;
            }

            if (preload_arg != NULL_POINTER && preload_arg->result.is_valid == TRUE && preload_arg->song_index == song_index && _musicbot_internal__song_matches(music_bot_client, song_index, preload_arg->result.generation))
            {
                prepared_song = preload_arg->result;
            }
            else
            {
                // stale or failed preload (song list changed since it was started); drop it and prepare inline
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

            // unusable song (empty, corrupt or too long); skip it
            if (prepared_song.is_valid == FALSE)
            {
                clib__null_memory((void *)&prepared_song, sizeof(musicbot_prepared_song_t));
                continue;
            }

            DBG_MUSIC_BOT log_info("%s %llu %s", "seconds length is ->", prepared_song.song_length_seconds, "\n");

            // start preparing the NEXT song in the rotation while this one streams
            preload_song_index = -1;
            clib__read_lock(&g_clients_global_rwlock_guard);
            for (next_offset = 1; next_offset <= MUSIC_BOT_MAX_FILE_COUNT; next_offset++)
            {
                if (music_bot_client->music_bot_client_extension.songs[(song_index + next_offset) % MUSIC_BOT_MAX_FILE_COUNT].is_existing)
                {
                    preload_song_index = (song_index + next_offset) % MUSIC_BOT_MAX_FILE_COUNT;
                    break;
                }
            }
            clib__unlock(&g_clients_global_rwlock_guard);
            if (preload_song_index >= 0)
            {
                preload_arg = (musicbot_prepare_arg_t *)memorymanager__allocate(sizeof(musicbot_prepare_arg_t), MEMALLOC_MUSICBOT_SONG);
                if (preload_arg != NULL_POINTER)
                {
                    clib__null_memory(preload_arg, sizeof(*preload_arg));
                    preload_arg->music_bot_client = music_bot_client;
                    preload_arg->song_index = preload_song_index;
                    if (pthread_create(&preload_thread, NULL_POINTER, _musicbot_internal__prepare_thread, preload_arg) == 0)
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

            // create the Opus encoder for this song. 160 kbps + full complexity: the old 96 kbps was
            // audibly below the source quality for music
            opus_error = 0;
            opus_encoder = opus_encoder_create(48000, (int)prepared_song.channels, OPUS_APPLICATION_AUDIO, &opus_error);
            if (opus_error != OPUS_OK)
            {
                log_info("%s", "opus_encoder_create failed \n");
                memorymanager__free((nuint)prepared_song.pcm_int16);
                clib__null_memory((void *)&prepared_song, sizeof(musicbot_prepared_song_t));
                continue;
            }
            opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(160000));
            opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(10));

            // announce the song, then stream it frame by frame
            clib__null_memory(opus_packet, sizeof(opus_packet));
            timestamp_started_playing = util__get_timestamp_ms();

            // a song that follows straight on from the previous one starts where that one ends on the
            // playback clock (no second burst, no gap); after a pause it starts now, with a fresh burst
            if (previous_song_end_playback_ms > timestamp_started_playing)
            {
                song_playback_start_ms = previous_song_end_playback_ms;
            }
            else
            {
                song_playback_start_ms = timestamp_started_playing;
            }
            frames_done = 0;

            clib__write_lock(&g_clients_global_rwlock_guard);
            song_exists = music_bot_client->music_bot_client_extension.is_music_bot_running && music_bot_client->music_bot_client_extension.songs[song_index].is_existing && music_bot_client->music_bot_client_extension.songs[song_index].generation == prepared_song.generation;
            if (song_exists)
            {
                music_bot_client->music_bot_client_extension.songs[song_index].is_currently_playing = TRUE;
                channel_snapshot = music_bot_client->channel_id;
                clib__null_memory(music_bot_client->song_name, SONG_NAME_MAX_LENGTH);
                clib__string_copy(music_bot_client->song_name, sizeof(music_bot_client->song_name), prepared_song.song_name);
                server_msg__musicbot__send_start_song_stream_message_to_clients_in_same_channel(music_bot_client);
            }
            clib__unlock(&g_clients_global_rwlock_guard);

            is_stop_reason_sudden_song_deletion = FALSE;

            DBG_MUSIC_BOT
            {
                probe_song_start = util__get_timestamp_ms();
                probe_frames_sent = 0;
            }

            for (frame_offset = 0; frame_offset < prepared_song.frame_count; frame_offset += frame_size)
            {
                status = _musicbot_internal__wait_for_playback(music_bot_client, &song_playback_start_ms, song_index, prepared_song.generation);
                if (status == FALSE)
                {
                    is_stop_reason_sudden_song_deletion = TRUE;
                    break;
                }

                // stop mid-song if the bot was shut down or the song was deleted
                status = _musicbot_internal__song_matches(music_bot_client, song_index, prepared_song.generation);
                if (status == FALSE)
                {
                    is_stop_reason_sudden_song_deletion = TRUE;
                    break;
                }

                // always encode exactly frame_size samples, zero-padding the last frame
                clib__null_memory(frame_samples, sizeof(frame_samples));

                available_samples = prepared_song.frame_count - frame_offset;
                if (available_samples > frame_size)
                {
                    available_samples = frame_size;
                }

                clib__copy_memory((void *)(prepared_song.pcm_int16 + frame_offset * prepared_song.channels), (void *)frame_samples, available_samples * prepared_song.channels * sizeof(opus_int16), available_samples * prepared_song.channels * sizeof(opus_int16));

                encoded_byte_count = opus_encode(opus_encoder, frame_samples, frame_size, opus_packet, sizeof(opus_packet));
                if (encoded_byte_count < 0)
                {
                    DBG_MUSIC_BOT log_info("%s", "opus_encode failed \n");
                    break;
                }

                audio_channel__send_music_bot_data(music_bot_client->client_id, channel_snapshot, song_stream_sequence_number, opus_packet, encoded_byte_count, song_index, prepared_song.generation);
                song_stream_sequence_number = (song_stream_sequence_number + 1) & 0xffff;
                DBG_MUSIC_BOT probe_frames_sent++;

                // pace to real time: after sending frame K we should be at started + (K+1)*20ms.
                // sleep only if we are ahead; if behind, send the next frame immediately to catch up.
                // this self-corrects sleep overshoot / load instead of accumulating drift like a fixed sleep did.
                // "ahead" means more than MUSIC_BOT_LEAD_MS ahead of the playback clock: the first
                // lead's worth of frames goes out at once, from then on one frame per 20 ms
                frames_done = (frame_offset / frame_size) + 1;
                pacing_target_ms = song_playback_start_ms + frames_done * 20;
                pacing_now_ms = util__get_timestamp_ms();
                if (pacing_target_ms > pacing_now_ms + MUSIC_BOT_LEAD_MS)
                {
                    util__sleep_for_milliseconds(pacing_target_ms - MUSIC_BOT_LEAD_MS - pacing_now_ms);
                }
            }

            if (is_stop_reason_sudden_song_deletion)
            {
                clib__read_lock(&g_clients_global_rwlock_guard);
                if (music_bot_client->is_existing && music_bot_client->is_music_bot)
                {
                    server_msg__musicbot__send_stop_song_stream_message_to_clients_in_same_channel(music_bot_client);
                }
                clib__unlock(&g_clients_global_rwlock_guard);
                previous_song_end_playback_ms = 0;
            }

            // pacing diagnostic (DBG_MUSIC_BOT): each frame is 20 ms of audio, so if wall-clock
            // exceeds the audio duration the bot is streaming slower than real time
            DBG_MUSIC_BOT
            {
                probe_elapsed = util__get_timestamp_ms() - probe_song_start;
                probe_audio_ms = probe_frames_sent * 20;
                log_info("%s %llu %s %llu %s %llu %s %s %s", "MUSICBOT PACING frames", probe_frames_sent, "audio_ms", probe_audio_ms, "wall_ms", probe_elapsed, "status", probe_elapsed > probe_audio_ms ? "behind" : "ok", "\n");
            }

            clib__write_lock(&g_clients_global_rwlock_guard);
            if (music_bot_client->music_bot_client_extension.songs[song_index].generation == prepared_song.generation)
            {
                music_bot_client->music_bot_client_extension.songs[song_index].is_currently_playing = FALSE;
                music_bot_client->music_bot_client_extension.songs[song_index].playback_buffered_until_ms = is_stop_reason_sudden_song_deletion ? 0 : song_playback_start_ms + frames_done * 20;
            }
            clib__unlock(&g_clients_global_rwlock_guard);

            // release this song's buffers and encoder
            streamed_song_length_seconds = prepared_song.song_length_seconds;
            memorymanager__free((nuint)prepared_song.pcm_int16);
            clib__null_memory((void *)&prepared_song, sizeof(musicbot_prepared_song_t));
            opus_encoder_destroy(opus_encoder);
            opus_encoder = NULL_POINTER;

            // the sender finished this song a lead's worth early. wait until the next song's first frame
            // is due on the playback clock (its end minus the lead), so the lead carries over the
            // boundary; a cut-short song hands over from where it stopped. sliced sleep so a delete
            // request stops the bot within ~100 ms instead of after minutes
            previous_song_end_playback_ms = is_stop_reason_sudden_song_deletion ? 0 : song_playback_start_ms + frames_done * 20;
            timestamp_stopped_playing = util__get_timestamp_ms();
            if (is_stop_reason_sudden_song_deletion == FALSE)
            {
                if (previous_song_end_playback_ms > timestamp_stopped_playing + MUSIC_BOT_LEAD_MS)
                {
                    remaining_play_ms = previous_song_end_playback_ms - MUSIC_BOT_LEAD_MS - timestamp_stopped_playing;
                    DBG_MUSIC_BOT log_info("%s %llu %s %llu %s", "song of", streamed_song_length_seconds, "s done, sleeping for ->", remaining_play_ms, "ms \n");

                    while (remaining_play_ms > 0 && music_bot_client->music_bot_client_extension.is_music_bot_running == TRUE)
                    {
                        sleep_slice_ms = remaining_play_ms > 100 ? 100 : remaining_play_ms;
                        util__sleep_for_milliseconds(sleep_slice_ms);
                        remaining_play_ms = remaining_play_ms - sleep_slice_ms;
                    }
                }
            }
        }
    }

    // the bot is shutting down: collect and drop any preparation still running so nothing leaks
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
    return NULL_POINTER;
}

/** @brief Checks that prepared audio still belongs to the occupied playlist slot. */
static boole _musicbot_internal__song_matches(client_t *client, int64 song_index, uint64 generation)
{
    boole matches = FALSE;

    clib__read_lock(&g_clients_global_rwlock_guard);
    matches = song_index >= 0 && song_index < MUSIC_BOT_MAX_FILE_COUNT && client->music_bot_client_extension.is_music_bot_running && client->music_bot_client_extension.songs != NULL_POINTER && client->music_bot_client_extension.songs[song_index].is_existing && client->music_bot_client_extension.songs[song_index].generation == generation;
    clib__unlock(&g_clients_global_rwlock_guard);
    return matches;
}
