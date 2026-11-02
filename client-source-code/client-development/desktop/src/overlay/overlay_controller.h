#ifndef LEMON_DESKTOP_OVERLAY_CONTROLLER_H
#define LEMON_DESKTOP_OVERLAY_CONTROLLER_H

#include "clib.h"
#include "settings/settings.h"
#include "overlay_data.h"

typedef struct desktop_overlay_controller_t desktop_overlay_controller_t;

desktop_overlay_controller_t* desktop_overlay_controller__create(void);
void desktop_overlay_controller__destroy(desktop_overlay_controller_t* controller);
void desktop_overlay_controller__configure(desktop_overlay_controller_t* controller, const desktop_settings_t* settings);
void desktop_overlay_controller__publish(desktop_overlay_controller_t* controller, const desktop_overlay_snapshot_t* snapshot);
void desktop_overlay_controller__status(desktop_overlay_controller_t* controller, char* text, uint64 capacity);

#endif
