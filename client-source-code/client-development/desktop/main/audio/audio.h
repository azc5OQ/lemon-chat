#ifndef LEMON_DESKTOP_AUDIO_H
#define LEMON_DESKTOP_AUDIO_H

#include "clib.h"
#include "audio/devices.h"
#include "audio/sounds.h"

#define DESKTOP_AUDIO_PACKET 3838
#define DESKTOP_AUDIO_RATE 48000
#define DESKTOP_AUDIO_FRAME 960
#define DESKTOP_AUDIO_PACKET_AGE 100

typedef struct desktop_audio_options_t
{
    boole enabled;
    boole microphone;
    boole transmit;
    int64 volume;
    int64 gain;
    boole voice_activation;
    int64 activation_threshold;
    int64 voice_bitrate;
    char input_device[DESKTOP_DEVICE_KEY];
    char output_device[DESKTOP_DEVICE_KEY];
} desktop_audio_options_t;

typedef struct desktop_audio_status_t
{
    boole playback_ready;
    boole capture_ready;
    boole transmitting;
    boole voice_active;
    int64 input_level;
    uint64 rejected_packets;
    uint64 dropped_capture_frames;
    char detail[192];
} desktop_audio_status_t;

typedef struct desktop_audio_packet_t
{
    uint sender;
    int64 volume_offset;
    ushort sequence;
    boole music;
    uint64 created;
    uint64 size;
    ubyte data[DESKTOP_AUDIO_PACKET];
} desktop_audio_packet_t;

typedef struct desktop_audio_t desktop_audio_t;

desktop_audio_t *desktop_audio__create(boole use_devices);
void desktop_audio__destroy(desktop_audio_t *audio);
void desktop_audio__configure(desktop_audio_t *audio, const desktop_audio_options_t *options);
void desktop_audio__status(desktop_audio_t *audio, desktop_audio_status_t *status);
boole desktop_audio__devices(desktop_audio_t *audio, desktop_devices_view_t *view, boole refresh);
void desktop_audio__reset(desktop_audio_t *audio);
void desktop_audio__discard_music(desktop_audio_t *audio);
void desktop_audio__sound(desktop_audio_t *audio, desktop_sound_t id);
void desktop_audio__stop_sound(desktop_audio_t *audio, desktop_sound_t id);
void desktop_audio__notifications(desktop_audio_t *audio, boole enabled, int64 volume, const char *output);
void desktop_audio__stop_voice(desktop_audio_t *audio);
boole desktop_audio__receive(desktop_audio_t *audio, const desktop_audio_packet_t *packet);
boole desktop_audio__next_packet(desktop_audio_t *audio, desktop_audio_packet_t *packet);
void desktop_audio__capture(desktop_audio_t *audio, const float *mono, uint64 frames);
void desktop_audio__playback(desktop_audio_t *audio, float *stereo, uint64 frames);

#endif
