#ifndef COLOR_MIX_H
#define COLOR_MIX_H

#include "types.h"
#include "gui.h"

gui_color_t color_mix__mix(gui_color_t a, gui_color_t b, float weight_a, float weight_b, const char* space);

#endif
