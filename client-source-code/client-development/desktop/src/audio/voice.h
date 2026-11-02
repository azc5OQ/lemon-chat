#ifndef LEMON_DESKTOP_VOICE_H
#define LEMON_DESKTOP_VOICE_H

#include "clib.h"
#include "session/session.h"
#include "cJSON.h"

typedef struct desktop_voice_t desktop_voice_t;

desktop_voice_t* desktop_voice__create(boole devices);
void desktop_voice__destroy(desktop_voice_t* voice);
void desktop_voice__stop(desktop_voice_t* voice);
boole desktop_voice__devices(desktop_voice_t* voice, desktop_devices_view_t* view, boole refresh);
void desktop_voice__channel_changed(desktop_voice_t* voice);
void desktop_voice__tick(desktop_session_t* session);
void desktop_voice__message(desktop_session_t* session, const cJSON* message);

#endif
