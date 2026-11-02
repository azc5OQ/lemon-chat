#ifndef WIDGET_REGISTRY_H
#define WIDGET_REGISTRY_H

#include "types.h"
#include "gui_api.h"
#include "gui.h"
#include "widget.h"

// widget_registry.h - the type -> vtable map.
// paired with widget_registry.c. one entry per gui_node_type_e.

GUI_API void                 widget_registry__register(gui_node_type_e type, const widget_vtable_t* vt);

GUI_API const widget_vtable_t* widget_registry__get(gui_node_type_e type);

GUI_API boole                widget_registry__lookup_by_name(char* name, gui_node_type_e* out_type);

GUI_API char*                widget_registry__type_name(gui_node_type_e type);

GUI_API void                 widget_registry__bootstrap_builtins(void);

#endif
