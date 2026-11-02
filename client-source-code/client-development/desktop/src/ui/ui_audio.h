#ifndef LEMON_DESKTOP_UI_AUDIO_H
#define LEMON_DESKTOP_UI_AUDIO_H

#include "session/session.h"
#include "settings/settings.h"

void desktop_ui_audio__initialize(desktop_session_t* session, desktop_settings_t* settings, boole native_devices);
void desktop_ui_audio__apply(void);
void desktop_ui_audio__update(const desktop_session_view_t* view);
void desktop_ui_audio__tick(void);

#endif
