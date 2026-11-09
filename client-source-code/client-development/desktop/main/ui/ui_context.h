#ifndef LEMON_DESKTOP_UI_CONTEXT_H
#define LEMON_DESKTOP_UI_CONTEXT_H

#include "../definitions.h"

#include "session/session.h"

void desktop_ui_context__initialize(desktop_session_t *session, const desktop_session_view_t *view, void (*show_info)(void), void (*join_channel)(int64));
void desktop_ui_context__open(const desktop_client_t *member, int64 x, int64 y);
void desktop_ui_context__channel(const desktop_channel_t *channel, int64 x, int64 y);
boole desktop_ui_context__tick(void);
void desktop_ui_context__close(void);

#endif
