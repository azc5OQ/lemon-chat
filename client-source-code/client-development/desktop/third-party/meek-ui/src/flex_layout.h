#ifndef FLEX_LAYOUT_H
#define FLEX_LAYOUT_H

#include "types.h"
#include "gui.h"

boole flex_layout__is_row(gui_node_t* node);
void flex_layout__row(gui_node_t* node, float x, float y, float width, float height, float scale);
void flex_layout__column(gui_node_t* node, float x, float y, float width, float height, float scale);

#endif
