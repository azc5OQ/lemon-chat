#ifndef LEMON_DESKTOP_UI_EFFECTS_H
#define LEMON_DESKTOP_UI_EFFECTS_H

struct gui_node_t;

void desktop_ui_effects__register(void);
void desktop_ui_effects__border(struct gui_node_t *node, float scale);

#endif
