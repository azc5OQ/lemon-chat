#ifndef LEMON_DESKTOP_UI_SERVER_H
#define LEMON_DESKTOP_UI_SERVER_H

#include "session/session.h"

void desktop_ui_server__initialize(desktop_session_t* session, const desktop_session_view_t* view);
void desktop_ui_server__update(void);

#endif
