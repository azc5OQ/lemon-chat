#include <stdlib.h>
#include <stdio.h>
#include "platform/thread.h"
#include "platform/system.h"
#include "cJSON.h"
#include "audio/sounds.h"
#include "miniaudio.h"
#include "mbedtls/base64.h"

#define SOUND_PLAYBACKS 128

typedef struct sound_clip_t
{
    float *pcm;
    ma_uint64 frames;
} sound_clip_t;

typedef struct sound_playback_t
{
    desktop_sound_t id;
    uint64 position;
    boole active;
} sound_playback_t;

struct desktop_sounds_t
{
    desktop_mutex_t *mutex;
    desktop_thread_t *watcher;
    boole stop;
    boole source_valid;
    char *path;
    char *source;
    sound_clip_t clips[DESKTOP_SOUND_COUNT];
    sound_playback_t playing[SOUND_PLAYBACKS];
};

static const char *const g_sound_names[DESKTOP_SOUND_COUNT] = {
    "connected", "user_entered_your_channel", "user_joined_your_channel", "channel_created", "channel_switched", "channel_edited", "channel_deleted", "user_left_your_channel", "user_disconnected_from_your_channel", "poke", "message_sent", "message_received", "call", "insufficient_permissions", "connection_lost", "disconnected", "error", "server_group_assigned", "server_group_revoked", "user_was_moved_out_of_your_channel", "user_was_moved_to_your_channel", "user_in_your_channel_was_banned_from_the_server", "user_was_kicked_to_your_channel", "user_was_kicked_out_of_your_channel", "you_edited_the_server", "microphone_muted", "microphone_activated", "you_were_kicked_from_the_server", "you_were_banned_from_the_server",
};

static boole _sounds_internal__decode(sound_clip_t *clip, const ubyte *bytes, size_t size)
{
    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 48000);
    ma_uint64 read = 0;
    boole valid = FALSE;
    if (ma_decoder_init_memory(bytes, size, &config, &decoder) != MA_SUCCESS)
    {
        return FALSE;
    }
    valid = ma_decoder_get_length_in_pcm_frames(&decoder, &clip->frames) == MA_SUCCESS && clip->frames > 0 && clip->frames <= 48000 * 30;
    if (valid)
    {
        clip->pcm = ma_malloc((size_t)clip->frames * sizeof(float), NULL);
        if (clip->pcm)
        {
            ma_decoder_read_pcm_frames(&decoder, clip->pcm, clip->frames, &read);
        }
        valid = clip->pcm && read > 0;
        clip->frames = read;
    }
    ma_decoder_uninit(&decoder);
    return valid;
}

// File reading and decoding run on the watcher, never on the speaker callback.
boole desktop_sounds__reload(desktop_sounds_t *sounds)
{
    FILE *file = fopen(sounds->path, "rb");
    char *source = NULL_POINTER;
    ubyte *bytes = NULL_POINTER;
    cJSON *root = NULL_POINTER;
    const cJSON *value = NULL_POINTER;
    const char *base64 = NULL_POINTER;
    const char *failed_entry = "catalog";
    sound_clip_t clips[DESKTOP_SOUND_COUNT] = { 0 };
    sound_clip_t previous[DESKTOP_SOUND_COUNT] = { 0 };
    long length = 0;
    size_t size = 0;
    size_t encoded_size = 0;
    int index = 0;
    boole valid = FALSE;
    if (!file)
    {
        return FALSE;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return FALSE;
    }
    source = calloc((size_t)length + 1, 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length)
    {
        fclose(file);
        free(source);
        return FALSE;
    }
    fclose(file);
    if (sounds->source && clib__strcmp(source, sounds->source) == 0)
    {
        free(source);
        return sounds->source_valid;
    }
    root = cJSON_Parse(source);
    valid = cJSON_IsObject(root);
    for (index = 0; valid && index < DESKTOP_SOUND_COUNT; index++)
    {
        failed_entry = g_sound_names[index];
        value = cJSON_GetObjectItemCaseSensitive(root, g_sound_names[index]);
        valid = cJSON_IsString(value);
        if (!valid || !value->valuestring[0])
        {
            continue;
        }
        base64 = value->valuestring;
        if (clib__strncmp(base64, "data:", 5) == 0)
        {
            base64 = clib__strstr(base64, ";base64,");
            if (!base64)
            {
                valid = FALSE;
                break;
            }
            base64 += 8;
        }
        encoded_size = clib__strlen(base64);
        size = encoded_size / 4 * 3 + 3;
        bytes = malloc(size);
        valid = bytes && mbedtls_base64_decode(bytes, size, &size, (const ubyte *)base64, encoded_size) == 0;
        if (valid)
        {
            valid = _sounds_internal__decode(&clips[index], bytes, size);
        }
        free(bytes);
        bytes = NULL;
    }
    cJSON_Delete(root);
    if (valid)
    {
        desktop_mutex__lock(sounds->mutex);
        clib__memcpy(previous, sounds->clips, sizeof(previous));
        clib__memcpy(sounds->clips, clips, sizeof(clips));
        clib__null_memory(sounds->playing, sizeof(sounds->playing));
        desktop_mutex__unlock(sounds->mutex);
        for (index = 0; index < DESKTOP_SOUND_COUNT; index++)
        {
            ma_free(previous[index].pcm, NULL);
        }
    }
    else
    {
        for (index = 0; index < DESKTOP_SOUND_COUNT; index++)
        {
            ma_free(clips[index].pcm, NULL);
        }
        fprintf(stderr, "Could not reload %s: invalid JSON, Base64 or audio; previous sounds remain active (entry %s).\n", sounds->path, failed_entry);
    }
    // Remember even a rejected edit, so it reports once until the file changes again.
    free(sounds->source);
    sounds->source = source;
    sounds->source_valid = valid;
    return valid;
}

static void _sounds_internal__watch(void *context)
{
    desktop_sounds_t *sounds = context;
    uint64 next = 0;
    boole stop = FALSE;
    for (;;)
    {
        desktop_mutex__lock(sounds->mutex);
        stop = sounds->stop;
        desktop_mutex__unlock(sounds->mutex);
        if (stop)
        {
            return;
        }
        if (desktop_system__milliseconds() >= next)
        {
            desktop_sounds__reload(sounds);
            next = desktop_system__milliseconds() + 1000;
        }
        desktop_system__sleep(100);
    }
}

// Runtime Base64 audio is loaded from a text file. The optional watcher reloads edits.
desktop_sounds_t *desktop_sounds__create(const char *path, boole watch)
{
    desktop_sounds_t *sounds = calloc(1, sizeof(*sounds));
    if (!sounds)
    {
        return NULL;
    }
    sounds->mutex = desktop_mutex__create();
    sounds->path = calloc(clib__strlen(path) + 1, 1);
    if (!sounds->mutex || !sounds->path)
    {
        desktop_sounds__destroy(sounds);
        return NULL;
    }
    clib__string_copy(sounds->path, clib__strlen(path) + 1, path);
    if (!desktop_sounds__reload(sounds) && !sounds->source)
    {
        fprintf(stderr, "Notification sound file is unavailable: %s\n", path);
    }
    if (watch)
    {
        sounds->watcher = desktop_thread__start(_sounds_internal__watch, sounds);
        if (!sounds->watcher)
        {
            fprintf(stderr, "Could not start the notification sound watcher for %s\n", path);
        }
    }
    return sounds;
}

void desktop_sounds__destroy(desktop_sounds_t *sounds)
{
    int index = 0;
    if (!sounds)
    {
        return;
    }
    if (sounds->mutex)
    {
        desktop_mutex__lock(sounds->mutex);
        sounds->stop = TRUE;
        desktop_mutex__unlock(sounds->mutex);
    }
    desktop_thread__join(sounds->watcher);
    for (index = 0; index < DESKTOP_SOUND_COUNT; index++)
    {
        ma_free(sounds->clips[index].pcm, NULL);
    }
    desktop_mutex__destroy(sounds->mutex);
    free(sounds->path);
    free(sounds->source);
    free(sounds);
}

boole desktop_sounds__play(desktop_sounds_t *sounds, desktop_sound_t id)
{
    int index = 0;
    if (!sounds || id < 0 || id >= DESKTOP_SOUND_COUNT)
    {
        return FALSE;
    }
    desktop_mutex__lock(sounds->mutex);
    for (index = 0; index < SOUND_PLAYBACKS && sounds->clips[id].frames; index++)
    {
        if (!sounds->playing[index].active)
        {
            sounds->playing[index].id = id;
            sounds->playing[index].position = 0;
            sounds->playing[index].active = TRUE;
            desktop_mutex__unlock(sounds->mutex);
            return TRUE;
        }
    }
    desktop_mutex__unlock(sounds->mutex);
    return FALSE;
}

void desktop_sounds__stop(desktop_sounds_t *sounds, desktop_sound_t id)
{
    int index = 0;
    if (!sounds)
    {
        return;
    }
    desktop_mutex__lock(sounds->mutex);
    for (index = 0; index < SOUND_PLAYBACKS; index++)
    {
        if (sounds->playing[index].id == id)
        {
            sounds->playing[index].active = FALSE;
        }
    }
    desktop_mutex__unlock(sounds->mutex);
}

void desktop_sounds__clear(desktop_sounds_t *sounds)
{
    if (sounds)
    {
        desktop_mutex__lock(sounds->mutex);
        clib__null_memory(sounds->playing, sizeof(sounds->playing));
        desktop_mutex__unlock(sounds->mutex);
    }
}

// Called from the existing speaker callback: no decoding, allocation or file access.
void desktop_sounds__mix(desktop_sounds_t *sounds, float *stereo, uint64 frames, int64 volume)
{
    int index = 0;
    uint64 frame = 0;
    float value = 0;
    float gain = (float)volume / 100.0f;
    sound_playback_t *playback = NULL_POINTER;
    sound_clip_t *clip = NULL_POINTER;
    if (!sounds || !desktop_mutex__try_lock(sounds->mutex))
    {
        return;
    }
    for (index = 0; index < SOUND_PLAYBACKS; index++)
    {
        playback = &sounds->playing[index];
        if (!playback->active)
        {
            continue;
        }
        clip = &sounds->clips[playback->id];
        for (frame = 0; frame < frames && playback->position < clip->frames; frame++)
        {
            value = clip->pcm[playback->position++] * gain;
            stereo[frame * 2] += value;
            stereo[frame * 2 + 1] += value;
        }
        playback->active = playback->position < clip->frames;
    }
    for (frame = 0; frame < frames * 2; frame++)
    {
        if (stereo[frame] > 1.0f)
        {
            stereo[frame] = 1.0f;
        }
        else if (stereo[frame] < -1.0f)
        {
            stereo[frame] = -1.0f;
        }
    }
    desktop_mutex__unlock(sounds->mutex);
}
