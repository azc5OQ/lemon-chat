#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "types.h"
#include "gui.h"

// Shared effect surfaces consume ordinary renderer submissions. Existing
// widget vtables keep emitting the same rectangles, images and glyphs.
boole compositor__active(void);
boole compositor__begin_node(gui_node_t* node, float scale);
void compositor__end_node(gui_node_t* node, float scale);
boole compositor__background(gui_node_t* node, float scale);
// Called only by the built-in div painter, inside its scissor. Reuse an
// isolated leaf's finished decoration when every raster input is unchanged.
boole compositor__reuse_decoration(gui_node_t* node, float scale);
void compositor__map_point(gui_node_t* node, float* x, float* y);

#endif
