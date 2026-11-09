#ifndef LEMON_DESKTOP_SOUNDS_H
#define LEMON_DESKTOP_SOUNDS_H

#include "clib.h"

typedef enum desktop_sound_t
{
    DESKTOP_SOUND_CONNECTED,
    DESKTOP_SOUND_USER_ENTERED_YOUR_CHANNEL,
    DESKTOP_SOUND_USER_JOINED_YOUR_CHANNEL,
    DESKTOP_SOUND_CHANNEL_CREATED,
    DESKTOP_SOUND_CHANNEL_SWITCHED,
    DESKTOP_SOUND_CHANNEL_EDITED,
    DESKTOP_SOUND_CHANNEL_DELETED,
    DESKTOP_SOUND_USER_LEFT_YOUR_CHANNEL,
    DESKTOP_SOUND_USER_DISCONNECTED_FROM_YOUR_CHANNEL,
    DESKTOP_SOUND_POKE,
    DESKTOP_SOUND_MESSAGE_SENT,
    DESKTOP_SOUND_MESSAGE_RECEIVED,
    DESKTOP_SOUND_CALL,
    DESKTOP_SOUND_INSUFFICIENT_PERMISSIONS,
    DESKTOP_SOUND_CONNECTION_LOST,
    DESKTOP_SOUND_DISCONNECTED,
    DESKTOP_SOUND_ERROR,
    DESKTOP_SOUND_SERVER_GROUP_ASSIGNED,
    DESKTOP_SOUND_SERVER_GROUP_REVOKED,
    DESKTOP_SOUND_USER_WAS_MOVED_OUT_OF_YOUR_CHANNEL,
    DESKTOP_SOUND_USER_WAS_MOVED_TO_YOUR_CHANNEL,
    DESKTOP_SOUND_USER_IN_YOUR_CHANNEL_WAS_BANNED_FROM_THE_SERVER,
    DESKTOP_SOUND_USER_WAS_KICKED_TO_YOUR_CHANNEL,
    DESKTOP_SOUND_USER_WAS_KICKED_OUT_OF_YOUR_CHANNEL,
    DESKTOP_SOUND_YOU_EDITED_THE_SERVER,
    DESKTOP_SOUND_MICROPHONE_MUTED,
    DESKTOP_SOUND_MICROPHONE_ACTIVATED,
    DESKTOP_SOUND_YOU_WERE_KICKED_FROM_THE_SERVER,
    DESKTOP_SOUND_YOU_WERE_BANNED_FROM_THE_SERVER,
    DESKTOP_SOUND_COUNT
} desktop_sound_t;

typedef struct desktop_sounds_t desktop_sounds_t;

desktop_sounds_t *desktop_sounds__create(const char *path, boole watch);
boole desktop_sounds__reload(desktop_sounds_t *sounds);
void desktop_sounds__destroy(desktop_sounds_t *sounds);
boole desktop_sounds__play(desktop_sounds_t *sounds, desktop_sound_t id);
void desktop_sounds__stop(desktop_sounds_t *sounds, desktop_sound_t id);
void desktop_sounds__clear(desktop_sounds_t *sounds);
void desktop_sounds__mix(desktop_sounds_t *sounds, float *stereo, uint64 frames, int64 volume);

#endif
