#ifndef LEMON_DESKTOP_UI_CHAT_FORMAT_H
#define LEMON_DESKTOP_UI_CHAT_FORMAT_H

#include "ui/ui_headers.h"
#include "session/session.h"
#include "settings/settings.h"

void desktop_ui_chat_format__initialize(desktop_session_t *session, desktop_settings_t *settings);
void desktop_ui_chat_format__apply(void);
boole desktop_ui_chat_format__tick(void);
void desktop_ui_chat_format__message(gui_node_t *node, const desktop_chat_format_t *format);
void desktop_ui_chat_format__open_fonts(void);

#endif
