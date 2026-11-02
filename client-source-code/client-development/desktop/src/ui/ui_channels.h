#ifndef LEMON_DESKTOP_UI_CHANNELS_H
#define LEMON_DESKTOP_UI_CHANNELS_H

#include "session/session.h"

void desktop_ui_channels__initialize(desktop_session_t* session, const desktop_session_view_t* view);
void desktop_ui_channels__update(void);
void desktop_ui_channels__open(int64 channel, desktop_channel_action_e action);

#endif
