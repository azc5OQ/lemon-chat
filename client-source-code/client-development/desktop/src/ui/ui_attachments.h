#ifndef LEMON_DESKTOP_UI_ATTACHMENTS_H
#define LEMON_DESKTOP_UI_ATTACHMENTS_H

#include "session/session.h"
#include "settings/settings.h"
struct gui_node_t;

void desktop_ui_attachments__initialize(desktop_session_t* session, desktop_session_view_t* view, desktop_settings_t* settings);
void desktop_ui_attachments__apply(void);
void desktop_ui_attachments__append(struct gui_node_t* parent, uint64 id);
void desktop_ui_attachments__shutdown(void);
void desktop_ui_images__initialize(desktop_attachments_t* attachments, desktop_chat_preferences_t* preferences);
void desktop_ui_images__append(struct gui_node_t* parent, const desktop_attachment_t* attachment);
void desktop_ui_images__refresh(const desktop_attachments_view_t* view);
void desktop_ui_images__shutdown(void);

#endif
