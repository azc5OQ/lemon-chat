#ifndef LEMON_DESKTOP_UI_ACTIONS_H
#define LEMON_DESKTOP_UI_ACTIONS_H

#include "session/session.h"
#include "settings/settings.h"

void desktop_ui_actions__initialize(desktop_session_t *session, const desktop_session_view_t *view, desktop_settings_t *settings);
void desktop_ui_actions__select(const desktop_client_t *member);
void desktop_ui_actions__update(void);
void desktop_ui_actions__activate(const char *id);
void desktop_ui_actions__shutdown(void);

#endif
