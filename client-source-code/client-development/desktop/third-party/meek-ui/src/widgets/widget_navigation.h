#ifndef WIDGET_NAVIGATION_H
#define WIDGET_NAVIGATION_H

#include "../gui.h"
#include "../gui_api.h"

// Existing child containers are retained pages. Their ids identify destinations;
// navigation_title labels them and navigation_parent describes breadcrumb ancestry.
GUI_API void widget_navigation__register(void);
GUI_API void scene__navigation_push(gui_node_t* anchor, char* page_id);
GUI_API void scene__navigation_pop(gui_node_t* anchor);
GUI_API char* scene__navigation_peek(gui_node_t* anchor);
// Go to an existing history entry, or push a destination not in that history.
GUI_API boole scene__navigation_go(gui_node_t* anchor, const char* page_id);
GUI_API int64 scene__navigation_depth(gui_node_t* anchor);
// Names match navigation-* CSS properties, with or without that prefix.
// A non-null value overrides UI/CSS; NULL removes the C override.
GUI_API boole scene__navigation_set_option(gui_node_t* anchor, const char* name, const char* value);
GUI_API boole scene__navigation_get_option(gui_node_t* anchor, const char* name, char* out, int64 capacity);
GUI_API boole scene__navigation_set_page(gui_node_t* anchor, const char* page_id, const char* title, const char* parent_id);
// Report active navigation transitions to diagnostic hosts.
GUI_API boole widget_navigation__active(gui_node_t* root);
// Internal frame integration; hosts use the normal scene/animator entry points.
void widget_navigation__prepare_tree(gui_node_t* root);
void widget_navigation__tick_tree(gui_node_t* root);
void widget_navigation__remember_focus(gui_node_t* node);
void widget_navigation__reconcile(gui_node_t* old, gui_node_t* current);

#endif
