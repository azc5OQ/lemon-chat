#ifndef LEMON_DESKTOP_UI_LAYOUT_H
#define LEMON_DESKTOP_UI_LAYOUT_H

typedef struct gui_node_t gui_node_t;

void desktop_ui_layout__register(void);
void desktop_ui_layout__raise(gui_node_t* node);

#endif
