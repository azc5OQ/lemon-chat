#ifndef LEMON_DESKTOP_UI_WINDOW_H
#define LEMON_DESKTOP_UI_WINDOW_H

#include "session/session.h"
#include "settings/settings.h"
#include "overlay_controller.h"

// Call these functions on the window thread. The caller owns all four objects.
// Keep session, view and settings alive through shutdown. The overlay must remain
// alive while actions or updates run; main stops it before freeing UI resources.
boole desktop_ui_window__initialize(desktop_session_t* session, desktop_session_view_t* view,
    desktop_settings_t* settings, desktop_overlay_controller_t* overlay, boole preview, boole native_devices);
void desktop_ui_window__apply_settings(void);
void desktop_ui_window__reload(void);
void desktop_ui_window__update(boole changed);
void desktop_ui_window__overlay(const desktop_overlay_snapshot_t* snapshot, const char* status);
boole desktop_ui_window__preview(void);
void desktop_ui_window__shutdown(void);

#endif
