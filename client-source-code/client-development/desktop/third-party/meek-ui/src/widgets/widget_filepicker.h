#ifndef WIDGET_FILEPICKER_H
#define WIDGET_FILEPICKER_H

// widget_filepicker.h - public API for the <filepicker> widget, a
// directory browser. .ui:
// <filepicker id="open" path="assets" filter=".png;.jpg" show-hidden="0" on_change="on_open_pick" />
// .style: size it and give it overflow-y: auto for scrolling.
// handler.c:
// UI_HANDLER void on_open_pick(gui_event_t* ev)
// {
// if (ev->file.is_dir == FALSE) { load(ev->file.path); }
// }
// A click on a directory enters it (".." goes up), a click on a file
// selects it; both fire on_change with the full path in ev->file.path
// (valid during the call; copy it to keep it) and ev->file.is_dir.
// A relative `path` attribute is resolved against the directory of the
// .ui file. From C, build the node with
// scene__node_new(GUI_NODE_TYPE__FILEPICKER) and call the functions
// below.

#include "types.h"
#include "gui.h"
#include "gui_api.h"

/**
 * @brief show a directory. The listing is read on the next layout.
 *
 * @param gui_node_t* n -> A filepicker node.
 * @param char* path -> Directory path, absolute or relative to the working directory; copied.
 *
 * @return boole -> FALSE when n is not a filepicker or the path does not fit.
 */
GUI_API boole       widget_filepicker__set_path(gui_node_t* n, char* path);

/**
 * @brief the directory currently shown.
 *
 * @param gui_node_t* n -> A filepicker node.
 *
 * @return const char* -> The path, owned by the node; NULL when n is not a filepicker.
 */
GUI_API const char* widget_filepicker__path(gui_node_t* n);

/**
 * @brief the last file the user picked.
 *
 * @param gui_node_t* n -> A filepicker node.
 *
 * @return const char* -> Its full path, owned by the node, or NULL when nothing was picked yet.
 */
GUI_API const char* widget_filepicker__selected_path(gui_node_t* n);

/**
 * @brief read the directory again on the next layout, for a host that changed files.
 *
 * @param gui_node_t* n -> A filepicker node.
 *
 * @return void
 */
GUI_API void        widget_filepicker__refresh(gui_node_t* n);

#endif
