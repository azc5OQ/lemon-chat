#ifndef MEEK_BORDER_H
#define MEEK_BORDER_H

#include "meek_ui_config.h"
#include "gui.h"

/** Paints only the border ring, preserving backgrounds and transparent centers. */
GUI_API void border__draw(gui_rect_t bounds, float radius, float width, gui_border_style_e style,
    gui_color_t color, const gui_gradient_t* gradient, float scale, float opacity);

#endif
