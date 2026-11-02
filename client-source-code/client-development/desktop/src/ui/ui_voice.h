#ifndef LEMON_DESKTOP_UI_VOICE_H
#define LEMON_DESKTOP_UI_VOICE_H

#include "clib.h"

typedef struct gui_node_t gui_node_t;

void desktop_ui_voice__register(void);
void desktop_ui_voice__shutdown(void);
void desktop_ui_voice__append(gui_node_t* parent, int64 audio, boole muted);
void desktop_ui_voice__draw_channel(gui_node_t* node);

#endif
