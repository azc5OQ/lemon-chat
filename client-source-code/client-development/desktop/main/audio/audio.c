#include "platform/memory.h"
// Device callbacks only move bounded PCM. A worker owns codecs, mixing and device lifetime.
#include <stdlib.h>
#include "audio/audio.h"
#include "audio/activity.h"
#include "audio/devices_native.h"
#include "platform/thread.h"
#include "platform/system.h"
#include "clib.h"
#include "opus.h"
#include "miniaudio.h"

#define AUDIO_RING_FRAMES 7680
#define AUDIO_PEERS 16
#define AUDIO_PEER_FRAMES 5760
#define AUDIO_MUSIC_FRAMES (DESKTOP_AUDIO_RATE * 9 / 2)
#define AUDIO_MUSIC_START_FRAMES DESKTOP_AUDIO_RATE
#define AUDIO_INCOMING 256
#define AUDIO_OUTGOING 16

typedef struct audio_peer_t
{
    OpusDecoder *decoder;
    uint id;
    ushort sequence;
    uint64 last_received;
    boole active;
    boole music;
    boole playing;
    int64 concealed;
    float gain;
    uint64 head;
    uint64 count;
    float *music_pcm;
    float pcm[AUDIO_PEER_FRAMES * 2];
} audio_peer_t;

struct desktop_audio_t
{
    desktop_mutex_t *mutex;
    desktop_thread_t *thread;
    desktop_devices_t *device_list;
    desktop_sounds_t *sounds;
    boole notifications;
    int64 notification_volume;
    boole devices;
    boole stop;
    uint64 reset;
    uint64 music_reset;
    uint64 transmit_until;
    desktop_audio_options_t options;
    desktop_audio_status_t status;
    desktop_audio_packet_t incoming[AUDIO_INCOMING];
    uint64 incoming_head;
    uint64 incoming_count;
    desktop_audio_packet_t outgoing[AUDIO_OUTGOING];
    uint64 outgoing_head;
    uint64 outgoing_count;
    float capture[AUDIO_RING_FRAMES];
    uint64 capture_head;
    uint64 capture_count;
    float playback[AUDIO_RING_FRAMES * 2];
    uint64 playback_head;
    uint64 playback_count;
};

// declarations
static void _audio_internal__worker(void *context);
static void _audio_internal__discard_capture(desktop_audio_t *audio);
static void _audio_internal__callback(ma_device *device, void *output, const void *input, ma_uint32 frames);
static boole _audio_internal__device(desktop_audio_t *audio, ma_device *device, boole capture, const char *key);
static void _audio_internal__detail(desktop_audio_t *audio, const char *text);
static void _audio_internal__append(audio_peer_t *peer, const float *pcm, uint64 frames);
static void _audio_internal__clear_peer(audio_peer_t *peer);
static void _audio_internal__clear_music(audio_peer_t *peers);
static void _audio_internal__decode(desktop_audio_t *audio, audio_peer_t *peers, const desktop_audio_packet_t *packet, float *scratch);
static void _audio_internal__mix(audio_peer_t *peers, float *mixed, int64 volume, float *clip_memory);

/**
 * @brief creates a bounded audio engine; devices remain closed until enabled.
 * @param boole use_devices -> 1 for native devices, 0 for deterministic injected PCM checks
 * @return desktop_audio_t* -> owned engine, or NULL on allocation/thread failure
 */
desktop_audio_t *desktop_audio__create(boole use_devices)
{
    desktop_audio_t *audio = calloc(1, sizeof(*audio));
    if (!audio)
    {
        return NULL;
    }
    audio->mutex = desktop_mutex__create();
    audio->device_list = desktop_devices__create(use_devices);
    audio->sounds = desktop_sounds__create("assets/sounds.json", use_devices);
    audio->devices = use_devices != 0;
    audio->options.volume = 100;
    audio->options.gain = 100;
    audio->options.voice_bitrate = 64;
    if (audio->mutex && audio->device_list)
    {
        audio->thread = desktop_thread__start(_audio_internal__worker, audio);
    }
    if (!audio->thread)
    {
        desktop_sounds__destroy(audio->sounds);
        desktop_mutex__destroy(audio->mutex);
        desktop_devices__destroy(audio->device_list);
        desktop_memory__erase(audio, sizeof(*audio));
        free(audio);
        return NULL;
    }
    return audio;
}

/**
 * @brief joins the device/codec worker before releasing callback-visible storage.
 * @param desktop_audio_t* audio -> owned engine, or NULL
 * @return void
 */
void desktop_audio__destroy(desktop_audio_t *audio)
{
    if (!audio)
    {
        return;
    }
    desktop_mutex__lock(audio->mutex);
    audio->stop = 1;
    desktop_mutex__unlock(audio->mutex);
    desktop_thread__join(audio->thread);
    desktop_devices__destroy(audio->device_list);
    desktop_sounds__destroy(audio->sounds);
    desktop_mutex__destroy(audio->mutex);
    clib__null_memory(audio, sizeof(*audio));
    desktop_memory__erase(audio, sizeof(*audio));
    free(audio);
}

/**
 * @brief applies validated local audio preferences; disabling transmission immediately discards captured PCM.
 * @param desktop_audio_t* audio -> owned engine
 * @param const desktop_audio_options_t* options -> copied preferences, volume 0..100 and gain 0..200
 * @return void
 */
void desktop_audio__configure(desktop_audio_t *audio, const desktop_audio_options_t *options)
{
    desktop_audio_options_t checked = *options;
    boole devices_changed = FALSE;
    if (!clib__memchr(checked.input_device, 0, sizeof(checked.input_device)) || !clib__memchr(checked.output_device, 0, sizeof(checked.output_device)))
    {
        checked.enabled = FALSE;
        clib__null_memory(checked.input_device, sizeof(checked.input_device));
        clib__null_memory(checked.output_device, sizeof(checked.output_device));
    }
    checked.enabled = checked.enabled != 0;
    checked.microphone = checked.enabled && checked.microphone;
    checked.transmit = checked.microphone && checked.transmit;
    checked.voice_activation = checked.microphone && checked.voice_activation;
    checked.activation_threshold = checked.activation_threshold >= -60 && checked.activation_threshold <= -10 ? checked.activation_threshold : -45;
    checked.voice_bitrate = checked.voice_bitrate >= 32 && checked.voice_bitrate <= 128 ? checked.voice_bitrate : 64;
    checked.volume = checked.volume < 0 ? 0 : (checked.volume > 100 ? 100 : checked.volume);
    checked.gain = checked.gain < 0 ? 0 : (checked.gain > 200 ? 200 : checked.gain);
    desktop_mutex__lock(audio->mutex);
    devices_changed = clib__strcmp(checked.input_device, audio->options.input_device) != 0 || clib__strcmp(checked.output_device, audio->options.output_device) != 0;
    if (devices_changed)
    {
        audio->reset++;
        _audio_internal__discard_capture(audio);
        audio->playback_count = 0;
        audio->playback_head = 0;
        audio->incoming_count = 0;
        audio->incoming_head = 0;
        audio->status.voice_active = FALSE;
        audio->status.transmitting = FALSE;
        audio->status.capture_ready = FALSE;
        audio->status.input_level = -90;
        clib__null_memory(audio->incoming, sizeof(audio->incoming));
        clib__null_memory(audio->playback, sizeof(audio->playback));
    }
    audio->transmit_until = checked.transmit || checked.voice_activation ? desktop_system__milliseconds() + 200 : 0;
    if (checked.voice_activation != audio->options.voice_activation)
    {
        audio->reset++;
        _audio_internal__discard_capture(audio);
        audio->status.voice_active = FALSE;
    }
    if (!checked.transmit && !checked.voice_activation)
    {
        _audio_internal__discard_capture(audio);
        audio->status.voice_active = FALSE;
        audio->status.transmitting = FALSE;
        audio->status.input_level = -90;
    }
    if (!checked.enabled && audio->options.enabled)
    {
        audio->reset++;
        audio->playback_count = 0;
        audio->playback_head = 0;
        audio->incoming_count = 0;
        audio->incoming_head = 0;
        clib__null_memory(audio->incoming, sizeof(audio->incoming));
        clib__null_memory(audio->playback, sizeof(audio->playback));
    }
    audio->options = checked;
    desktop_mutex__unlock(audio->mutex);
    if (devices_changed && (checked.input_device[0] || checked.output_device[0]))
    {
        desktop_devices__view(audio->device_list, NULL, TRUE);
    }
}

/**
 * @brief obtains device choices independently of connection state and codec work.
 * @param desktop_audio_t* audio -> live engine
 * @param desktop_devices_view_t* view -> caller-owned snapshot
 * @param boole refresh -> TRUE to request enumeration without opening streams
 * @return boole -> TRUE when the snapshot changed
 */
boole desktop_audio__devices(desktop_audio_t *audio, desktop_devices_view_t *view, boole refresh)
{
    return desktop_devices__view(audio->device_list, view, refresh);
}

/**
 * @brief discards unsent microphone data while the audio mutex is held.
 * @param desktop_audio_t* audio -> locked audio engine
 * @return void
 */
static void _audio_internal__discard_capture(desktop_audio_t *audio)
{
    clib__null_memory(audio->capture, sizeof(audio->capture));
    audio->capture_head = 0;
    audio->capture_count = 0;
    clib__null_memory(audio->outgoing, sizeof(audio->outgoing));
    audio->outgoing_head = 0;
    audio->outgoing_count = 0;
}

/**
 * @brief copies a coherent device status without exposing any audio buffers.
 * @param desktop_audio_t* audio -> owned engine
 * @param desktop_audio_status_t* status -> destination
 * @return void
 */
void desktop_audio__status(desktop_audio_t *audio, desktop_audio_status_t *status)
{
    desktop_mutex__lock(audio->mutex);
    *status = audio->status;
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief drops audio from the old channel before another channel can become audible.
 * @param desktop_audio_t* audio -> owned engine
 * @return void
 */
void desktop_audio__reset(desktop_audio_t *audio)
{
    desktop_mutex__lock(audio->mutex);
    audio->reset++;
    audio->incoming_count = 0;
    audio->outgoing_count = 0;
    audio->capture_count = 0;
    audio->playback_count = 0;
    audio->incoming_head = 0;
    audio->outgoing_head = 0;
    audio->capture_head = 0;
    audio->playback_head = 0;
    clib__null_memory(audio->capture, sizeof(audio->capture));
    clib__null_memory(audio->playback, sizeof(audio->playback));
    clib__null_memory(audio->incoming, sizeof(audio->incoming));
    clib__null_memory(audio->outgoing, sizeof(audio->outgoing));
    audio->status.voice_active = FALSE;
    audio->status.transmitting = FALSE;
    audio->status.input_level = -90;
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief queues one already authenticated Opus packet; network input cannot grow this queue.
 * @param desktop_audio_t* audio -> owned engine
 * @param const desktop_audio_packet_t* packet -> copied encoded frame
 * @return boole -> 1 when queued
 */
boole desktop_audio__receive(desktop_audio_t *audio, const desktop_audio_packet_t *packet)
{
    boole accepted = 0;
    if (!packet || !packet->size || packet->size > DESKTOP_AUDIO_PACKET)
    {
        return 0;
    }
    desktop_mutex__lock(audio->mutex);
    if (audio->options.enabled && audio->incoming_count < AUDIO_INCOMING)
    {
        audio->incoming[(audio->incoming_head + audio->incoming_count) % AUDIO_INCOMING] = *packet;
        audio->incoming_count++;
        accepted = 1;
    }
    else
    {
        audio->status.rejected_packets++;
    }
    desktop_mutex__unlock(audio->mutex);
    return accepted;
}

/**
 * @brief drops stopped music while retaining queued voice and microphone data.
 * @param desktop_audio_t* audio -> owned engine
 * @return void
 */
void desktop_audio__discard_music(desktop_audio_t *audio)
{
    uint64 index = 0;
    uint64 retained = 0;
    uint64 source = 0;
    uint64 destination = 0;

    desktop_mutex__lock(audio->mutex);
    audio->music_reset++;
    for (index = 0; index < audio->incoming_count; index++)
    {
        source = (audio->incoming_head + index) % AUDIO_INCOMING;
        if (!audio->incoming[source].music)
        {
            destination = (audio->incoming_head + retained) % AUDIO_INCOMING;
            audio->incoming[destination] = audio->incoming[source];
            retained++;
        }
    }
    for (index = retained; index < audio->incoming_count; index++)
    {
        source = (audio->incoming_head + index) % AUDIO_INCOMING;
        clib__null_memory(&audio->incoming[source], sizeof(audio->incoming[source]));
    }
    audio->incoming_count = retained;

    // Already mixed output cannot be separated back into its senders.
    audio->playback_head = 0;
    audio->playback_count = 0;
    clib__null_memory(audio->playback, sizeof(audio->playback));
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief transfers one freshly encoded microphone packet to the network worker.
 * @param desktop_audio_t* audio -> owned engine
 * @param desktop_audio_packet_t* packet -> cleared output
 * @return boole -> 1 when available
 */
boole desktop_audio__next_packet(desktop_audio_t *audio, desktop_audio_packet_t *packet)
{
    boole available = 0;
    uint64 now = desktop_system__milliseconds();
    clib__null_memory(packet, sizeof(*packet));
    desktop_mutex__lock(audio->mutex);
    while (audio->outgoing_count && audio->options.transmit && now < audio->transmit_until && (!audio->options.voice_activation || audio->status.voice_active))
    {
        *packet = audio->outgoing[audio->outgoing_head];
        clib__null_memory(&audio->outgoing[audio->outgoing_head], sizeof(*packet));
        audio->outgoing_head = (audio->outgoing_head + 1) % AUDIO_OUTGOING;
        audio->outgoing_count--;
        if (now - packet->created < DESKTOP_AUDIO_PACKET_AGE)
        {
            available = TRUE;
            break;
        }
        clib__null_memory(packet, sizeof(*packet));
    }
    desktop_mutex__unlock(audio->mutex);
    return available;
}

/**
 * @brief copies microphone PCM without blocking or allocating in the device callback.
 * @param desktop_audio_t* audio -> engine retained while the device runs
 * @param const float* mono -> 48 kHz mono input
 * @param uint64 frames -> sample count
 * @return void
 */
void desktop_audio__capture(desktop_audio_t *audio, const float *mono, uint64 frames)
{
    uint64 index = 0;
    if (!mono || !frames || frames > AUDIO_RING_FRAMES || !desktop_mutex__try_lock(audio->mutex))
    {
        return;
    }
    if ((audio->options.transmit || audio->options.voice_activation) && desktop_system__milliseconds() < audio->transmit_until)
    {
        if (frames > AUDIO_RING_FRAMES - audio->capture_count)
        {
            audio->status.dropped_capture_frames += (int64)audio->capture_count;
            audio->capture_head = 0;
            audio->capture_count = 0;
        }
        for (index = 0; index < frames; index++)
        {
            audio->capture[(audio->capture_head + audio->capture_count + index) % AUDIO_RING_FRAMES] = mono[index];
        }
        audio->capture_count += frames;
    }
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief supplies stereo PCM without waiting; contention and underruns produce silence.
 * @param desktop_audio_t* audio -> engine retained while the device runs
 * @param float* stereo -> output with frames times two floats
 * @param uint64 frames -> requested 48 kHz frame count
 * @return void
 */
void desktop_audio__playback(desktop_audio_t *audio, float *stereo, uint64 frames)
{
    uint64 index = 0;
    uint64 available = 0;
    uint64 position = 0;
    if (!stereo || frames > AUDIO_RING_FRAMES)
    {
        return;
    }
    clib__null_memory(stereo, frames * 2 * sizeof(float));
    if (!desktop_mutex__try_lock(audio->mutex))
    {
        return;
    }
    available = frames < audio->playback_count ? frames : audio->playback_count;
    for (index = 0; index < available; index++)
    {
        position = (audio->playback_head + index) % AUDIO_RING_FRAMES;
        stereo[index * 2] = audio->playback[position * 2];
        stereo[index * 2 + 1] = audio->playback[position * 2 + 1];
        audio->playback[position * 2] = 0;
        audio->playback[position * 2 + 1] = 0;
    }
    audio->playback_head = (audio->playback_head + available) % AUDIO_RING_FRAMES;
    audio->playback_count -= available;
    desktop_sounds__mix(audio->sounds, stereo, frames, audio->notification_volume);
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief adapts miniaudio callbacks to the bounded PCM queues in chunks.
 * @param ma_device* device -> engine-owned capture or playback device
 * @param void* output -> stereo float output, or NULL
 * @param const void* input -> mono float input, or NULL
 * @param ma_uint32 frames -> requested frame count
 * @return void
 */
static void _audio_internal__callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
    desktop_audio_t *audio = device->pUserData;
    uint64 offset = 0;
    uint64 count = 0;
    while (offset < frames)
    {
        count = frames - offset;
        if (count > DESKTOP_AUDIO_FRAME)
        {
            count = DESKTOP_AUDIO_FRAME;
        }
        if (output)
        {
            desktop_audio__playback(audio, (float *)output + offset * 2, count);
        }
        if (input)
        {
            desktop_audio__capture(audio, (const float *)input + offset, count);
        }
        offset += count;
    }
}

/**
 * @brief opens the selected native device on the audio worker, never on the GUI thread.
 * @param desktop_audio_t* audio -> owned engine
 * @param ma_device* device -> zeroed device storage retained until uninit
 * @param boole capture -> TRUE for microphone, FALSE for speakers
 * @param const char* key -> saved device key; empty follows the system default
 * @return boole -> 1 after successful initialization and start
 */
static boole _audio_internal__device(desktop_audio_t *audio, ma_device *device, boole capture, const char *key)
{
    ma_device_config configuration = ma_device_config_init(capture ? ma_device_type_capture : ma_device_type_playback);
    desktop_device_selection_t selection = { 0 };
    ma_result result = MA_ERROR;
    if (key[0] && !desktop_devices__resolve(audio->device_list, key, capture, &selection))
    {
        desktop_devices__view(audio->device_list, NULL, TRUE);
        return FALSE;
    }
    configuration.sampleRate = DESKTOP_AUDIO_RATE;
    configuration.periodSizeInMilliseconds = 10;
    configuration.capture.format = ma_format_f32;
    configuration.capture.channels = 1;
    configuration.playback.format = ma_format_f32;
    configuration.playback.channels = 2;
    configuration.dataCallback = _audio_internal__callback;
    configuration.pUserData = audio;
    if (key[0])
    {
        configuration.capture.pDeviceID = capture ? &selection.id : NULL;
        configuration.playback.pDeviceID = capture ? NULL : &selection.id;
        result = ma_device_init_ex(&selection.backend, 1, NULL, &configuration, device);
    }
    else
    {
        result = ma_device_init(NULL, &configuration, device);
    }
    if (result != MA_SUCCESS)
    {
        return 0;
    }
    if (ma_device_start(device) != MA_SUCCESS)
    {
        ma_device_uninit(device);
        return 0;
    }
    return 1;
}

/**
 * @brief publishes a device error or listening status.
 * @param desktop_audio_t* audio -> owned engine
 * @param const char* text -> status copied to bounded storage
 * @return void
 */
static void _audio_internal__detail(desktop_audio_t *audio, const char *text)
{
    desktop_mutex__lock(audio->mutex);
    clib__string_copy(audio->status.detail, sizeof(audio->status.detail), text);
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief clears one sender and releases its optional music buffer on the audio worker.
 * @param audio_peer_t* peer -> worker-owned speaker queue
 * @return void
 */
static void _audio_internal__clear_peer(audio_peer_t *peer)
{
    OpusDecoder *decoder = peer->decoder;

    if (peer->music_pcm)
    {
        desktop_memory__erase(peer->music_pcm, AUDIO_MUSIC_FRAMES * 2 * sizeof(float));
        free(peer->music_pcm);
    }
    clib__null_memory(peer, sizeof(*peer));
    peer->decoder = decoder;
    if (decoder)
    {
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    }
}

/**
 * @brief releases stopped music buffers without resetting other speakers.
 * @param audio_peer_t* peers -> worker-owned decoder pool
 * @return void
 */
static void _audio_internal__clear_music(audio_peer_t *peers)
{
    int64 index = 0;

    for (index = 0; index < AUDIO_PEERS; index++)
    {
        if (peers[index].music)
        {
            _audio_internal__clear_peer(&peers[index]);
        }
    }
}

/**
 * @brief retains the server's music lead while keeping ordinary voice buffers short.
 * @param audio_peer_t* peer -> worker-owned sender buffer
 * @param const float* pcm -> decoded stereo samples
 * @param uint64 frames -> decoded frame count
 * @return void
 */
static void _audio_internal__append(audio_peer_t *peer, const float *pcm, uint64 frames)
{
    uint64 index = 0;
    uint64 position = 0;
    uint64 discard = 0;
    uint64 capacity = peer->music ? AUDIO_MUSIC_FRAMES : AUDIO_PEER_FRAMES;
    float *output = peer->music ? peer->music_pcm : peer->pcm;

    if (!output || frames > capacity)
    {
        return;
    }
    if (frames > capacity - peer->count)
    {
        discard = frames - (capacity - peer->count);
        peer->head = (peer->head + discard) % capacity;
        peer->count -= discard;
    }
    for (index = 0; index < frames; index++)
    {
        position = (peer->head + peer->count + index) % capacity;
        output[position * 2] = pcm[index * 2];
        output[position * 2 + 1] = pcm[index * 2 + 1];
    }
    peer->count += frames;
}

/**
 * @brief validates and decodes one speaker's packet using its own bounded decoder state.
 * @param desktop_audio_t* audio -> engine used to publish rejection counts
 * @param audio_peer_t* peers -> worker-owned fixed decoder pool
 * @param const desktop_audio_packet_t* packet -> authenticated packet
 * @param float* scratch -> stereo storage for at least AUDIO_PEER_FRAMES frames
 * @return void
 */
static void _audio_internal__decode(desktop_audio_t *audio, audio_peer_t *peers, const desktop_audio_packet_t *packet, float *scratch)
{
    int64 index = 0;
    audio_peer_t *peer = NULL;
    int64 frames = 0;
    ushort delta = 0;
    uint64 now = desktop_system__milliseconds();
    for (index = 0; index < AUDIO_PEERS; index++)
    {
        if (peers[index].active && peers[index].id == packet->sender)
        {
            peer = &peers[index];
            break;
        }
    }
    if (!peer)
    {
        for (index = 0; index < AUDIO_PEERS; index++)
        {
            if (!peers[index].active || ((!peers[index].music || !peers[index].count) && now - peers[index].last_received > 2000))
            {
                peer = &peers[index];
                peer->active = 0;
                break;
            }
        }
    }
    if (!peer || !peer->decoder || opus_packet_get_nb_samples(packet->data, (opus_int32)packet->size, DESKTOP_AUDIO_RATE) > AUDIO_PEER_FRAMES)
    {
        goto label_rejected;
    }
    delta = (ushort)(packet->sequence - peer->sequence);
    if (peer->active && now - peer->last_received < 500 && (delta == 0 || delta > 32768))
    {
        goto label_rejected;
    }
    if (!peer->active || peer->music != (packet->music != FALSE) || delta >= 500 || (!packet->music && now - peer->last_received > 500))
    {
        _audio_internal__clear_peer(peer);
    }
    peer->music = packet->music != FALSE;
    if (peer->music && !peer->music_pcm)
    {
        peer->music_pcm = calloc(AUDIO_MUSIC_FRAMES * 2, sizeof(float));
        if (!peer->music_pcm)
        {
            goto label_rejected;
        }
    }
    frames = opus_decode_float(peer->decoder, packet->data, (opus_int32)packet->size, scratch, AUDIO_PEER_FRAMES, 0);
    if (frames <= 0)
    {
        goto label_rejected;
    }
    peer->id = packet->sender;
    peer->sequence = packet->sequence;
    peer->last_received = now;
    peer->active = 1;
    peer->concealed = 0;
    peer->gain = 1 + (float)(packet->volume_offset < -100 ? -100 : packet->volume_offset > 100 ? 100 : packet->volume_offset) / 100;
    _audio_internal__append(peer, scratch, (uint64)frames);
    return;
label_rejected:
    desktop_mutex__lock(audio->mutex);
    audio->status.rejected_packets++;
    desktop_mutex__unlock(audio->mutex);
}

/**
 * @brief mixes one 20 ms output interval, soft-clips peaks and conceals at most two brief underruns.
 * @param audio_peer_t* peers -> worker-owned decoder/PCM pool
 * @param float* mixed -> output for DESKTOP_AUDIO_FRAME stereo frames
 * @param int64 volume -> output volume from zero through 100
 * @param float* clip_memory -> worker-owned continuity state, one float per output channel
 * @return void
 */
static void _audio_internal__mix(audio_peer_t *peers, float *mixed, int64 volume, float *clip_memory)
{
    int64 member = 0;
    uint64 frame = 0;
    uint64 available = 0;
    uint64 position = 0;
    audio_peer_t *peer = NULL;
    const float *pcm = NULL;
    uint64 capacity = 0;
    float gain = (float)volume / 100.0f;
    float concealed[DESKTOP_AUDIO_FRAME * 2] = { 0 };
    uint64 now = desktop_system__milliseconds();
    clib__null_memory(mixed, DESKTOP_AUDIO_FRAME * 2 * sizeof(float));
    for (member = 0; member < AUDIO_PEERS; member++)
    {
        peer = &peers[member];
        if (!peer->active)
        {
            continue;
        }
        if (peer->music)
        {
            if (!peer->count && now - peer->last_received > 2000)
            {
                _audio_internal__clear_peer(peer);
                continue;
            }
            if (!peer->playing)
            {
                // A short final fragment must also drain when no more packets arrive.
                if (!peer->count || (peer->count < AUDIO_MUSIC_START_FRAMES && now - peer->last_received < 1000))
                {
                    continue;
                }
                peer->playing = TRUE;
            }
        }
        if (!peer->music && !peer->count && peer->concealed < 2 && now - peer->last_received < 120)
        {
            if (opus_decode_float(peer->decoder, NULL, 0, concealed, DESKTOP_AUDIO_FRAME, 0) == DESKTOP_AUDIO_FRAME)
            {
                _audio_internal__append(peer, concealed, DESKTOP_AUDIO_FRAME);
            }
            peer->concealed++;
        }
        capacity = peer->music ? AUDIO_MUSIC_FRAMES : AUDIO_PEER_FRAMES;
        pcm = peer->music ? peer->music_pcm : peer->pcm;
        available = peer->count < DESKTOP_AUDIO_FRAME ? peer->count : DESKTOP_AUDIO_FRAME;
        for (frame = 0; frame < available; frame++)
        {
            position = (peer->head + frame) % capacity;
            mixed[frame * 2] += pcm[position * 2] * peer->gain;
            mixed[frame * 2 + 1] += pcm[position * 2 + 1] * peer->gain;
        }
        peer->head = (peer->head + available) % capacity;
        peer->count -= available;
        if (peer->music && !peer->count)
        {
            peer->playing = FALSE;
        }
    }
    for (frame = 0; frame < DESKTOP_AUDIO_FRAME * 2; frame++)
    {
        mixed[frame] *= gain;
    }
    opus_pcm_soft_clip(mixed, DESKTOP_AUDIO_FRAME, 2, clip_memory);
}

/**
 * @brief owns every device transition and codec call outside the GUI and device callbacks.
 * @param void* context -> engine retained until this worker joins
 * @return void
 */
static void _audio_internal__worker(void *context)
{
    desktop_audio_t *audio = context;
    desktop_audio_options_t options = { 0 };
    audio_peer_t *peers = calloc(AUDIO_PEERS, sizeof(*peers));
    OpusEncoder *encoder = NULL;
    desktop_activity_t *activity = desktop_activity__create();
    ma_device playback = { 0 };
    ma_device capture = { 0 };
    desktop_audio_packet_t packet = { 0 };
    float mono[DESKTOP_AUDIO_FRAME] = { 0 };
    float mixed[DESKTOP_AUDIO_FRAME * 2] = { 0 };
    float decoded[AUDIO_PEER_FRAMES * 2] = { 0 };
    uint64 epoch = 0;
    uint64 observed = 0;
    uint64 music_epoch = 0;
    uint64 music_observed = 0;
    int64 index = 0;
    int64 batch = 0;
    ushort sequence = 0;
    uint64 playback_retry = 0;
    uint64 capture_retry = 0;
    uint64 frame = 0;
    uint64 position = 0;
    uint64 buffered = 0;
    int error = 0;
    boole stop = 0;
    boole have_packet = 0;
    boole playback_started = 0;
    boole notifications = FALSE;
    boole capture_started = 0;
    boole transmitting = 0;
    boole wanted = 0;
    boole voice_active = FALSE;
    uint64 captured_at = 0;
    int64 encoded = 0;
    int64 bitrate = 64;
    float gain = 1.0f;
    float clip_memory[2] = { 0 };
    char input_device[DESKTOP_DEVICE_KEY] = { 0 };
    char output_device[DESKTOP_DEVICE_KEY] = { 0 };

    encoder = opus_encoder_create(DESKTOP_AUDIO_RATE, 1, OPUS_APPLICATION_VOIP, &error);
    if (!peers || !encoder || !activity)
    {
        _audio_internal__detail(audio, "Audio codec initialization failed.");
        goto label_finished;
    }
    if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000)) != OPUS_OK || opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(8)) != OPUS_OK || opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) != OPUS_OK || opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK || opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK)
    {
        _audio_internal__detail(audio, "Audio encoder configuration failed.");
        goto label_finished;
    }
    for (index = 0; index < AUDIO_PEERS; index++)
    {
        peers[index].decoder = opus_decoder_create(DESKTOP_AUDIO_RATE, 2, &error);
        if (!peers[index].decoder)
        {
            _audio_internal__detail(audio, "Audio decoder initialization failed.");
            goto label_finished;
        }
    }
    while (!stop)
    {
        desktop_mutex__lock(audio->mutex);
        stop = audio->stop;
        if ((audio->options.transmit || audio->options.voice_activation) && desktop_system__milliseconds() >= audio->transmit_until)
        {
            // Protocol work stalled. Do not continue transmitting under an old push-to-talk decision.
            audio->options.transmit = FALSE;
            audio->options.voice_activation = FALSE;
            _audio_internal__discard_capture(audio);
        }
        options = audio->options;
        notifications = audio->notifications;
        observed = audio->reset;
        music_observed = audio->music_reset;
        desktop_mutex__unlock(audio->mutex);
        if (stop)
        {
            break;
        }
        if (options.voice_bitrate != bitrate)
        {
            if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE((opus_int32)(options.voice_bitrate * 1000))) != OPUS_OK)
            {
                _audio_internal__detail(audio, "Could not apply the requested voice bitrate.");
                goto label_finished;
            }
            bitrate = options.voice_bitrate;
        }
        if (epoch != observed)
        {
            for (index = 0; index < AUDIO_PEERS; index++)
            {
                _audio_internal__clear_peer(&peers[index]);
            }
            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
            clib__null_memory(clip_memory, sizeof(clip_memory));
            transmitting = 0;
            voice_active = FALSE;
            desktop_activity__reset(activity);
            epoch = observed;
        }
        if (music_epoch != music_observed)
        {
            _audio_internal__clear_music(peers);
            music_epoch = music_observed;
        }
        if (audio->devices)
        {
            if (clib__strcmp(output_device, options.output_device) != 0)
            {
                playback_retry = 0;
            }
            if (clib__strcmp(input_device, options.input_device) != 0)
            {
                capture_retry = 0;
            }
            if (playback_started && ((!options.enabled && !notifications) || !ma_device_is_started(&playback) || clib__strcmp(output_device, options.output_device) != 0))
            {
                ma_device_uninit(&playback);
                playback_started = 0;
                playback_retry = 0;
            }
            if (capture_started && (!options.microphone || !ma_device_is_started(&capture) || clib__strcmp(input_device, options.input_device) != 0))
            {
                ma_device_uninit(&capture);
                capture_started = 0;
                capture_retry = 0;
            }
            if ((options.enabled || notifications) && !playback_started && desktop_system__milliseconds() >= playback_retry)
            {
                playback_started = _audio_internal__device(audio, &playback, FALSE, options.output_device);
                clib__string_copy(output_device, sizeof(output_device), options.output_device);
                playback_retry = desktop_system__milliseconds() + 3000;
                _audio_internal__detail(audio, playback_started ? "Listening" : "Could not open the selected speakers. Check Audio settings or reconnect the device.");
            }
            if (options.microphone && !capture_started && desktop_system__milliseconds() >= capture_retry)
            {
                capture_started = _audio_internal__device(audio, &capture, TRUE, options.input_device);
                clib__string_copy(input_device, sizeof(input_device), options.input_device);
                capture_retry = desktop_system__milliseconds() + 3000;
                if (!capture_started)
                {
                    _audio_internal__detail(audio, "Could not open the selected microphone. Check Audio settings, device access or reconnect it.");
                }
            }
        }
        else
        {
            playback_started = options.enabled || notifications;
            capture_started = options.microphone;
        }
        wanted = (options.transmit || options.voice_activation) && capture_started;
        if (wanted && !transmitting)
        {
            sequence = (ushort)(sequence + 1000);
            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
            desktop_activity__reset(activity);
            captured_at = 0;
            voice_active = FALSE;
        }
        transmitting = wanted;
        if (!wanted || (captured_at && desktop_system__milliseconds() - captured_at > 250))
        {
            voice_active = FALSE;
        }
        desktop_mutex__lock(audio->mutex);
        audio->status.playback_ready = playback_started;
        audio->status.capture_ready = capture_started;
        audio->status.voice_active = voice_active;
        audio->status.transmitting = options.transmit && transmitting && (!options.voice_activation || voice_active);
        desktop_mutex__unlock(audio->mutex);
        for (batch = 0; batch < 16; batch++)
        {
            desktop_mutex__lock(audio->mutex);
            have_packet = audio->incoming_count > 0 && audio->music_reset == music_epoch;
            if (have_packet)
            {
                packet = audio->incoming[audio->incoming_head];
                clib__null_memory(&audio->incoming[audio->incoming_head], sizeof(packet));
                audio->incoming_head = (audio->incoming_head + 1) % AUDIO_INCOMING;
                audio->incoming_count--;
            }
            desktop_mutex__unlock(audio->mutex);
            if (!have_packet)
            {
                break;
            }
            _audio_internal__decode(audio, peers, &packet, decoded);
        }
        for (batch = 0; batch < 4 && transmitting; batch++)
        {
            desktop_mutex__lock(audio->mutex);
            buffered = audio->capture_count;
            if (buffered >= DESKTOP_AUDIO_FRAME && (audio->options.transmit || audio->options.voice_activation) && audio->reset == epoch)
            {
                for (frame = 0; frame < DESKTOP_AUDIO_FRAME; frame++)
                {
                    position = (audio->capture_head + frame) % AUDIO_RING_FRAMES;
                    mono[frame] = audio->capture[position];
                    audio->capture[position] = 0;
                }
                audio->capture_head = (audio->capture_head + DESKTOP_AUDIO_FRAME) % AUDIO_RING_FRAMES;
                audio->capture_count -= DESKTOP_AUDIO_FRAME;
            }
            else
            {
                buffered = 0;
            }
            desktop_mutex__unlock(audio->mutex);
            if (buffered < DESKTOP_AUDIO_FRAME)
            {
                break;
            }
            gain = (float)options.gain / 100.0f;
            for (frame = 0; frame < DESKTOP_AUDIO_FRAME; frame++)
            {
                mono[frame] *= gain;
                if (mono[frame] != mono[frame])
                {
                    mono[frame] = 0;
                }
                mono[frame] = mono[frame] < -1.0f ? -1.0f : (mono[frame] > 1.0f ? 1.0f : mono[frame]);
            }
            voice_active = desktop_activity__process(activity, mono, options.activation_threshold);
            captured_at = desktop_system__milliseconds();
            clib__null_memory(&packet, sizeof(packet));
            encoded = opus_encode_float(encoder, mono, DESKTOP_AUDIO_FRAME, packet.data, sizeof(packet.data));
            clib__null_memory(mono, sizeof(mono));
            if (encoded > 0)
            {
                packet.size = (uint64)encoded;
                packet.created = desktop_system__milliseconds();
                packet.sequence = ++sequence;
                desktop_mutex__lock(audio->mutex);
                if ((audio->options.transmit || audio->options.voice_activation) && audio->reset == epoch && desktop_system__milliseconds() < audio->transmit_until)
                {
                    // Retain 60 ms locally while silent; sending still requires speech and server permission.
                    while (audio->outgoing_count >= (options.voice_activation && !voice_active ? 3 : AUDIO_OUTGOING))
                    {
                        clib__null_memory(&audio->outgoing[audio->outgoing_head], sizeof(packet));
                        audio->outgoing_head = (audio->outgoing_head + 1) % AUDIO_OUTGOING;
                        audio->outgoing_count--;
                    }
                    audio->outgoing[(audio->outgoing_head + audio->outgoing_count) % AUDIO_OUTGOING] = packet;
                    audio->outgoing_count++;
                    audio->status.voice_active = voice_active;
                    audio->status.input_level = desktop_activity__level(activity);
                    audio->status.transmitting = audio->options.transmit && (!options.voice_activation || voice_active);
                }
                desktop_mutex__unlock(audio->mutex);
            }
        }
        for (batch = 0; batch < 2 && playback_started; batch++)
        {
            desktop_mutex__lock(audio->mutex);
            buffered = audio->playback_count;
            desktop_mutex__unlock(audio->mutex);
            if (buffered >= DESKTOP_AUDIO_FRAME * 2)
            {
                break;
            }
            _audio_internal__mix(peers, mixed, options.volume, clip_memory);
            desktop_mutex__lock(audio->mutex);
            if (audio->options.enabled && audio->reset == epoch && audio->music_reset == music_epoch && audio->playback_count <= AUDIO_RING_FRAMES - DESKTOP_AUDIO_FRAME)
            {
                for (frame = 0; frame < DESKTOP_AUDIO_FRAME; frame++)
                {
                    position = (audio->playback_head + audio->playback_count + frame) % AUDIO_RING_FRAMES;
                    audio->playback[position * 2] = mixed[frame * 2];
                    audio->playback[position * 2 + 1] = mixed[frame * 2 + 1];
                }
                audio->playback_count += DESKTOP_AUDIO_FRAME;
            }
            desktop_mutex__unlock(audio->mutex);
        }
        desktop_system__sleep(5);
    }
label_finished:
    if (audio->devices && capture_started)
    {
        ma_device_uninit(&capture);
    }
    if (audio->devices && playback_started)
    {
        ma_device_uninit(&playback);
    }
    opus_encoder_destroy(encoder);
    desktop_activity__destroy(activity);
    if (peers)
    {
        for (index = 0; index < AUDIO_PEERS; index++)
        {
            _audio_internal__clear_peer(&peers[index]);
            opus_decoder_destroy(peers[index].decoder);
        }
        clib__null_memory(peers, AUDIO_PEERS * sizeof(*peers));
        desktop_memory__erase(peers, AUDIO_PEERS * sizeof(*peers));
        free(peers);
    }
    clib__null_memory(mono, sizeof(mono));
    clib__null_memory(decoded, sizeof(decoded));
}

void desktop_audio__sound(desktop_audio_t *audio, desktop_sound_t id)
{
    if (!audio)
    {
        return;
    }
    desktop_mutex__lock(audio->mutex);
    if (audio->notifications)
    {
        desktop_sounds__play(audio->sounds, id);
    }
    desktop_mutex__unlock(audio->mutex);
}

void desktop_audio__stop_sound(desktop_audio_t *audio, desktop_sound_t id)
{
    if (!audio)
    {
        return;
    }
    desktop_mutex__lock(audio->mutex);
    desktop_sounds__stop(audio->sounds, id);
    desktop_mutex__unlock(audio->mutex);
}

void desktop_audio__notifications(desktop_audio_t *audio, boole enabled, int64 volume, const char *output)
{
    desktop_mutex__lock(audio->mutex);
    audio->notifications = enabled;
    audio->notification_volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    if (!enabled)
    {
        desktop_sounds__clear(audio->sounds);
    }
    if (clib__strcmp(audio->options.output_device, output) != 0)
    {
        audio->reset++;
        audio->playback_count = 0;
        audio->playback_head = 0;
        clib__string_copy(audio->options.output_device, sizeof(audio->options.output_device), output);
    }
    desktop_mutex__unlock(audio->mutex);
}

// Closing voice leaves the selected speakers available for notifications.
void desktop_audio__stop_voice(desktop_audio_t *audio)
{
    desktop_mutex__lock(audio->mutex);
    audio->options.enabled = FALSE;
    audio->options.microphone = FALSE;
    audio->options.transmit = FALSE;
    audio->options.voice_activation = FALSE;
    desktop_mutex__unlock(audio->mutex);
    desktop_audio__reset(audio);
}
