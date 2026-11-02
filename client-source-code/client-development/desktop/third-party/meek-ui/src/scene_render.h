// scene_render.h -- internal header for the draw-emission subsystem
// split out of scene.c. Public render API (scene__emit_default_bg,
// scene__emit_border, scene__submit_fitted_image, scene__border_width)
// stays on scene.h.
// scene_render__emit_tree is called from scene__emit_draws in scene.c
// after it seeds the root's effective_opacity.
#ifndef MEEK_UI_SCENE_RENDER_H
#define MEEK_UI_SCENE_RENDER_H

#include "gui.h"

void scene_render__emit_tree(gui_node_t* root, float scale);

int64 scene_render__paint_key(gui_node_t* n);

#endif
