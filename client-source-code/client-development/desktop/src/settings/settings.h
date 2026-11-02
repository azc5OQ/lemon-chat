#ifndef LEMON_DESKTOP_SETTINGS_H
#define LEMON_DESKTOP_SETTINGS_H

#include "clib.h"
#include "audio/devices.h"
#include "attachments/attachments.h"

#define DESKTOP_GAME_NAME_CAPACITY 260
#define DESKTOP_BOOKMARK_LIMIT 16
#define DESKTOP_THEME_NAME_CAPACITY 64

typedef struct desktop_bookmark_t
{
    char name[64];
    char address[256];
} desktop_bookmark_t;

typedef struct desktop_settings_t
{
    boole overlay_enabled;
    char game_executable[DESKTOP_GAME_NAME_CAPACITY];
    char address[1024];
    char username[101];
    char identity[513];
    char server_keys[2049];
    int64 key_bits;
    boole audio_enabled;
    int64 audio_volume;
    int64 microphone_gain;
    int64 push_to_talk_key;
    boole voice_activation;
    int64 activation_threshold;
    int64 voice_bitrate;
    char input_device[DESKTOP_DEVICE_KEY];
    char output_device[DESKTOP_DEVICE_KEY];
    desktop_chat_preferences_t chat;
    char theme[DESKTOP_THEME_NAME_CAPACITY];
    int64 bookmark_count;
    desktop_bookmark_t bookmarks[DESKTOP_BOOKMARK_LIMIT];
} desktop_settings_t;

boole desktop_settings__valid_game(const char* executable);
boole desktop_settings__valid_theme(const char* name);
boole desktop_settings__valid_bookmark(const desktop_bookmark_t* bookmark);
void desktop_settings__load(desktop_settings_t* settings);
boole desktop_settings__save(const desktop_settings_t* settings);

#endif
