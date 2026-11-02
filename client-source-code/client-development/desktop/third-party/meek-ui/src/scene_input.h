// scene_input.h -- internal header for the input-state-machine
// subsystem split out of scene.c. Public input API (scene__on_*,
// scene__set_focus, etc.) stays on scene.h.
// scene_input__on_node_freed is called from scene.c's
// _scene_internal__on_node_freed so that a node being destroyed
// nulls out any focus/press/hover reference held inside this
// module's state.
#ifndef MEEK_UI_SCENE_INPUT_H
#define MEEK_UI_SCENE_INPUT_H

#include "gui.h"

void scene_input__on_node_freed(gui_node_t* n);

#endif
